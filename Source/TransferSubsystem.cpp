#include <Nodos/Plugin.hpp>

#include <nosTransfer/nosTransfer.h>

NOS_INIT()
NOS_BEGIN_IMPORT_DEPS()
NOS_END_IMPORT_DEPS()

namespace nos::transfer
{

struct Context
{
	nosResult RegisterCopyFunctions(nos::Name objectTypeName, const nosTransferCopyFunctions* funcs)
	{
		std::unique_lock lock(CopyFunctionsMutex);
		if (CopyFunctions.contains(objectTypeName))
			return NOS_RESULT_INVALID_ARGUMENT;
		if (!funcs || (!funcs->CanCopy && !funcs->Copy && !funcs->CreateCopyDestination))
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

	nosResult Copy(nosObjectHandle src, nosObjectHandle dst, nosObjectHandle* outNewDst)
	{
		nosName srcTypeName{}, dstTypeName{};
		if (nosEngine.ObjectAPI->GetObjectTypeName(src, &srcTypeName) != NOS_RESULT_SUCCESS)
			return NOS_RESULT_INVALID_ARGUMENT;
		if (nosEngine.ObjectAPI->GetObjectTypeName(dst, &dstTypeName) != NOS_RESULT_SUCCESS)
			return NOS_RESULT_INVALID_ARGUMENT;
		if (srcTypeName != dstTypeName)
			return NOS_RESULT_INVALID_ARGUMENT;
		ObjectRef srcObj(src);
		ObjectRef dstObj(dst);
		std::shared_lock lock(CopyFunctionsMutex);
		if (!srcObj.IsValid() || !dstObj.IsValid())
			return NOS_RESULT_INVALID_ARGUMENT;
		std::shared_lock funcsLock(CopyFunctionsMutex);
		auto res = CopyObjectRecursive(srcObj, dstObj, funcsLock);
		if (res != NOS_RESULT_SUCCESS)
			return res;
		if (outNewDst)
			*outNewDst = dstObj.Handle;
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
		// TODO: Check recursively too.
	}

	nosResult CreateCopyDestination(nosObjectHandle src, nosObjectHandle* outDst)
	{
		if (!outDst)
			return NOS_RESULT_INVALID_ARGUMENT;
		std::shared_lock funcsLock(CopyFunctionsMutex);
		ObjectRef dst{};
		auto res = CreateCopyDestinationRecursive(src, dst, funcsLock);
		if (res != NOS_RESULT_SUCCESS)
			return res;
		*outDst = dst.Release();
		return NOS_RESULT_SUCCESS;
	}
	nosResult CreateCopyDestinationRecursive(ObjectRef src,
											 ObjectRef& dst,
											 std::shared_lock<std::shared_mutex>const& funcsLock)
	{
		nos::Name typeName = src.GetTypeName();
		if (!typeName.IsValid())
			return NOS_RESULT_INVALID_ARGUMENT;
		auto it = CopyFunctions.find(typeName);
		if (it != CopyFunctions.end() && it->second.CreateCopyDestination)
		{
			auto& func = it->second.CreateCopyDestination;
			auto res = func(src, &dst.Handle);
			if (res != NOS_RESULT_SUCCESS)
				return res;
			return NOS_RESULT_SUCCESS;
		}
		nosObjectKind kind{};
		if (NOS_RESULT_SUCCESS != nosEngine.ObjectAPI->GetObjectKind(src, &kind))
			return NOS_RESULT_FAILED;
		switch (kind)
		{
		case NOS_OBJECT_KIND_PRIMITIVE:
			{
				dst = src;
				return NOS_RESULT_SUCCESS;
			}
		case NOS_OBJECT_KIND_COMPOSITE:
			{
				std::vector<ObjectRef> newFieldObjects;
				std::vector<nosCompositeObjectField> changedFields;
				nosObjectHandle fieldHandle{};
				nosName fieldName{};
				size_t i = 0;
				while (NOS_RESULT_SUCCESS == nosEngine.ObjectAPI->IterateFields(src, i++, &fieldName, &fieldHandle))
				{
					ObjectRef newDstFieldObject{};
					CreateCopyDestinationRecursive(fieldHandle, newDstFieldObject, funcsLock);
					if (src.Handle == newDstFieldObject.Handle)
					{
						continue;
					}
					newFieldObjects.push_back(newDstFieldObject);
					changedFields.push_back({fieldName, newDstFieldObject.Handle});
				}
				if (changedFields.empty())
				{
					dst = src;
					return NOS_RESULT_SUCCESS;
				}
				ObjectRef newComposite{};
				nosEngine.ObjectAPI->CopyCompositeObjectWithEdits(src, changedFields.data(), changedFields.size(), &newComposite.Handle);
				dst = newComposite;
				return NOS_RESULT_SUCCESS;	
			}
		case NOS_OBJECT_KIND_ARRAY:
			{
				return NOS_RESULT_NOT_IMPLEMENTED;
			}
		case NOS_OBJECT_KIND_FOREIGN:
			{
				// Since the foreign object did not register a copy function, we can only re-create using the serialized
				// buffer of the source object.
				ForeignObjectRef srcObj(src);
				dst = srcObj.CloneForeignObject();
				return NOS_RESULT_SUCCESS;
			}
		}
	}

	nosResult CopyObjectRecursive(ObjectRef src, ObjectRef& dst, std::shared_lock<std::shared_mutex> const& funcsLock)
	{
		nos::Name typeName = src.GetTypeName();
		if (!typeName.IsValid())
			return NOS_RESULT_INVALID_ARGUMENT;
		auto it = CopyFunctions.find(typeName);
		if (it != CopyFunctions.end() && it->second.Copy)
		{
			auto& func = it->second.Copy;
			auto res = func(src, dst, &dst.Handle);
			if (res != NOS_RESULT_SUCCESS)
				return res;
			return NOS_RESULT_SUCCESS;
		}
		nosObjectKind kind{};
		if (NOS_RESULT_SUCCESS != nosEngine.ObjectAPI->GetObjectKind(src, &kind))
			return NOS_RESULT_FAILED;
		switch (kind)
		{
		case NOS_OBJECT_KIND_PRIMITIVE:
			{
				dst = src;
				return NOS_RESULT_SUCCESS;
			}
		case NOS_OBJECT_KIND_COMPOSITE:
			{
				std::vector<ObjectRef> newFieldObjects;
				std::vector<nosCompositeObjectField> changedFields;
				ObjectRef srcFieldObject{};
				nosName srcFieldName{};
				size_t i = 0;
				while (NOS_RESULT_SUCCESS == nosEngine.ObjectAPI->IterateFields(src, i++, &srcFieldName, &srcFieldObject.Handle))
				{
					ObjectRef dstFieldObject{};
					nosEngine.ObjectAPI->GetField(dst, srcFieldName, &dstFieldObject.Handle);
					auto newDstField = dstFieldObject;
					auto res = CopyObjectRecursive(srcFieldObject, newDstField, funcsLock);
					if (dstFieldObject.Handle == newDstField.Handle)
					{
						continue;
					}
					newFieldObjects.push_back(newDstField);
					changedFields.push_back({srcFieldName, newDstField.Handle});
				}
				if (changedFields.empty())
				{
					// This either means object is composed of foreigns or the same field objects are shared between src and dst.
					return NOS_RESULT_SUCCESS;
				}
				ObjectRef newComposite{};
				nosEngine.ObjectAPI->CopyCompositeObjectWithEdits(src, changedFields.data(), changedFields.size(), &newComposite.Handle);
				dst = newComposite;
				return NOS_RESULT_SUCCESS;
			}
		case NOS_OBJECT_KIND_ARRAY:
			{
				return NOS_RESULT_NOT_IMPLEMENTED;
			}
		case NOS_OBJECT_KIND_FOREIGN:
			{
				// Since the foreign object did not register a copy function, we can only re-create using the serialized
				// buffer of the source object.
				ForeignObjectRef srcObj(src);
				dst = srcObj.CloneForeignObject();
				return NOS_RESULT_SUCCESS;
			}
		}
		return NOS_RESULT_NOT_IMPLEMENTED;
	}

	std::shared_mutex CopyFunctionsMutex;
	std::unordered_map<nos::Name, nosTransferCopyFunctions> CopyFunctions;
} GContext;

nosResult NOSAPI_CALL RegisterCopyFunctions(nosName objectTypeName, const nosTransferCopyFunctions* funcs)
{
	return GContext.RegisterCopyFunctions(objectTypeName, funcs);
}
nosResult NOSAPI_CALL UnregisterCopyFunctions(nosName objectTypeName)
{
	return GContext.UnregisterCopyFunctions(objectTypeName);
}

nosResult NOSAPI_CALL Copy(nosObjectHandle src, nosObjectHandle dst, nosObjectHandle* outNewDst)
{
	return GContext.Copy(src, dst, outNewDst);
}

nosBool NOSAPI_CALL CanCopy(nosObjectHandle src, nosObjectHandle dst)
{
	return GContext.CanCopy(src, dst);
}

nosResult NOSAPI_CALL CreateCopyDestination(nosObjectHandle src, nosObjectHandle* outDestination)
{
	return GContext.CreateCopyDestination(src, outDestination);
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