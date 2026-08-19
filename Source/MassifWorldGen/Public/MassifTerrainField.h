#pragma once

#include "CoreMinimal.h"
#include "WorldGenConfig.h"

class FMassifUpliftField;

/**
 * The terrain, as a pure function of world position:
 *
 *     Height(Position, SampleSpacing)
 *
 * The surface is the uplift envelope shaped by fractal base relief, with the
 * analytic erosion filter cutting everything below the base wavelength —
 * after Rune Skovbo Johansen's "Fast and Gorgeous Erosion Filter" (2026).
 * There is no grid and no solve: every point is evaluated from its own
 * inputs alone, which is what lets terrain be sampled in tiles, at any
 * resolution, without generating anything around them.
 *
 * SampleSpacing is the distance between the samples the caller intends to take.
 * Detail finer than a few times that spacing is dropped, and the finest octave
 * kept is faded rather than switched off, so a coarse sampling and a fine
 * sampling of the same point agree instead of stepping where they meet.
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

	/** Octaves worth evaluating at a spacing, and the weight the finest of them gets. */
	int32 OctavesForSpacing(double SampleSpacingMeters, float& OutFinestWeight) const;

private:
	/** Uplift envelope shaped by the fractal base relief; the surface the filter erodes. */
	double SampleBaseHeightMeters(FVector2d PositionMeters) const;

	/** Base-surface gradient by central differences at a fixed step, so it cannot depend on the caller. */
	FVector2d SampleBaseGradient(FVector2d PositionMeters) const;

	/** 0 at base level, 1 at the crest altitude overhead. Decides where the filter fades out. */
	float ComputeReliefFraction(FVector2d PositionMeters, double GroundHeightMeters) const;

	FWorldGenConfig Config;
	TUniquePtr<FMassifUpliftField> Uplift;
	uint32 BaseReliefSeed = 0;
	uint32 ErosionSeed = 0;
};
