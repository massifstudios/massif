#pragma once

#include "CoreMinimal.h"
#include "WorldGenConfig.h"

class FMassifUpliftField;
class FRiverNetwork;

/**
 * The terrain, as a pure function of world position.
 *
 *     Height(Position, SampleSpacing)
 *
 * There is no heightfield. At 150 km across, a grid fine enough for the detail
 * we want could not exist — a centimeter lattice would be 10^15 samples — so
 * on-demand evaluation is not an optimisation, it is the only representation
 * that works. Points are evaluated in isolation, which is what lets terrain be
 * sampled in tiles, at any resolution, without generating anything around them.
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

	const FRiverNetwork& GetRiverNetwork() const
	{
		return *RiverNetwork;
	}

	int32 GetOrientationNodeCount() const
	{
		return OrientationSlope.Num();
	}

	/** Octaves worth evaluating at a spacing, and the weight the finest of them gets. */
	int32 OctavesForSpacing(double SampleSpacingMeters, float& OutFinestWeight) const;

private:
	/**
	 * Macro surface slope on a world-aligned lattice, built once.
	 *
	 * The erosion filter orients its stripes by this at four pivots per octave,
	 * up to a cell away, so it needs the slope at arbitrary points and needs it
	 * smooth. Evaluating the river graph at each of those pivots would cost
	 * more than the rest of a sample put together, and differencing the graph
	 * over a short step would hand the filter a slope blocky enough to
	 * stair-step the gullies.
	 *
	 * This is an acceleration structure, not the terrain: it holds gradient at
	 * a couple of hundred meters, and the height itself is always evaluated
	 * directly from the graph. Being world-aligned and built whole, it gives
	 * the same answer wherever a query comes from.
	 */
	void BuildOrientationLattice();

	FVector2d SampleOrientationSlope(FVector2d PositionMeters) const;

	int32 ClampOrientationNode(int32 NodeIndex) const;

	FWorldGenConfig Config;
	TUniquePtr<FMassifUpliftField> Uplift;
	TUniquePtr<FRiverNetwork> RiverNetwork;

	double OrientationSpacingMeters = 0.0;
	double OrientationOriginMeters = 0.0;
	int32 OrientationNodesPerSide = 0;
	TArray<FVector2f> OrientationSlope;
};
