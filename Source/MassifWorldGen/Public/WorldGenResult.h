#pragma once

#include "CoreMinimal.h"

/**
 * Generated world data. Single source of truth for terrain height queries:
 * sim and presentation both sample this, never the rendered meshes.
 *
 * Coordinates: world meters with the origin at the map center, X east,
 * Y north. Heights are absolute altitude in meters.
 */
struct MASSIFWORLDGEN_API FWorldGenResult
{
	int32 Resolution = 0;
	float WorldSizeMeters = 0.0f;
	float MetersPerPixel = 0.0f;
	TArray<float> HeightMeters;

	bool IsValid() const
	{
		return Resolution > 1 && HeightMeters.Num() == Resolution * Resolution;
	}

	float GetHeightAtPixel(int32 PixelX, int32 PixelY) const;

	/** Bilinear height sample; positions outside the map clamp to the border. */
	float SampleHeightMeters(float WorldX, float WorldY) const;

	/** Upward surface normal from central differences. */
	FVector3f SampleNormal(float WorldX, float WorldY) const;

	float SampleSlopeDegrees(float WorldX, float WorldY) const;

	uint64 ComputeHash() const;
};
