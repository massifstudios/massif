#pragma once

#include "CoreMinimal.h"
#include "WorldGenConfig.h"

class FMassifUpliftField;
class FMassifFluvialBake;

/**
 * The terrain, as a pure function of world position and the once-per-seed
 * coarse fluvial bake:
 *
 *     Height(Position, SampleSpacing)
 *
 * There is no fine heightfield. At 150 km across, a grid fine enough for the
 * detail we want could not exist — a centimeter lattice would be 10^15
 * samples — so on-demand evaluation is not an optimisation, it is the only
 * representation that works. The bake is coarse, global and immutable after
 * construction, and everything below its resolution is evaluated per point in
 * isolation, which is what lets terrain be sampled in tiles, at any
 * resolution, without generating anything around them.
 *
 * SampleSpacing is the distance between the samples the caller intends to take.
 * Detail finer than a few times that spacing is dropped, and the finest octave
 * kept is faded rather than switched off, so a coarse sampling and a fine
 * sampling of the same point agree instead of stepping where they meet.
 *
 * Positions are doubles throughout. A float coordinate 75 km from the origin
 * resolves about 4 mm, which would spend most of the detail budget on
 * coordinate error before any terrain was evaluated.
 *
 * Thread-safe once constructed: sampling touches only immutable state.
 */
class MASSIFWORLDGEN_API FMassifTerrainField
{
public:
	explicit FMassifTerrainField(const FWorldGenConfig& InConfig);
	~FMassifTerrainField();

	FMassifTerrainField(const FMassifTerrainField&) = delete;
	FMassifTerrainField& operator=(const FMassifTerrainField&) = delete;

	/** Ground altitude in meters. */
	double SampleHeightMeters(FVector2d PositionMeters, double SampleSpacingMeters) const;

	/** Upward surface normal, differenced at the sample spacing. */
	FVector3d SampleNormal(FVector2d PositionMeters, double SampleSpacingMeters) const;

	double SampleSlopeDegrees(FVector2d PositionMeters, double SampleSpacingMeters) const;

	const FWorldGenConfig& GetConfig() const
	{
		return Config;
	}

	const FMassifFluvialBake& GetFluvialBake() const;

	/** Octaves worth evaluating at a spacing, and the weight the finest of them gets. */
	int32 OctavesForSpacing(double SampleSpacingMeters, float& OutFinestWeight) const;

private:
	/** 0 at the water the ground here drains to, 1 at the crest altitude overhead. */
	float ComputeReliefFraction(FVector2d PositionMeters, double GroundHeightMeters) const;

	FWorldGenConfig Config;
	TUniquePtr<FMassifUpliftField> Uplift;
	TUniquePtr<FMassifFluvialBake> FluvialBake;
};
