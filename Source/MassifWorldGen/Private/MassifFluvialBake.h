#pragma once

#include "CoreMinimal.h"

struct FWorldGenConfig;
class FMassifUpliftField;

/** One point on an extracted watercourse; chains to its parent downstream. */
struct FMassifFluvialRiverNode
{
	FVector2d PositionMeters = FVector2d::ZeroVector;
	double WaterElevationMeters = 0.0;
	/** Drained surface at the upstream end of this node's segment. Stands in for discharge. */
	double DrainageAreaSquareMeters = 0.0;
	/** Downstream node this one flows into; INDEX_NONE at an outlet on the world border. */
	int32 ParentIndex = INDEX_NONE;
};

/**
 * The macro terrain, solved once per seed: a stream-power erosion solve over
 * the whole world on a coarse grid, with the world border as base level.
 *
 * Drainage is global — a valley floor's elevation depends on the whole path to
 * its outlet — and this is where it is resolved: every cell drains to the
 * border through the D8 receiver graph of the solve, so watersheds, discharge
 * and outlets are consistent by construction. Everything finer than the grid
 * is analytic per point on top of it, so the bake stays bounded in memory and
 * time no matter where the player goes. It lives in memory only and is
 * regenerated from the seed; nothing here is ever serialized.
 *
 * Immutable after Build. Sampling touches only the solved grids and the river
 * segment index, so it is thread-safe.
 */
class FMassifFluvialBake
{
public:
	static FMassifFluvialBake Build(const FWorldGenConfig& Config, const FMassifUpliftField& Uplift);

	/** Macro ground altitude, bicubic over the bake grid. */
	double SampleHeightMeters(FVector2d PositionMeters) const;

	/** Macro surface gradient, meters of rise per meter travelled, bicubic. */
	FVector2d SampleGradientMetersPerMeter(FVector2d PositionMeters) const;

	/** Water elevation of the channel the ground here drains to, bilinear. */
	double SampleDrainWaterElevationMeters(FVector2d PositionMeters) const;

	/**
	 * Meters to cut below the macro surface for the channels near this point.
	 * The bake grid already carries every valley wider than a couple of cells;
	 * this supplies only the sub-cell channel sharpness along the extracted
	 * river polylines, so its reach is capped by the channel config.
	 */
	double SampleChannelCarveMeters(FVector2d PositionMeters, double MacroHeightMeters, const FWorldGenConfig& Config) const;

	const TArray<FMassifFluvialRiverNode>& GetRiverNodes() const
	{
		return RiverNodes;
	}

	int32 GetCellsPerSide() const
	{
		return CellsPerSide;
	}

	/** Receiver cell of each cell on the final surface; a border cell receives itself. */
	const TArray<int32>& GetReceiverIndices() const
	{
		return ReceiverIndices;
	}

	const TArray<double>& GetDrainageAreasSquareMeters() const
	{
		return DrainageAreaSquareMeters;
	}

	const TArray<double>& GetHeightGridMeters() const
	{
		return HeightGridMeters;
	}

	bool IsBorderCell(int32 CellIndex) const;

	/** Farthest a channel carve can reach from its polyline, from the channel config caps. */
	static double CarveReachMeters(const FWorldGenConfig& Config);

	/** Hash of the solved height grid, for determinism tests. */
	uint64 ComputeGridHash() const;

private:
	void ComputeGradientGrid();
	void ComputeDrainWaterGrid(const TArray<int32>& PopOrder, double ChannelAreaThresholdSquareMeters);
	void BlurDrainWaterRow(int32 CellY, TArray<double>& Blurred) const;
	void ExtractRiverPolylines(const TArray<int32>& PopOrder, double ChannelAreaThresholdSquareMeters);
	int32 NextChannelCellDownstream(int32 CellIndex, const TArray<int32>& NodeOfCell) const;
	void SmoothRiverPolylines();
	FVector2d SmoothedNodePosition(int32 NodeIndex, const TArray<int32>& SingleChildOfNode, const TArray<int32>& ChildCounts) const;
	void BuildRiverSegmentIndex(const FWorldGenConfig& Config);

	int32 ClampGridNode(int32 NodeIndex) const;
	FVector2d CellPositionMeters(int32 CellIndex) const;

	/**
	 * All sampling goes through a smooth domain warp. The solve runs on a
	 * square lattice and D8 leaves channel segments locked to its eight
	 * headings; warping the query by a couple of cell sizes of low-frequency
	 * noise bends every straight lattice line without touching the solve, and
	 * the polylines warp identically because the carve queries warp too.
	 */
	FVector2d WarpedPositionMeters(FVector2d PositionMeters) const;

	/** Best two channel cuts near a point, so crossing channels can merge smoothly. */
	void GatherChannelCuts(
		FVector2d PositionMeters, double MacroHeightMeters, const FWorldGenConfig& Config,
		double& OutDeepestCutMeters, double& OutSecondDeepestCutMeters) const;

	void AccumulateChannelCell(
		int32 IndexCell, FVector2d PositionMeters, double MacroHeightMeters, const FWorldGenConfig& Config,
		double& InOutDeepestCutMeters, double& InOutSecondDeepestCutMeters) const;

	double ChannelCutMeters(int32 NodeIndex, FVector2d PositionMeters, double MacroHeightMeters, const FWorldGenConfig& Config) const;

	int32 CellsPerSide = 0;
	double CellSizeMeters = 0.0;
	double OriginMeters = 0.0;

	double SampleWarpMeters = 0.0;
	double SampleWarpWavelengthMeters = 1.0;
	uint32 SampleWarpSeedX = 0;
	uint32 SampleWarpSeedY = 0;

	TArray<double> HeightGridMeters;
	TArray<double> DrainageAreaSquareMeters;
	TArray<double> DrainWaterElevationMeters;
	TArray<FVector2f> GradientMetersPerMeter;
	TArray<int32> ReceiverIndices;

	TArray<FMassifFluvialRiverNode> RiverNodes;

	double RiverIndexCellSizeMeters = 0.0;
	int32 RiverIndexCellsPerSide = 0;
	double RiverIndexOriginMeters = 0.0;
	/** Start of each cell's run in RiverIndexedSegments; one extra entry closes the last cell. */
	TArray<int32> RiverIndexCellStarts;
	TArray<int32> RiverIndexedSegments;
};
