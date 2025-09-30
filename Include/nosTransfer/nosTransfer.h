/*
 * Copyright MediaZ Teknoloji A.S. All Rights Reserved.
 */

#ifndef NOS_TRANSFER_H_INCLUDED
#define NOS_TRANSFER_H_INCLUDED

#if __cplusplus
extern "C"
{
#endif

#include <Nodos/Types.h>

typedef struct nosTransferSubsystem {
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