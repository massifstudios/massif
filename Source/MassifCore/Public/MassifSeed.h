#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"

/**
 * Master seed with derived per-system channels.
 * Channel derivation hashes the channel name string (not FName indices),
 * so results are stable across sessions and platforms.
 */
struct MASSIFCORE_API FMassifSeed
{
	uint32 MasterSeed = 0;

	FMassifSeed() = default;

	explicit FMassifSeed(uint32 InMasterSeed)
		: MasterSeed(InMasterSeed)
	{
	}

	uint32 MakeChannelSeed(const TCHAR* ChannelName) const;
	FRandomStream MakeChannelStream(const TCHAR* ChannelName) const;
};
