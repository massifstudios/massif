#pragma once

#include "CoreMinimal.h"
#include "WorldGenResult.h"

struct MASSIFWORLDGEN_API FWorldGenDebugExport
{
	/** Writes Heightfield.png (normalized grayscale) and Hillshade.png into Directory. */
	static bool SaveHeightfieldPngs(const FWorldGenResult& Result, const FString& Directory);
};
