#pragma once

#include "CoreMinimal.h"

struct FWorldGenConfig;

/** What the continental shape does at one point, before any water has run over it. */
struct FMassifUpliftSample
{
	/** Altitude the crests reach around here, in meters. */
	double CrestAltitudeMeters = 0.0;

	/** 0 on a basin floor, 1 on the axis of a range. */
	float RangeFraction = 0.0f;
};

/**
 * The tectonic layer: where mountain ranges stand and where basins lie,
 * varying over tens of kilometers.
 *
 * At 150 km the hydrology graph alone cannot carry the largest scale. Left to
 * itself it produces one undifferentiated field of valleys, because every
 * catchment is drawn from the same distribution. This decides instead that one
 * part of the world is high alpine and another is a broad valley floor, and
 * the rivers then run through what it sets out: it caps how high headwaters
 * may climb, so drainage stays inside the ranges that contain it.
 */
class FMassifUpliftField
{
public:
	FMassifUpliftField(const FWorldGenConfig& InConfig, uint32 InSeed);

	FMassifUpliftSample Sample(FVector2d PositionMeters) const;

	/** Highest a watercourse may rise here, leaving room for the relief above it. */
	double MaxWaterAltitudeMeters(FVector2d PositionMeters) const;

private:
	FVector2d WarpedPosition(FVector2d PositionMeters) const;

	const FWorldGenConfig& Config;
	uint32 RangeSeed = 0;
	uint32 WarpSeedX = 0;
	uint32 WarpSeedY = 0;
};
