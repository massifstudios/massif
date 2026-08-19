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
 * At 150 km erosion alone cannot carry the largest scale: every catchment
 * would be drawn from the same distribution and the world would come out as
 * one undifferentiated field of valleys. This decides instead that one part
 * of the world is high alpine and another is a broad valley floor. The
 * fluvial bake starts from the ceiling this sets and keeps uplifting the
 * ranges while erosion carves them, so relief concentrates where the
 * tectonics say.
 */
class FMassifUpliftField
{
public:
	FMassifUpliftField(const FWorldGenConfig& InConfig, uint32 InSeed);

	FMassifUpliftSample Sample(FVector2d PositionMeters) const;

private:
	FVector2d WarpedPosition(FVector2d PositionMeters) const;

	const FWorldGenConfig& Config;
	uint32 RangeSeed = 0;
	uint32 PeakSeed = 0;
	uint32 WarpSeedX = 0;
	uint32 WarpSeedY = 0;
};
