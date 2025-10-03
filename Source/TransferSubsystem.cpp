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
		std::unique_lock lock(CopyFunctionsMutex);
		if (CopyFunctions.contains(objectTypeName))
			return NOS_RESULT_INVALID_ARGUMENT;
		if (!funcs || (!funcs->CanCopy && !funcs->Copy))
			return NOS_RESULT_INVALID_ARGUMENT;
		CopyFunctions[objectTypeName] = *funcs;
		return NOS_RESULT_SUCCESS;
	}

	nosResult UnregisterCopyFunctions(nos::Name objectTypeName)
	{
		std::unique_lock lock(CopyFunctionsMutex);
		auto it = CopyFunctions.find(objectTypeName);
		if (it == CopyFunctions.end())
			return NOS_RESULT_INVALID_ARGUMENT;
		CopyFunctions.erase(it);
		return NOS_RESULT_SUCCESS;
	}

	nosResult Copy(nosObjectHandle src, nosTransferCopyDestination dst)
	{
		nosName srcTypeName{}, dstTypeName{};
		if (nosEngine.ObjectAPI->GetObjectTypeName(src, &srcTypeName) != NOS_RESULT_SUCCESS)
			return NOS_RESULT_INVALID_ARGUMENT;
		if (nosEngine.ObjectAPI->GetObjectTypeName(dst, &dstTypeName) != NOS_RESULT_SUCCESS)
			return NOS_RESULT_INVALID_ARGUMENT;
		if (srcTypeName != dstTypeName)
			return NOS_RESULT_INVALID_ARGUMENT;
		std::shared_lock lock(CopyFunctionsMutex);
		auto it = CopyFunctions.find(srcTypeName);
		if (it == CopyFunctions.end())
			return CopyDefault(src, dst); 
		lock.unlock();
		if (!it->second.Copy)
			return NOS_RESULT_NOT_IMPLEMENTED;
		std::shared_lock slotsLock(SlotsMutex);
		auto sit = Slots.find(dst);
		if (sit == Slots.end())
			return NOS_RESULT_INVALID_ARGUMENT;
		auto dstHandle = sit->second->Destination.Handle;
		auto res = it->second.Copy(src, &dstHandle);
		if (res == NOS_RESULT_SUCCESS)
		{
			slotsLock.unlock();
			std::unique_lock slotsWriteLock(SlotsMutex);
			Slots[dst]->Destination = ObjectRef(dstHandle);
		}
		return res;
	}

	ObjectRef TransferCopyRecursive(nosObjectHandle src, CopyDestinationNode& dstNode)
	{
		nosObjectKind kind{};
		if (NOS_RESULT_SUCCESS != nosEngine.ObjectAPI->GetObjectKind(src, &kind))
			return NOS_RESULT_FAILED;
		switch (kind)
		{
		case NOS_OBJECT_KIND_PRIMITIVE:
			{
				return src;
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
					return src;
				}
				// Now, we have to shallow copy the composite object, and then deep copy any foreign fields.
				std::vector<nosCompositeObjectField> foreignObjectContainingFields;
				nosObjectHandle fieldHandle{};
				nosName fieldName{};
				size_t i = 0;
				while (NOS_RESULT_SUCCESS == nosEngine.ObjectAPI->IterateFields(src, i++, &fieldName, &fieldHandle))
				{
					auto it = dstNode.CompositeChildren.find(fieldName);
					if (it == dstNode.CompositeChildren.end())
						return ObjectRef();
					auto& childNode = *it->second;
					auto dst = TransferCopyRecursive(fieldHandle, childNode);
					if (dst.Handle == fieldHandle)
						continue;
					foreignObjectContainingFields.push_back(nosCompositeObjectField{fieldName, dst.Handle});
				}
				if (foreignObjectContainingFields.empty())
					return src;
				ObjectRef newComposite{};
				nosEngine.ObjectAPI->CopyCompositeObjectWithEdits(src, foreignObjectContainingFields.data(), foreignObjectContainingFields.size(), &newComposite.Handle);
				return newComposite;
			}
		case NOS_OBJECT_KIND_ARRAY:
			{
				// If the array contains foreign objects, we should call copy on them,
				// otherwise we can just copy the references.
				return ObjectRef();
			}
		case NOS_OBJECT_KIND_FOREIGN:
			{
				ForeignObjectRef srcObj(src);
				std::shared_lock lock(CopyFunctionsMutex);
				auto it = CopyFunctions.find(srcObj.GetTypeName());
				if (it != CopyFunctions.end() && it->second.Copy)
					it->second.Copy(src, &dstNode.Destination.Handle);
				return dstNode.Destination;
			}
		}
	}

	nosResult CopyDefault(nosObjectHandle src, nosTransferCopyDestination dst)
	{
		std::shared_lock lock(SlotsMutex);
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

	nosBool CanCopy(nosObjectHandle src, nosObjectHandle dst)
	{
		nosName srcTypeName{}, dstTypeName{};
		if (nosEngine.ObjectAPI->GetObjectTypeName(src, &srcTypeName) != NOS_RESULT_SUCCESS)
			return NOS_FALSE;
		if (nosEngine.ObjectAPI->GetObjectTypeName(dst, &dstTypeName) != NOS_RESULT_SUCCESS)
			return NOS_FALSE;
		if (srcTypeName != dstTypeName)
			return NOS_FALSE;
		std::shared_lock lock(CopyFunctionsMutex);
		auto it = CopyFunctions.find(srcTypeName);
		if (it == CopyFunctions.end())
			return NOS_TRUE;
		lock.unlock();
		if (!it->second.CanCopy)
			return NOS_TRUE;
		return it->second.CanCopy(src, dst);
	}

	nosResult GetObjectHandle(nosTransferCopyDestination slot, nosObjectHandle* outObjectHandle)
	{
		if (!outObjectHandle)
			return NOS_RESULT_INVALID_ARGUMENT;
		std::shared_lock lock(SlotsMutex);
		auto it = Slots.find(slot);
		if (it == Slots.end())
			return NOS_RESULT_INVALID_ARGUMENT;
		*outObjectHandle = it->second->Destination.Handle;
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

	nosResult CreateCopyDestination(nosObjectHandle src, nosTransferCopyDestination* outDst)
	{
		if (!outDst)
			return NOS_RESULT_INVALID_ARGUMENT;
		std::unique_lock lock(SlotsMutex);
		auto slot = std::make_unique<CopyDestinationNode>();
		auto& node = *slot;
		Slots[NextSlotId++] = std::move(slot);
		return PopulateCopyDestinationNode(src, node);
	}

	nosResult ReleaseCopyDestination(nosTransferCopyDestination dst)
	{
		std::unique_lock lock(SlotsMutex);
		auto it = Slots.find(dst);
		if (it == Slots.end())
			return NOS_RESULT_INVALID_ARGUMENT;
		Slots.erase(it);
		return NOS_RESULT_SUCCESS;
	}

	nosResult PopulateCopyDestinationNode(nosObjectHandle obj, CopyDestinationNode& node)
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
				nosName srcTypeName{};
				if (nosEngine.ObjectAPI->GetObjectTypeName(obj, &srcTypeName) != NOS_RESULT_SUCCESS)
					return NOS_RESULT_FAILURE;
				nosObjectHandle fieldHandle{};
				nosName fieldName{};
				size_t i = 0;
				std::vector<nosCompositeObjectField> changedFields;
				while (NOS_RESULT_SUCCESS == nosEngine.ObjectAPI->IterateFields(obj, i++, &fieldName, &fieldHandle))
				{
					node.CompositeChildren[fieldName] = std::make_unique<CopyDestinationNode>();
					auto res = PopulateCopyDestinationNode(fieldHandle, *node.CompositeChildren[fieldName]);
					if (res != NOS_RESULT_SUCCESS)
						return res;
					if (fieldHandle != node.CompositeChildren[fieldName]->Destination.Handle)
					{
						changedFields.push_back({fieldName, node.CompositeChildren[fieldName]->Destination.Handle});
					}
				}
				if (changedFields.empty())
				{
					node.Destination = obj;
					return NOS_RESULT_SUCCESS;
				}
				return nosEngine.ObjectAPI->CopyCompositeObjectWithEdits(obj, changedFields.data(), changedFields.size(), &node.Destination.Handle);
			}
		case NOS_OBJECT_KIND_ARRAY:
			{
				return NOS_RESULT_NOT_IMPLEMENTED;
			}
		case NOS_OBJECT_KIND_FOREIGN:
			{
				node.Destination = ForeignObjectRef(obj).Clone();
				return NOS_RESULT_SUCCESS;
			}
		}
		return NOS_RESULT_SUCCESS;
	}

	std::shared_mutex CopyFunctionsMutex;
	std::unordered_map<nos::Name, nosTransferCopyFunctions> CopyFunctions;
	std::shared_mutex SlotsMutex;
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

nosResult NOSAPI_CALL Copy(nosObjectHandle src, nosTransferCopyDestination dst)
{
	return GContext.Copy(src, dst);
}

nosBool NOSAPI_CALL CanCopy(nosObjectHandle src, nosTransferCopyDestination dst)
{
	return GContext.CanCopy(src, dst);
}

nosResult NOSAPI_CALL GetObjectHandle(nosObjectHandle src, nosObjectHandle* outObjectHandle)
{
	return GContext.GetObjectHandle(src, outObjectHandle);
}

nosResult NOSAPI_CALL CreateCopyDestination(nosObjectHandle src, nosTransferCopyDestination* outDestination)
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
	subsystem.GetObjectHandle = GetObjectHandle;
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