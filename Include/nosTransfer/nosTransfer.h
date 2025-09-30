/*
 * Copyright MediaZ Teknoloji A.S. All Rights Reserved.
 */

#ifndef NOS_TRANSFER_H_INCLUDED
#define NOS_TRANSFER_H_INCLUDED

#if __cplusplus
extern "C"
{
#endif

#include <Nodos/PluginAPI.h>

typedef nosResult(*nosPfnCopyObject)(nosObjectHandle src, nosObjectHandle dst);

typedef struct nosTransferCopyFunctions
{
	/// Checks whether the destination is suitable for copying the source object into it.
	nosBool (NOSAPI_CALL* CanCopy)(nosObjectHandle src, nosObjectHandle dst);
	/// Copies the source object into the destination object.
	/// When using the transfer subsystems' copy API, if there's no nosTransferCopyFunctions registered for the object's type,
	/// it will use the object API's GetBuffer/SetBuffer functions to copy the object data.
	nosResult (NOSAPI_CALL* Copy)(nosObjectHandle src, nosObjectHandle dst);
} nosTransferCopyFunctions;

typedef struct nosTransferSubsystem {
	nosResult (NOSAPI_CALL* RegisterCopyFunctions)(nosName objectTypeName, const nosTransferCopyFunctions* functions);
	nosResult (NOSAPI_CALL* UnregisterCopyFunctions)(nosName objectTypeName);

	nosTransferCopyFunctions* CopyAPI;
} nosTransferSubsystem;

#pragma region Helper Declarations & Macros

// Make sure these are same with nossys file.
#define NOS_TRANSFER_PLUGIN_NAME "nos.transfer"
#define NOS_TRANSFER_PLUGIN_VERSION_MAJOR 0
#define NOS_TRANSFER_PLUGIN_VERSION_MINOR 1

extern struct nosPluginInfo nosTransferPluginInfo;
extern nosTransferSubsystem* nosTransfer;

#define NOS_TRANSFER_PLUGIN_INIT()      \
	nosPluginInfo nosTransferPluginInfo; \
	nosTransferSubsystem* nosTransfer = nullptr;

#define NOS_TRANSFER_PLUGIN_IMPORT() NOS_IMPORT_DEP(NOS_TRANSFER_PLUGIN_NAME, nosTransferPluginInfo, nosTransfer)

#pragma endregion

#if __cplusplus
}
#endif

#endif // NOS_TRANSFER_H_INCLUDED 