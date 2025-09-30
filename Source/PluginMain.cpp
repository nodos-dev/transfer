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
		std::unique_lock lock(Mutex);
		if (CopyFunctions.contains(objectTypeName))
			return NOS_RESULT_INVALID_ARGUMENT;
		if (!funcs || (!funcs->CanCopy && !funcs->Copy))
			return NOS_RESULT_INVALID_ARGUMENT;
		CopyFunctions[objectTypeName] = *funcs;
		return NOS_RESULT_SUCCESS;
	}

	nosResult UnregisterCopyFunctions(nos::Name objectTypeName)
	{
		std::unique_lock lock(Mutex);
		auto it = CopyFunctions.find(objectTypeName);
		if (it == CopyFunctions.end())
			return NOS_RESULT_INVALID_ARGUMENT;
		CopyFunctions.erase(it);
		return NOS_RESULT_SUCCESS;
	}

	nosResult Copy(nosObjectHandle src, nosObjectHandle dst)
	{
		nosName srcTypeName{}, dstTypeName{};
		if (nosEngine.ObjectAPI->GetObjectTypeName(src, &srcTypeName) != NOS_RESULT_SUCCESS)
			return NOS_RESULT_INVALID_ARGUMENT;
		if (nosEngine.ObjectAPI->GetObjectTypeName(dst, &dstTypeName) != NOS_RESULT_SUCCESS)
			return NOS_RESULT_INVALID_ARGUMENT;
		if (srcTypeName != dstTypeName)
			return NOS_RESULT_INVALID_ARGUMENT;
		std::shared_lock lock(Mutex);
		auto it = CopyFunctions.find(srcTypeName);
		if (it == CopyFunctions.end())
			return CopyDefault(src, dst); 
		lock.unlock();
		if (!it->second.Copy)
			return NOS_RESULT_NOT_IMPLEMENTED;
		return it->second.Copy(src, dst);
	}

	nosResult CopyDefault(nosObjectHandle src, nosObjectHandle dst)
	{
		// TODO: Arrays, composites (?)
		nosBuffer const* srcBuf = nullptr;
		nosEngine.ObjectAPI->GetBufferView(src, &srcBuf);
		if (!srcBuf)
			return NOS_RESULT_INVALID_ARGUMENT;
		return nosEngine.ObjectAPI->SetBuffer(dst, *srcBuf);
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
		std::shared_lock lock(Mutex);
		auto it = CopyFunctions.find(srcTypeName);
		if (it == CopyFunctions.end())
			return NOS_TRUE;
		lock.unlock();
		if (!it->second.CanCopy)
			return NOS_TRUE;
		return it->second.CanCopy(src, dst);
	}

	std::shared_mutex Mutex;
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

nosResult NOSAPI_CALL Copy(nosObjectHandle src, nosObjectHandle dst)
{
	return GContext.Copy(src, dst);
}
nosBool NOSAPI_CALL CanCopy(nosObjectHandle src, nosObjectHandle dst)
{
	return GContext.CanCopy(src, dst);
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

	static nosTransferCopyFunctions copyApi{};
	copyApi.CanCopy = CanCopy;
	copyApi.Copy = Copy;
	subsystem.CopyAPI = &copyApi;

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