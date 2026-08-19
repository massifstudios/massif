#pragma once

#include "CoreMinimal.h"
#include "WorldGenConfig.h"
#include "WorldGenResult.h"

struct MASSIFWORLDGEN_API FWorldGenPipeline
{
	/**
	 * Runs all generation stages. CPU stages are deterministic per config on
	 * every platform; GPU erosion is deterministic per machine. Call from the
	 * game thread when erosion is enabled (it dispatches render commands);
	 * erosion is skipped without a rendering RHI.
	 */
	static FWorldGenResult Generate(const FWorldGenConfig& Config);
};
