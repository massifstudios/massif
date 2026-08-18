#pragma once

#include "CoreMinimal.h"
#include "WorldGenConfig.h"
#include "WorldGenResult.h"

struct MASSIFWORLDGEN_API FWorldGenPipeline
{
	/** Runs all generation stages. Deterministic per config; safe to call off the game thread. */
	static FWorldGenResult Generate(const FWorldGenConfig& Config);
};
