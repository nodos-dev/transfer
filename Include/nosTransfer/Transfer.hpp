#pragma once

#include "nosTransfer.h"

#include <Nodos/Plugin.hpp>

namespace  nos::transfer
{
struct Slot
{
	nosTransferCopyDestination Handle;
	Slot(ObjectRef src)
	{
		auto res = nosTransfer->CreateCopyDestination(src, &Handle);
		NOS_SOFT_CHECK(res == NOS_RESULT_SUCCESS, "Failed to create transfer copy destination slot");
	}
	~Slot()
	{
		nosTransfer->ReleaseCopyDestination(Handle);
	}
	Slot(const Slot&) = delete;
	Slot& operator=(const Slot&) = delete;
	nosResult CopyFrom(nosObjectId obj)
	{
		return nosTransfer->Copy(obj, Handle);
	}
	bool IsDestinationCompatibleWith(nosObjectId obj) const
	{
		return nosTransfer->CanCopy(obj, Handle) == NOS_TRUE;
	}
	ObjectRef GetObject() const
	{
		ObjectRef obj{};
		if (nosTransfer->GetObjectReference(Handle, &obj.GetStorage()) != NOS_RESULT_SUCCESS)
			return ObjectRef();
		return obj;
	}
};

inline uint64_t GetPhaseCount(nosObjectId obj, std::optional<std::string>* outWarning = nullptr)
{
	uint64_t phaseCount = 1;
	nosName warning{};
	nosTransfer->GetPhaseCount(obj, &phaseCount, &warning);
	if (outWarning)
		*outWarning = warning.ID ? std::optional(nos::Name(warning).AsString()) : std::nullopt;
	return phaseCount;
}
}
