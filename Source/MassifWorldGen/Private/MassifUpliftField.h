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
 * Base relief and the erosion filter draw from the same distribution
 * everywhere; this decides instead that one part of the world is high alpine
 * and another is a broad valley floor. The base surface lerps between base
 * level and the crest ceiling this sets, so relief concentrates where the
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
