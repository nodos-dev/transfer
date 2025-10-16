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

typedef nosResult(*nosPfnCopyObject)(nosObjectId src, nosObjectId dst);

typedef uint64_t nosTransferCopyDestination;
	
typedef struct nosTransferCopyFunctions
{
	/// Checks whether the destination is suitable for copying the source object into it.
	nosBool (NOSAPI_CALL* CanCopy)(nosObjectId src, nosObjectId dst);
	nosResult (NOSAPI_CALL* Copy)(nosObjectId src, nosObjectId dst, nosObjectReference* outNewDst);
} nosTransferCopyFunctions;

typedef struct nosTransferSubsystem {
	nosResult (NOSAPI_CALL* RegisterCopyFunctions)(nosName objectTypeName, const nosTransferCopyFunctions* functions);
	nosResult (NOSAPI_CALL* UnregisterCopyFunctions)(nosName objectTypeName);

	/// Creates a slot that will be used for copying the object.
	nosResult (NOSAPI_CALL* CreateCopyDestination)(nosObjectId copySource, nosTransferCopyDestination* outDestination);
	nosResult (NOSAPI_CALL* ReleaseCopyDestination)(nosTransferCopyDestination destination);
	/// Copies the source object into the copy destination.
	/// When using the transfer subsystems' copy API, if there's no nosTransferCopyFunctions registered for the object's type,
	/// default implementation will do the following:
	/// - For all immutable types, it will only set the object handle inside the copy destination to the source object handle.
	/// - If the type contains a foreign object, it will do a shallow copy of the object and call the
	///   foreign object's copy function if available.
	///   If it is not available, it will simply construct a new foreign object from the source object's serialized buffer.
	///   All the rest of the fields' object handles will be set to the source object's fields'.
	/// inoutCopiedObject should point to a null object handle for non-foreign objects, and for foreign objects,
	/// it should point to a valid object handle of the same type as src.
	nosResult (NOSAPI_CALL* Copy)(nosObjectId src, nosTransferCopyDestination dst);
	nosBool (NOSAPI_CALL* CanCopy)(nosObjectId src, nosTransferCopyDestination dst);
	nosResult (NOSAPI_CALL* GetObjectReference)(nosTransferCopyDestination dst, nosObjectReference* outRef);
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