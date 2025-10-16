#include <Nodos/Plugin.hpp>

#include <nosTransfer/nosTransfer.h>

NOS_INIT()
NOS_BEGIN_IMPORT_DEPS()
NOS_END_IMPORT_DEPS()

namespace nos::transfer
{
struct CopyDestinationNode
{
	ObjectRef Destination;
	std::unordered_map<nos::Name, std::unique_ptr<CopyDestinationNode>> CompositeChildren;
	std::vector<std::unique_ptr<CopyDestinationNode>> ArrayChildren;
	bool IsArray() const { return !ArrayChildren.empty(); }
	bool IsComposite() const { return !CompositeChildren.empty(); }
	bool IsLeaf() const { return !IsArray() && !IsComposite(); }
};

struct Context
{
	nosResult RegisterCopyFunctions(nos::Name objectTypeName, const nosTransferCopyFunctions* funcs)
	{
		std::unique_lock lock(CopyMutex);
		if (CopyFunctions.contains(objectTypeName))
			return NOS_RESULT_INVALID_ARGUMENT;
		if (!funcs || (!funcs->CanCopy && !funcs->Copy))
			return NOS_RESULT_INVALID_ARGUMENT;
		CopyFunctions[objectTypeName] = *funcs;
		return NOS_RESULT_SUCCESS;
	}

	nosResult UnregisterCopyFunctions(nos::Name objectTypeName)
	{
		std::unique_lock lock(CopyMutex);
		auto it = CopyFunctions.find(objectTypeName);
		if (it == CopyFunctions.end())
			return NOS_RESULT_INVALID_ARGUMENT;
		CopyFunctions.erase(it);
		return NOS_RESULT_SUCCESS;
	}

	nosResult Copy(nosObjectId src, nosTransferCopyDestination dstSlot)
	{
		std::unique_lock lock(CopyMutex);
		nosName srcTypeName{}, dstTypeName{};
		if (nosEngine.ObjectAPI->GetObjectTypeName(src, &srcTypeName) != NOS_RESULT_SUCCESS)
			return NOS_RESULT_INVALID_ARGUMENT;
		auto copyDst = GetCopyDestination(dstSlot);
		if (!copyDst)
			return NOS_RESULT_INVALID_ARGUMENT;
		if (nosEngine.ObjectAPI->GetObjectTypeName(*copyDst, &dstTypeName) != NOS_RESULT_SUCCESS)
			return NOS_RESULT_INVALID_ARGUMENT;
		if (srcTypeName != dstTypeName)
			return NOS_RESULT_INVALID_ARGUMENT;
		
		auto it = CopyFunctions.find(srcTypeName);
		if (it == CopyFunctions.end())
			return CopyDefault(src, dstSlot); 
		lock.unlock();
		if (!it->second.Copy)
			return NOS_RESULT_NOT_IMPLEMENTED;
		auto dst = copyDst->GetObjectId();
		ObjectRef dstObj{};
		auto res = it->second.Copy(src, dst, &dstObj.GetStorage());
		if (res == NOS_RESULT_SUCCESS)
			Slots[dstSlot]->Destination = std::move(dstObj);
		return res;
	}

	ObjectRef TransferCopyRecursive(nosObjectId src, CopyDestinationNode& dstNode)
	{
		nosObjectKind kind{};
		if (NOS_RESULT_SUCCESS != nosEngine.ObjectAPI->GetObjectKind(src, &kind))
			return ObjectRef();
		ObjectRef copied;
		switch (kind)
		{
		case NOS_OBJECT_KIND_PRIMITIVE:
			{
				copied = src;
				break;
			}
		case NOS_OBJECT_KIND_COMPOSITE:
			{
				// If the composite object has any fields that contain foreign objects, we should call copy on them,
				// and the rest of the fields should be set to the source references.
				// TODO: Cache this.
				nosName srcTypeName{};
				if (nosEngine.ObjectAPI->GetObjectTypeName(src, &srcTypeName) != NOS_RESULT_SUCCESS)
					return ObjectRef();
				if (!ContainsForeignObject(srcTypeName))
				{
					copied = src;
					break;
				}
				CompositeObjectRef compSrc(src);
				// Now, we have to shallow copy the composite object, and then deep copy any foreign fields.
				std::vector<CompositeObjectField> foreignObjectContainingFields;
				bool exit = false;
				for (auto& field : compSrc)
				{
					auto it = dstNode.CompositeChildren.find(field.Name);
					if (it == dstNode.CompositeChildren.end())
					{
						exit = true;
						auto newDstFieldSlot = std::make_unique<CopyDestinationNode>();
						newDstFieldSlot->Destination = field.Object;
						auto res = PopulateCopyDestinationNode(field.Object, *newDstFieldSlot);
						if (res != NOS_RESULT_SUCCESS)
							break;
						copied = newDstFieldSlot->Destination;
						break;
					}
					auto& childNode = *it->second;
					auto dst = TransferCopyRecursive(field.Object, childNode);
					if (dst == field.Object)
						continue;
					foreignObjectContainingFields.push_back(CompositeObjectField{field.Name, dst});
				}
				if (exit)
					break;
				if (foreignObjectContainingFields.empty())
				{
					copied = src;
					break;
				}
				auto copiedOpt = compSrc.CopyWithEdits(foreignObjectContainingFields);
				if (!copiedOpt)
					break;
				copied = std::move(*copiedOpt);
				break;
			}
		case NOS_OBJECT_KIND_ARRAY:
			{
				nosName srcTypeName{};
				if (nosEngine.ObjectAPI->GetObjectTypeName(src, &srcTypeName) != NOS_RESULT_SUCCESS)
					return ObjectRef();
				// If the array contains foreign objects, we should call copy on them,
				// otherwise we can just copy the references.
				if (!ContainsForeignObject(srcTypeName))
				{
					copied = src;
					break;
				}
				std::vector<ArrayObjectDelta> edits;
				ArrayObjectRef arraySrc(src);
				std::vector<ArrayObjectDelta> foreignObjectContainingDeltas;
				bool exit = false;
				auto size = arraySrc.GetSize();
				size_t i = 0;
				for (auto& srcField : arraySrc)
				{
					if (i >= dstNode.ArrayChildren.size())
					{
						exit = true;
						auto newDstFieldSlot = std::make_unique<CopyDestinationNode>();
						newDstFieldSlot->Destination = srcField;
						auto res = PopulateCopyDestinationNode(srcField, *newDstFieldSlot);
						if (res != NOS_RESULT_SUCCESS)
							break;
						copied = newDstFieldSlot->Destination;
						break;
					}
					auto& childNode = *dstNode.ArrayChildren[i];
					auto dst = TransferCopyRecursive(srcField, childNode);
					if (dst == srcField)
					{
						++i;
						continue;
					}
					foreignObjectContainingDeltas.push_back(ArrayObjectDelta::Set(i, dst));
					++i;
				}
				if (exit)
					break;
				if (foreignObjectContainingDeltas.empty())
				{
					copied = src;
					break;
				}
				auto copiedOpt = arraySrc.CopyWithEdits(foreignObjectContainingDeltas);
				if (!copiedOpt)
					break;
				copied = std::move(*copiedOpt);
				break;
			}
		case NOS_OBJECT_KIND_FOREIGN:
			{
				ForeignObjectRef srcObj(src);
				auto it = CopyFunctions.find(srcObj.GetTypeName());
				copied = dstNode.Destination;
				if (it != CopyFunctions.end() && it->second.Copy)
				{
					nosObjectId newDst = copied.GetObjectId();
					nosObjectReference newDstRef{};
					it->second.Copy(src, newDst, &newDstRef);
					copied = ObjectRef::AcquireOwnership(newDstRef);
				}
				break;
			}
		}
		dstNode.Destination = copied;
		return copied;
	}

	nosResult CopyDefault(nosObjectId src, nosTransferCopyDestination dst)
	{
		auto it = Slots.find(dst);
		if (it == Slots.end())
		{
			nosEngine.LogE("Copy destination %llu not found", dst);
			return NOS_RESULT_NOT_FOUND;
		}
		auto& slot = *it->second;
		auto res = TransferCopyRecursive(src, slot);
		if (!res)
			return NOS_RESULT_FAILURE;
		return NOS_RESULT_SUCCESS;
	}

	nosBool CanCopy(nosObjectId src, nosTransferCopyDestination dstSlot)
	{
		std::shared_lock lock(CopyMutex);
		nosName srcTypeName{}, dstTypeName{};
		if (nosEngine.ObjectAPI->GetObjectTypeName(src, &srcTypeName) != NOS_RESULT_SUCCESS)
			return NOS_FALSE;
		auto copyDst = GetCopyDestination(dstSlot);
		if (!copyDst)
			return NOS_FALSE;
		if (nosEngine.ObjectAPI->GetObjectTypeName(*copyDst, &dstTypeName) != NOS_RESULT_SUCCESS)
			return NOS_FALSE;
		if (srcTypeName != dstTypeName)
			return NOS_FALSE;
		auto it = CopyFunctions.find(srcTypeName);
		if (it == CopyFunctions.end())
			return NOS_TRUE;
		lock.unlock();
		if (!it->second.CanCopy)
			return NOS_TRUE;
		return it->second.CanCopy(src, *copyDst);
	}

	std::optional<ObjectRef> GetCopyDestination(nosTransferCopyDestination slot)
	{
		auto it = Slots.find(slot);
		if (it == Slots.end())
			return std::nullopt;
		return it->second->Destination;
	}

	nosResult GetObjectReference(nosTransferCopyDestination slot, nosObjectReference* outRef)
	{
		if (!outRef)
			return NOS_RESULT_INVALID_ARGUMENT;
		std::shared_lock lock(CopyMutex);
		auto it = Slots.find(slot);
		if (it == Slots.end())
			return NOS_RESULT_INVALID_ARGUMENT;
		ObjectRef ret = it->second->Destination;
		*outRef = ret.Release();
		return NOS_RESULT_SUCCESS;
	}

	bool ContainsForeignObject(nosName typeName)
	{
		std::stack<nosName> toVisit;
		toVisit.push(typeName);
		while (!toVisit.empty())
		{
			auto current = toVisit.top();
			toVisit.pop();
			nosObjectKind kind{};
			if (NOS_RESULT_SUCCESS != nosEngine.ObjectAPI->GetObjectKindFromTypeName(current, &kind))
				return false;
			if (kind == NOS_OBJECT_KIND_FOREIGN)
				return true;
			if (kind == NOS_OBJECT_KIND_PRIMITIVE)
				continue;
			if (kind == NOS_OBJECT_KIND_ARRAY)
			{
				nos::TypeInfo type(current);
				if (!type)
					return false;
				if (type->BaseType != NOS_BASE_TYPE_ARRAY || !type->ElementType)
					continue;
				toVisit.push(type->ElementType->TypeName);
			}
			else if (kind == NOS_OBJECT_KIND_COMPOSITE)
			{
				nos::TypeInfo type(current);
				if (!type)
					return false;
				if (type->BaseType == NOS_BASE_TYPE_STRUCT)
				{
					for (uint32_t i = 0; i < type->FieldCount; ++i)
					{
						toVisit.push(type->Fields[i].Type->TypeName);
					}
				}
			}
		}
		return false;
	}

	nosResult CreateCopyDestination(nosObjectId src, nosTransferCopyDestination* outDst)
	{
		if (!outDst)
			return NOS_RESULT_INVALID_ARGUMENT;
		std::unique_lock lock(CopyMutex);
		auto slot = std::make_unique<CopyDestinationNode>();
		auto& node = *slot;
		auto id = NextSlotId++;
		Slots[id] = std::move(slot);
		auto res = PopulateCopyDestinationNode(src, node);
		if (res == NOS_RESULT_SUCCESS)
			*outDst = id;
		return res;
	}

	nosResult ReleaseCopyDestination(nosTransferCopyDestination dst)
	{
		std::unique_lock lock(CopyMutex);
		auto it = Slots.find(dst);
		if (it == Slots.end())
			return NOS_RESULT_INVALID_ARGUMENT;
		Slots.erase(it);
		return NOS_RESULT_SUCCESS;
	}

	nosResult PopulateCopyDestinationNode(nosObjectId obj, CopyDestinationNode& node)
	{
		nosObjectKind kind{};
		if (NOS_RESULT_SUCCESS != nosEngine.ObjectAPI->GetObjectKind(obj, &kind))
			return NOS_RESULT_FAILED;
		node.Destination = obj;
		switch (kind)
		{
		case NOS_OBJECT_KIND_PRIMITIVE:
			{
				node.Destination = obj;
				return NOS_RESULT_SUCCESS;
			}
		case NOS_OBJECT_KIND_COMPOSITE:
			{
				CompositeObjectRef compositeObj(obj);
				std::vector<CompositeObjectField> changedFields;
				for (auto& field : compositeObj)
				{
					auto& newChild = *(node.CompositeChildren[field.Name] = std::make_unique<CopyDestinationNode>());
					auto res = PopulateCopyDestinationNode(field.Object, newChild);
					if (res != NOS_RESULT_SUCCESS)
						return res;
					if (field.Object != newChild.Destination)
					{
						CompositeObjectField changed;
						changed.Name = field.Name;
						changed.Object = newChild.Destination;
						changedFields.emplace_back(std::move(changed));
					}
				}
				if (changedFields.empty())
				{
					node.Destination = obj;
					return NOS_RESULT_SUCCESS;
				}
				auto dst = compositeObj.CopyWithEdits(changedFields);
				if (!dst)
					return NOS_RESULT_FAILURE;
				node.Destination = std::move(*dst);
				return NOS_RESULT_SUCCESS;
			}
		case NOS_OBJECT_KIND_ARRAY:
			{
				ArrayObjectRef arrayObj(obj);
				std::vector<ArrayObjectDelta> changedElements;
				size_t idx = 0;
				for (auto& element : arrayObj)
				{
					bool append = node.ArrayChildren.size() <= idx;
					if (append)
						node.ArrayChildren.push_back(nullptr);
					node.ArrayChildren[idx] = std::make_unique<CopyDestinationNode>();
					auto res = PopulateCopyDestinationNode(element, *node.ArrayChildren[idx]);
					if (res != NOS_RESULT_SUCCESS)
						return res;
					if (element != node.ArrayChildren[idx]->Destination)
					{
						if (append)
							changedElements.push_back(ArrayObjectDelta::Append(node.ArrayChildren[idx]->Destination));
						else
							changedElements.push_back(ArrayObjectDelta::Set(idx, node.ArrayChildren[idx]->Destination));
					}
					idx++;
				}
				if (changedElements.empty())
				{
					node.Destination = obj;
					return NOS_RESULT_SUCCESS;
				}
				auto dst = arrayObj.CopyWithEdits(changedElements);
				if (!dst)
					return NOS_RESULT_FAILURE;
				node.Destination = std::move(*dst);
				return NOS_RESULT_SUCCESS;
			}
		case NOS_OBJECT_KIND_FOREIGN:
			{
				node.Destination = ForeignObjectRef(obj).Clone();
				return NOS_RESULT_SUCCESS;
			}
		}
		return NOS_RESULT_SUCCESS;
	}

	std::shared_mutex CopyMutex;
	std::unordered_map<nos::Name, nosTransferCopyFunctions> CopyFunctions;
	nosTransferCopyDestination NextSlotId = 1;
	std::unordered_map<nosTransferCopyDestination, std::unique_ptr<CopyDestinationNode>> Slots;
} GContext;

nosResult NOSAPI_CALL RegisterCopyFunctions(nosName objectTypeName, const nosTransferCopyFunctions* funcs)
{
	return GContext.RegisterCopyFunctions(objectTypeName, funcs);
}
nosResult NOSAPI_CALL UnregisterCopyFunctions(nosName objectTypeName)
{
	return GContext.UnregisterCopyFunctions(objectTypeName);
}

nosResult NOSAPI_CALL Copy(nosObjectId src, nosTransferCopyDestination dst)
{
	return GContext.Copy(src, dst);
}

nosBool NOSAPI_CALL CanCopy(nosObjectId src, nosTransferCopyDestination dst)
{
	return GContext.CanCopy(src, dst);
}

nosResult NOSAPI_CALL GetObjectReference(nosTransferCopyDestination slot, nosObjectReference* outRef)
{
	return GContext.GetObjectReference(slot, outRef);
}

nosResult NOSAPI_CALL CreateCopyDestination(nosObjectId src, nosTransferCopyDestination* outDestination)
{
	return GContext.CreateCopyDestination(src, outDestination);
}

nosResult NOSAPI_CALL ReleaseCopyDestination(nosTransferCopyDestination dst)
{
	return GContext.ReleaseCopyDestination(dst);
}

std::unordered_map<uint32_t, std::unique_ptr<nosTransferSubsystem>> GExportedApiVersions;
NOSAPI_ATTR nosResult NOSAPI_CALL OnRequest(uint32_t minor, void** outApi)
{
	if (auto it = GExportedApiVersions.find(minor); it != GExportedApiVersions.end())
	{
		*outApi = it->second.get();
		return NOS_RESULT_SUCCESS;
	}
	nosTransferSubsystem& subsystem = *(GExportedApiVersions[minor] = std::make_unique<nosTransferSubsystem>());
	subsystem.RegisterCopyFunctions = RegisterCopyFunctions;
	subsystem.UnregisterCopyFunctions = UnregisterCopyFunctions;
	subsystem.CanCopy = CanCopy;
	subsystem.Copy = Copy;
	subsystem.GetObjectReference = GetObjectReference;
	subsystem.CreateCopyDestination = CreateCopyDestination;
	subsystem.ReleaseCopyDestination = ReleaseCopyDestination;

	*outApi = &subsystem;
	return NOS_RESULT_SUCCESS;
}

nosResult NOSAPI_CALL Initialize()
{
	return NOS_RESULT_SUCCESS;
}

}

extern "C"
{
NOSAPI_ATTR nosResult NOSAPI_CALL nosExportPlugin(nosPluginFunctions* out)
{
	out->OnRequestAPI = nos::transfer::OnRequest;
	out->Initialize = nos::transfer::Initialize;
	return NOS_RESULT_SUCCESS;
}
}