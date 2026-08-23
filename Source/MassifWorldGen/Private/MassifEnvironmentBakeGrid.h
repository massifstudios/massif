#pragma once

#include "CoreMinimal.h"
#include "MassifEnvironmentBakeParams.h"
#include "MassifRiverNetwork.h"

class FMassifHeightmap;

/**
 * The raster the bake solves on: column 0 west, row 0 north, cell centres in
 * world metres with the origin at the centre of the world, matching the
 * elevation model's own layout.
 */
struct FMassifBakeGrid
{
	int32 Width = 0;
	int32 Height = 0;
	double CellSizeMeters = 0.0;
	double WorldSizeXMeters = 0.0;
	double WorldSizeYMeters = 0.0;

	static FMassifBakeGrid FromHeightmap(const FMassifHeightmap& Heightmap, double CellSizeMeters);

	int32 CellCount() const
	{
		return Width * Height;
	}

	int32 Index(int32 Column, int32 Row) const
	{
		return Row * Width + Column;
	}

	bool Contains(int32 Column, int32 Row) const
	{
		return Column >= 0 && Column < Width && Row >= 0 && Row < Height;
	}

	bool IsBorder(int32 Column, int32 Row) const
	{
		return Column == 0 || Row == 0 || Column == Width - 1 || Row == Height - 1;
	}

	double CellAreaSquareMeters() const
	{
		return CellSizeMeters * CellSizeMeters;
	}

	FVector2d CellCentreMeters(int32 Column, int32 Row) const;
};

/** The eight neighbours of a cell, and how far away each one is in cell widths. */
namespace MassifBakeNeighbours
{
	inline constexpr int32 Count = 8;
	extern const int32 OffsetX[Count];
	extern const int32 OffsetY[Count];
	extern const double DistanceInCells[Count];

	/** Share of the cell's outflow boundary that faces this neighbour, as multiple-flow routing weights by. */
	extern const double ContourWidthInCells[Count];
}

/**
 * The CPU reference solve: the definition of every stage, and what the
 * automation suite runs (ARCHITECTURE §5.6). Exact where the GPU path relaxes
 * — depressions are filled by priority flood rather than by iteration, and
 * flow is routed in one pass over cells sorted by altitude rather than pushed
 * repeatedly — so the reference has no iteration count to converge against.
 */
namespace MassifBakeCpu
{
	void SampleAltitudes(const FMassifHeightmap& Heightmap, const FMassifBakeGrid& Grid, TArray<double>& OutMeasured);

	/**
	 * Raises every hollow to the level it would overflow at, so the surface
	 * descends without interruption from anywhere to the world border. Each cell
	 * comes out strictly below the neighbour it was reached from, which is what
	 * lets the flow routing treat altitude order as a drainage order.
	 */
	void FillDepressions(
		const FMassifBakeGrid& Grid,
		const FMassifEnvironmentBakeParams& Params,
		TArrayView<const double> Measured,
		TArray<double>& OutFilled);

	void MeasureStandingWater(
		TArrayView<const double> Measured, TArrayView<const double> Filled, TArray<float>& OutDepthMeters);

	/**
	 * Square metres draining through each cell. Every cell starts with its own
	 * area and hands it to the neighbours below it, split by gradient, in one
	 * pass from the highest cell to the lowest — an order that is a valid
	 * drainage order precisely because the filled surface has no local minima.
	 */
	void RouteFlow(
		const FMassifBakeGrid& Grid,
		const FMassifEnvironmentBakeParams& Params,
		TArrayView<const double> Filled,
		TArray<float>& OutAccumulationSquareMeters);

	/**
	 * The same routing, over what each cell contributes rather than over its area:
	 * a cell hands downstream the water that landed on it, so what comes out is
	 * discharge and not catchment. Whatever unit the contributions are in is the
	 * unit of the result.
	 *
	 * Splitting this out of RouteFlow rather than weighting it is deliberate. The
	 * catchment area a cell drains is a property of the terrain that the wetness
	 * index and the channel-initiation threshold both read as an area, and it does
	 * not become something else because the rain over it is uneven.
	 */
	void RouteInflow(
		const FMassifBakeGrid& Grid,
		const FMassifEnvironmentBakeParams& Params,
		TArrayView<const double> Filled,
		TArrayView<const double> InflowPerCell,
		TArray<float>& OutRouted);

	/**
	 * Runs water down the surface as particles and records where each one dropped
	 * what it was carrying. The surface is never modified — this measures the
	 * soil an already-shaped massif would have, and shaping the ground is a
	 * separate concern (ARCHITECTURE §5.1.1).
	 *
	 * Droplets are independent of each other and scatter through integer atomics
	 * on a fixed-point encoding, so the result does not depend on how the work
	 * was split across threads.
	 */
	void RunDroplets(
		const FMassifBakeGrid& Grid,
		const FMassifEnvironmentBakeParams& Params,
		uint32 DropletSeed,
		TArrayView<const double> Filled,
		TArray<float>& OutSoilDepthMeters);

	/**
	 * Soil moisture from the topographic wetness index: how much ground drains
	 * through a cell against how fast the cell sheds it. Ground under standing
	 * water is saturated outright.
	 */
	void ComputeWetness(
		const FMassifBakeGrid& Grid,
		const FMassifEnvironmentBakeParams& Params,
		TArrayView<const double> Measured,
		TArrayView<const float> AccumulationSquareMeters,
		TArrayView<const float> StandingWaterDepthMeters,
		TArray<float>& OutWetness);

	/**
	 * Traces the channels out of the solved drainage: where enough catchment has
	 * gathered to make a river, and the path each one takes to the world border.
	 *
	 * Reads the working surfaces rather than the finished channels because it has
	 * to. Altitudes are quantised to four centimetres on the way into a channel,
	 * and the drop the depression fill adds to keep a flat descending is a ten
	 * thousandth of that, so a river traced against the stored surface would lose
	 * its way across every lake bed in the world.
	 */
	TSharedRef<const FMassifRiverNetwork> ExtractRivers(
		const FMassifBakeGrid& Grid,
		const FMassifRiverParams& Params,
		TArrayView<const double> Measured,
		TArrayView<const double> Filled,
		TArrayView<const float> AccumulationSquareMeters,
		TArrayView<const float> DischargeCubicMetersPerSecond);
}
