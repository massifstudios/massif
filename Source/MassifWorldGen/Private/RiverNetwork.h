#pragma once

#include "CoreMinimal.h"

struct FWorldGenConfig;
class FMassifUpliftField;

/** One point on a watercourse. Nodes chain to their parent, downstream towards a mouth. */
struct FRiverNode
{
	FVector2d PositionMeters = FVector2d::ZeroVector;
	double ElevationMeters = 0.0;
	int32 ParentIndex = INDEX_NONE;
	/** Segments between this node and its mouth; 0 at the mouth. */
	int32 DownstreamDepth = 0;
	/** Nodes draining into this one. Stands in for discharge: high on trunks, zero at headwaters. */
	int32 UpstreamNodeCount = 0;
};

/** Terrain shape under one point, derived from the watercourses around it. */
struct FRiverTerrainSample
{
	/** Ground height with no erosion detail, in meters. */
	double HeightMeters = 0.0;
	double DistanceToWaterMeters = 0.0;
	double NearestWaterElevationMeters = 0.0;
	/** 0 on the valley floor, 1 at the crest altitude the uplift field sets overhead. */
	float ReliefFraction = 0.0f;
};

/**
 * Seeded river network grown inland from mouths on the world border, after
 * Genevaux et al., "Terrain Generation Using Procedural Models Based on
 * Hydrology" (2013). The graph is the terrain's skeleton: valleys exist
 * because a watercourse runs along them, and ridges are what is left between.
 *
 * Segments shorten as a watercourse climbs, so one graph spans a 3 km trunk
 * valley and the 400 m streams feeding it. Over a 150 km world that is a few
 * hundred thousand nodes — a few megabytes, built in one pass at startup.
 * It stays global because a valley floor's elevation depends on the whole
 * path to its outlet, which no amount of chunking can make local.
 */
class FRiverNetwork
{
public:
	static FRiverNetwork Build(const FWorldGenConfig& Config, const FMassifUpliftField& Uplift);

	FRiverTerrainSample Sample(FVector2d PositionMeters, const FWorldGenConfig& Config, const FMassifUpliftField& Uplift) const;

	const TArray<FRiverNode>& GetNodes() const
	{
		return Nodes;
	}

private:
	/**
	 * Segments bucketed into a uniform grid, so a query touches only the
	 * watercourses near it instead of all of them. Each segment is registered in
	 * every cell its extent overlaps.
	 */
	void BuildSegmentIndex(const FWorldGenConfig& Config);

	/**
	 * Radius beyond which no watercourse can still win. Valley sides keep
	 * climbing with distance, so past the point where even the lowest possible
	 * water in the world would propose more ground than we have already found,
	 * nothing farther can lower the answer. Searching to a fixed radius instead
	 * would show as a seam wherever a distant trunk legitimately wins.
	 */
	double LowestPossibleProposalBeyond(double RadiusMeters, double CrestAltitudeMeters, const FWorldGenConfig& Config) const;

	void GatherRingCells(int32 RingIndex, int32 CenterCellX, int32 CenterCellY, TArray<int32>& OutCells) const;

	/** Closest a point in this cell can be to the query. */
	double CellMinDistanceMeters(int32 Cell, FVector2d PositionMeters) const;

	/** Folds one cell's segments into the running best two proposals, or skips the cell whole. */
	void AccumulateCell(
		int32 Cell, FVector2d PositionMeters, double CrestAltitudeMeters, const FWorldGenConfig& Config,
		double& LowestHeight, double& SecondLowestHeight, FRiverTerrainSample& Result) const;

	TArray<FRiverNode> Nodes;

	double IndexCellSizeMeters = 0.0;
	int32 IndexCellsPerSide = 0;
	double IndexOriginMeters = 0.0;
	/** Start of each cell's run in IndexedSegments; one extra entry closes the last cell. */
	TArray<int32> IndexCellStarts;
	TArray<int32> IndexedSegments;

	/**
	 * Lowest water any segment in a cell carries. Every segment is registered in
	 * the cell holding its closest point to any query, so a cell whose best
	 * conceivable proposal already loses cannot hold a winner — which is most
	 * cells in every ring but the first, and skipping them is the difference
	 * between scanning a couple of hundred segments and a couple of thousand.
	 */
	TArray<float> IndexCellMinWaterElevation;
};
