#pragma once

#include "CoreMinimal.h"

class FMassifHeightmap;

/**
 * The terrain, as a function of world position:
 *
 *     Height(Position, SampleSpacing)
 *
 * The surface comes from a measured elevation model (§5): a real massif,
 * imported once and sampled here. Later layers — geological refinement and
 * sub-meter procedural detail — are added on top of it behind this same API,
 * so callers keep sampling one continuous function and never see the source.
 *
 * SampleSpacing is the distance between the samples the caller intends to
 * take. Detail finer than that spacing is filtered out rather than switched
 * off, so a coarse sampling and a fine sampling of the same point agree
 * instead of stepping where they meet.
 *
 * Positions are doubles throughout. A float coordinate at the world edge
 * resolves only about a millimeter, which would spend the fine-detail budget
 * on coordinate error before any terrain was evaluated.
 *
 * Thread-safe once constructed: sampling touches only immutable state.
 */
class MASSIFWORLDGEN_API FMassifTerrainField
{
public:
	explicit FMassifTerrainField(TSharedRef<const FMassifHeightmap> InHeightmap);

	FMassifTerrainField(const FMassifTerrainField&) = delete;
	FMassifTerrainField& operator=(const FMassifTerrainField&) = delete;

	/** Ground altitude in meters. */
	double SampleHeightMeters(FVector2d PositionMeters, double SampleSpacingMeters) const;

	/** Upward surface normal, differenced at the sample spacing. */
	FVector3d SampleNormal(FVector2d PositionMeters, double SampleSpacingMeters) const;

	double SampleSlopeDegrees(FVector2d PositionMeters, double SampleSpacingMeters) const;

	/** Side of the square the terrain covers, origin at its centre. */
	double GetWorldSizeMeters() const;

	double GetMinAltitudeMeters() const;
	double GetMaxAltitudeMeters() const;

	const FMassifHeightmap& GetHeightmap() const
	{
		return Heightmap.Get();
	}

private:
	TSharedRef<const FMassifHeightmap> Heightmap;
};
