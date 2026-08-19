#pragma once

#include "CoreMinimal.h"
#include "WorldGenConfig.h"

/** One point on a watercourse. Nodes chain to their parent, downstream towards a mouth. */
struct FRiverNode
{
	FVector2f PositionMeters = FVector2f::ZeroVector;
	float ElevationMeters = 0.0f;
	int32 ParentIndex = INDEX_NONE;
	/** Segments between this node and its mouth; 0 at the mouth. */
	int32 DownstreamDepth = 0;
	/** Nodes draining into this one. Stands in for discharge: high on trunks, zero at headwaters. */
	int32 UpstreamNodeCount = 0;
};

/** Terrain shape under one point, derived from the watercourses around it. */
struct FRiverTerrainSample
{
	/** Ground height with no noise detail, in meters. */
	float BaseHeightMeters = 0.0f;
	float DistanceToWaterMeters = 0.0f;
	float NearestWaterElevationMeters = 0.0f;
};

/**
 * Seeded river network grown inland from mouths on the map border, after
 * Genevaux et al., "Terrain Generation Using Procedural Models Based on
 * Hydrology" (2013). The graph is the terrain's skeleton: valleys exist
 * because a watercourse runs along them, and ridges are what is left between.
 *
 * It is small (hundreds of nodes for a whole map) and answers queries from
 * local data only, so any region can be generated without generating any
 * other.
 */
class FRiverNetwork
{
public:
	static FRiverNetwork Build(const FWorldGenConfig& Config);

	/**
	 * Every segment is tested. The graph is small, and any cut-off search
	 * radius shows up as a seam where a segment leaves the range: a low trunk
	 * far away legitimately wins over a high stream close by.
	 */
	FRiverTerrainSample Sample(FVector2f PositionMeters, const FWorldGenConfig& Config) const;

	const TArray<FRiverNode>& GetNodes() const
	{
		return Nodes;
	}

private:
	TArray<FRiverNode> Nodes;
};
