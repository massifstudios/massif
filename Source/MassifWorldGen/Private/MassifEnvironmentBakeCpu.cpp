#include "Algo/Sort.h"
#include "Async/ParallelFor.h"
#include "HAL/PlatformAtomics.h"
#include "MassifEnvironmentBakeGrid.h"
#include "MassifHeightmap.h"
#include "Math/RandomStream.h"

namespace
{
	/** Sediment is accumulated as fixed-point tenths of a millimetre so the adds are integer and order-free. */
	constexpr double SedimentFixedPointScale = 10000.0;

	/** Standing water this deep already reads as saturated ground; below it, wetness fades in. */
	constexpr double StandingWaterSaturationMeters = 0.05;

	/** Droplets per unit of work handed to a worker thread. */
	constexpr int64 DropletsPerBatch = 4096;

	double AltitudeAt(const FMassifBakeGrid& Grid, TArrayView<const double> Altitudes, int32 Column, int32 Row)
	{
		const int32 ClampedColumn = FMath::Clamp(Column, 0, Grid.Width - 1);
		const int32 ClampedRow = FMath::Clamp(Row, 0, Grid.Height - 1);
		return Altitudes[Grid.Index(ClampedColumn, ClampedRow)];
	}
}

// ---------------------------------------------------------------------------
// Sampling the elevation model onto the bake grid
// ---------------------------------------------------------------------------

namespace
{
	void SampleAltitudeRow(
		const FMassifHeightmap& Heightmap, const FMassifBakeGrid& Grid, int32 Row, TArray<double>& OutMeasured)
	{
		for (int32 Column = 0; Column < Grid.Width; ++Column)
		{
			const FVector2d Position = Grid.CellCentreMeters(Column, Row);
			OutMeasured[Grid.Index(Column, Row)] = Heightmap.SampleHeightMeters(Position, Grid.CellSizeMeters);
		}
	}
}

void MassifBakeCpu::SampleAltitudes(
	const FMassifHeightmap& Heightmap, const FMassifBakeGrid& Grid, TArray<double>& OutMeasured)
{
	OutMeasured.SetNumUninitialized(Grid.CellCount());
	ParallelFor(Grid.Height, [&Heightmap, &Grid, &OutMeasured](int32 Row)
	{
		SampleAltitudeRow(Heightmap, Grid, Row, OutMeasured);
	});
}

// ---------------------------------------------------------------------------
// Depression filling, by priority flood from the world border inwards
// ---------------------------------------------------------------------------

namespace
{
	struct FFillEntry
	{
		double AltitudeMeters = 0.0;
		int32 CellIndex = 0;
	};

	/**
	 * Lowest first, and the cell index breaks ties. Without the tiebreak, cells
	 * at equal altitude would settle in whatever order the heap happened to give
	 * them, and a flat plateau would fill differently from one run to the next.
	 */
	struct FLowestFirst
	{
		bool operator()(const FFillEntry& Left, const FFillEntry& Right) const
		{
			if (Left.AltitudeMeters != Right.AltitudeMeters)
			{
				return Left.AltitudeMeters < Right.AltitudeMeters;
			}
			return Left.CellIndex < Right.CellIndex;
		}
	};

	/**
	 * Priority flood: the world border is where water leaves, so it starts
	 * settled at its measured altitude, and the surface grows inwards from the
	 * lowest unsettled edge of what has settled so far. A cell reached from a
	 * spill point can be no lower than that point, which is what raises hollows
	 * to their overflow level in a single pass.
	 */
	struct FDepressionFill
	{
		const FMassifBakeGrid& Grid;
		TArrayView<const double> Measured;
		TArray<double>& Filled;
		double EpsilonMeters = 0.0;

		TBitArray<> Settled;
		TArray<FFillEntry> Frontier;

		void Run()
		{
			Settled.Init(false, Grid.CellCount());
			Frontier.Reserve(2 * (Grid.Width + Grid.Height));
			SeedBorder();

			while (Frontier.Num() > 0)
			{
				FFillEntry Lowest;
				Frontier.HeapPop(Lowest, FLowestFirst(), EAllowShrinking::No);
				SettleNeighboursOf(Lowest);
			}
		}

		void SeedBorder()
		{
			for (int32 Column = 0; Column < Grid.Width; ++Column)
			{
				SettleAsOutlet(Grid.Index(Column, 0));
				SettleAsOutlet(Grid.Index(Column, Grid.Height - 1));
			}
			for (int32 Row = 1; Row < Grid.Height - 1; ++Row)
			{
				SettleAsOutlet(Grid.Index(0, Row));
				SettleAsOutlet(Grid.Index(Grid.Width - 1, Row));
			}
		}

		void SettleAsOutlet(int32 CellIndex)
		{
			Settled[CellIndex] = true;
			Filled[CellIndex] = Measured[CellIndex];
			Frontier.HeapPush(FFillEntry{ Measured[CellIndex], CellIndex }, FLowestFirst());
		}

		void SettleNeighboursOf(const FFillEntry& Entry)
		{
			const int32 Column = Entry.CellIndex % Grid.Width;
			const int32 Row = Entry.CellIndex / Grid.Width;
			for (int32 Direction = 0; Direction < MassifBakeNeighbours::Count; ++Direction)
			{
				Settle(
					Column + MassifBakeNeighbours::OffsetX[Direction],
					Row + MassifBakeNeighbours::OffsetY[Direction],
					Entry.AltitudeMeters);
			}
		}

		void Settle(int32 Column, int32 Row, double SpillAltitudeMeters)
		{
			if (!Grid.Contains(Column, Row))
			{
				return;
			}

			const int32 CellIndex = Grid.Index(Column, Row);
			if (Settled[CellIndex])
			{
				return;
			}

			Settled[CellIndex] = true;
			Filled[CellIndex] = FMath::Max(Measured[CellIndex], SpillAltitudeMeters + EpsilonMeters);
			Frontier.HeapPush(FFillEntry{ Filled[CellIndex], CellIndex }, FLowestFirst());
		}
	};
}

void MassifBakeCpu::FillDepressions(
	const FMassifBakeGrid& Grid,
	const FMassifEnvironmentBakeParams& Params,
	TArrayView<const double> Measured,
	TArray<double>& OutFilled)
{
	OutFilled.SetNumUninitialized(Grid.CellCount());
	FDepressionFill Fill{ Grid, Measured, OutFilled, Params.FillEpsilonMeters };
	Fill.Run();
}

void MassifBakeCpu::MeasureStandingWater(
	TArrayView<const double> Measured, TArrayView<const double> Filled, TArray<float>& OutDepthMeters)
{
	OutDepthMeters.SetNumUninitialized(Measured.Num());
	for (int32 CellIndex = 0; CellIndex < Measured.Num(); ++CellIndex)
	{
		OutDepthMeters[CellIndex] = static_cast<float>(FMath::Max(Filled[CellIndex] - Measured[CellIndex], 0.0));
	}
}

// ---------------------------------------------------------------------------
// Flow routing, one pass from the highest cell down
// ---------------------------------------------------------------------------

namespace
{
	struct FDownhillWeights
	{
		double PerDirection[MassifBakeNeighbours::Count] = {};
		double Total = 0.0;
	};

	double DownhillWeight(
		const FMassifBakeGrid& Grid,
		const FMassifEnvironmentBakeParams& Params,
		TArrayView<const double> Filled,
		int32 Column,
		int32 Row,
		double CellAltitudeMeters,
		int32 Direction)
	{
		const int32 NeighbourColumn = Column + MassifBakeNeighbours::OffsetX[Direction];
		const int32 NeighbourRow = Row + MassifBakeNeighbours::OffsetY[Direction];
		if (!Grid.Contains(NeighbourColumn, NeighbourRow))
		{
			return 0.0;
		}

		const double DropMeters = CellAltitudeMeters - Filled[Grid.Index(NeighbourColumn, NeighbourRow)];
		if (DropMeters <= 0.0)
		{
			return 0.0;
		}

		const double DistanceMeters = MassifBakeNeighbours::DistanceInCells[Direction] * Grid.CellSizeMeters;
		const double SlopeTangent = DropMeters / DistanceMeters;
		return FMath::Pow(SlopeTangent, Params.FlowConcentrationExponent)
			* MassifBakeNeighbours::ContourWidthInCells[Direction];
	}

	FDownhillWeights ComputeDownhillWeights(
		const FMassifBakeGrid& Grid,
		const FMassifEnvironmentBakeParams& Params,
		TArrayView<const double> Filled,
		int32 Column,
		int32 Row)
	{
		FDownhillWeights Weights;
		const double CellAltitudeMeters = Filled[Grid.Index(Column, Row)];
		for (int32 Direction = 0; Direction < MassifBakeNeighbours::Count; ++Direction)
		{
			Weights.PerDirection[Direction] =
				DownhillWeight(Grid, Params, Filled, Column, Row, CellAltitudeMeters, Direction);
			Weights.Total += Weights.PerDirection[Direction];
		}
		return Weights;
	}

	void AddNeighbourFlow(
		const FMassifBakeGrid& Grid,
		int32 Column,
		int32 Row,
		int32 Direction,
		double FlowSquareMeters,
		TArray<double>& Accumulation)
	{
		if (FlowSquareMeters <= 0.0)
		{
			return;
		}

		const int32 NeighbourColumn = Column + MassifBakeNeighbours::OffsetX[Direction];
		const int32 NeighbourRow = Row + MassifBakeNeighbours::OffsetY[Direction];
		if (!Grid.Contains(NeighbourColumn, NeighbourRow))
		{
			return;
		}

		Accumulation[Grid.Index(NeighbourColumn, NeighbourRow)] += FlowSquareMeters;
	}

	void DistributeCellFlow(
		const FMassifBakeGrid& Grid,
		const FMassifEnvironmentBakeParams& Params,
		TArrayView<const double> Filled,
		int32 CellIndex,
		TArray<double>& Accumulation)
	{
		const int32 Column = CellIndex % Grid.Width;
		const int32 Row = CellIndex / Grid.Width;
		const FDownhillWeights Weights = ComputeDownhillWeights(Grid, Params, Filled, Column, Row);
		if (Weights.Total <= 0.0)
		{
			// Nothing below it: the cell is on the border and this water leaves the world.
			return;
		}

		const double OutflowSquareMeters = Accumulation[CellIndex];
		for (int32 Direction = 0; Direction < MassifBakeNeighbours::Count; ++Direction)
		{
			const double Share = Weights.PerDirection[Direction] / Weights.Total;
			AddNeighbourFlow(Grid, Column, Row, Direction, OutflowSquareMeters * Share, Accumulation);
		}
	}

	/**
	 * Cells highest first. On a surface with no local minima this is a drainage
	 * order: by the time a cell is reached, everything that drains into it has
	 * already handed over its water.
	 */
	TArray<int32> MakeDrainageOrder(TArrayView<const double> Filled)
	{
		TArray<int32> Order;
		Order.SetNumUninitialized(Filled.Num());
		for (int32 CellIndex = 0; CellIndex < Filled.Num(); ++CellIndex)
		{
			Order[CellIndex] = CellIndex;
		}

		Algo::Sort(Order, [Filled](int32 Left, int32 Right)
		{
			if (Filled[Left] != Filled[Right])
			{
				return Filled[Left] > Filled[Right];
			}
			return Left < Right;
		});
		return Order;
	}

	/**
	 * Hands every cell's holding to the cells below it, highest first, and reads
	 * the totals back out.
	 *
	 * Accumulated in double: a valley cell gathers millions of hillside
	 * contributions, and in float the small ones would stop registering long
	 * before the river was drawn.
	 */
	void RouteDownhill(
		const FMassifBakeGrid& Grid,
		const FMassifEnvironmentBakeParams& Params,
		TArrayView<const double> Filled,
		TArray<double>& InOutAccumulation,
		TArray<float>& OutRouted)
	{
		const TArray<int32> DrainageOrder = MakeDrainageOrder(Filled);
		for (int32 CellIndex : DrainageOrder)
		{
			DistributeCellFlow(Grid, Params, Filled, CellIndex, InOutAccumulation);
		}

		OutRouted.SetNumUninitialized(Grid.CellCount());
		for (int32 CellIndex = 0; CellIndex < InOutAccumulation.Num(); ++CellIndex)
		{
			OutRouted[CellIndex] = static_cast<float>(InOutAccumulation[CellIndex]);
		}
	}
}

void MassifBakeCpu::RouteFlow(
	const FMassifBakeGrid& Grid,
	const FMassifEnvironmentBakeParams& Params,
	TArrayView<const double> Filled,
	TArray<float>& OutAccumulationSquareMeters)
{
	TArray<double> Accumulation;
	Accumulation.Init(Grid.CellAreaSquareMeters(), Grid.CellCount());
	RouteDownhill(Grid, Params, Filled, Accumulation, OutAccumulationSquareMeters);
}

void MassifBakeCpu::RouteInflow(
	const FMassifBakeGrid& Grid,
	const FMassifEnvironmentBakeParams& Params,
	TArrayView<const double> Filled,
	TArrayView<const double> InflowPerCell,
	TArray<float>& OutRouted)
{
	check(InflowPerCell.Num() == Grid.CellCount());
	TArray<double> Accumulation(InflowPerCell.GetData(), InflowPerCell.Num());
	RouteDownhill(Grid, Params, Filled, Accumulation, OutRouted);
}

// ---------------------------------------------------------------------------
// Droplets, and the soil they leave behind
// ---------------------------------------------------------------------------

namespace
{
	struct FDropletState
	{
		FVector2d PositionInCells = FVector2d::ZeroVector;
		FVector2d Direction = FVector2d::ZeroVector;
		double Speed = 0.0;
		double WaterVolume = 0.0;
		double SedimentMeters = 0.0;
	};

	/**
	 * Runs droplets over a surface it never modifies, recording only the sediment
	 * balance each cell ends up with. Shaping the ground is a separate, later
	 * concern (ARCHITECTURE §5.1.1); what this measures is where the loose
	 * material on an already-shaped massif would be sitting.
	 *
	 * Every droplet is therefore independent of every other, and the only shared
	 * state is the balance buffer, written through integer atomics — so the
	 * answer does not depend on how the droplets were split across threads.
	 */
	struct FDropletRunner
	{
		const FMassifBakeGrid& Grid;
		const FMassifEnvironmentBakeParams& Params;
		TArrayView<const double> Filled;
		TArray<int64>& SedimentBalanceFixed;
		uint32 DropletSeed = 0;

		void RunBatch(int64 FirstDroplet, int64 DropletCount) const
		{
			for (int64 Droplet = FirstDroplet; Droplet < FirstDroplet + DropletCount; ++Droplet)
			{
				RunDroplet(Droplet);
			}
		}

		void RunDroplet(int64 DropletIndex) const
		{
			FDropletState Droplet;
			Droplet.PositionInCells = MakeStartPosition(DropletIndex);
			Droplet.Speed = Params.DropletInitialSpeed;
			Droplet.WaterVolume = Params.DropletInitialWaterVolume;

			for (int32 Step = 0; Step < Params.MaxDropletSteps; ++Step)
			{
				if (!Advance(Droplet))
				{
					break;
				}
			}

			// Whatever it still carries when it runs out of steps settles here.
			ScatterSediment(Droplet.PositionInCells, Droplet.SedimentMeters);
		}

		/** Droplets sit on the cell lattice with a seeded offset, so they cover the world evenly without lining up. */
		FVector2d MakeStartPosition(int64 DropletIndex) const
		{
			const int32 LatticeCell = static_cast<int32>(DropletIndex % Grid.CellCount());
			const uint32 StreamSeed = HashCombine(DropletSeed, GetTypeHash(DropletIndex));
			FRandomStream Stream(static_cast<int32>(StreamSeed));
			return FVector2d(
				LatticeCell % Grid.Width + Stream.FRand(),
				LatticeCell / Grid.Width + Stream.FRand());
		}

		bool Advance(FDropletState& Droplet) const
		{
			FVector2d Gradient;
			const double HeightMeters = SampleSurface(Droplet.PositionInCells, Gradient);
			Droplet.Direction = Steer(Droplet.Direction, Gradient);
			if (Droplet.Direction.IsNearlyZero())
			{
				return false;
			}

			const FVector2d NextPosition = Droplet.PositionInCells + Droplet.Direction;
			if (!IsInside(NextPosition))
			{
				// It left the world and took its load with it.
				Droplet.SedimentMeters = 0.0;
				return false;
			}

			FVector2d NextGradient;
			const double NextHeightMeters = SampleSurface(NextPosition, NextGradient);

			// Slope, not the drop across one cell: a step covers one cell, so a
			// raw height difference would make the same ground erode harder at a
			// coarser bake resolution.
			const double DownhillSlope = (HeightMeters - NextHeightMeters) / Grid.CellSizeMeters;
			SettleOrPickUp(Droplet, DownhillSlope);

			Droplet.Speed = FMath::Min(
				FMath::Sqrt(FMath::Max(
					Droplet.Speed * Droplet.Speed + DownhillSlope * Params.DropletGravity, 0.0)),
				Params.DropletMaxSpeed);
			Droplet.WaterVolume *= 1.0 - Params.DropletEvaporationRate;
			Droplet.PositionInCells = NextPosition;
			return true;
		}

		FVector2d Steer(FVector2d Direction, FVector2d Gradient) const
		{
			const FVector2d Steered =
				Direction * Params.DropletInertia - Gradient * (1.0 - Params.DropletInertia);
			return Steered.GetSafeNormal();
		}

		/**
		 * A droplet carries what its speed and water let it: fast water on a steep
		 * drop scours, and the moment the ground flattens under it the surplus
		 * falls out. That is why sediment piles up on valley floors and benches
		 * rather than on the faces above them.
		 */
		void SettleOrPickUp(FDropletState& Droplet, double DownhillSlope) const
		{
			const double Capacity = FMath::Max(
				DownhillSlope * Droplet.Speed * Droplet.WaterVolume * Params.DropletCapacityFactor,
				Params.DropletMinCapacity);

			if (DownhillSlope <= 0.0)
			{
				// Ran into rising ground and stalled: everything it carried settles here.
				Deposit(Droplet, Droplet.SedimentMeters);
				return;
			}
			if (Droplet.SedimentMeters > Capacity)
			{
				Deposit(Droplet, (Droplet.SedimentMeters - Capacity) * Params.DropletDepositionRate);
				return;
			}

			const double PickUpMeters = FMath::Min(
				(Capacity - Droplet.SedimentMeters) * Params.DropletErosionRate,
				Params.DropletMaxErosionMetersPerStep);
			Droplet.SedimentMeters += PickUpMeters;
			ScatterSediment(Droplet.PositionInCells, -PickUpMeters);
		}

		void Deposit(FDropletState& Droplet, double AmountMeters) const
		{
			Droplet.SedimentMeters -= AmountMeters;
			ScatterSediment(Droplet.PositionInCells, AmountMeters);
		}

		bool IsInside(FVector2d PositionInCells) const
		{
			return PositionInCells.X >= 0.0 && PositionInCells.Y >= 0.0
				&& PositionInCells.X <= Grid.Width - 1.0 && PositionInCells.Y <= Grid.Height - 1.0;
		}

		/** Bilinear height at a continuous cell position, with the gradient the same interpolation implies. */
		double SampleSurface(FVector2d PositionInCells, FVector2d& OutGradient) const
		{
			const int32 Column = FMath::Clamp(FMath::FloorToInt32(PositionInCells.X), 0, Grid.Width - 2);
			const int32 Row = FMath::Clamp(FMath::FloorToInt32(PositionInCells.Y), 0, Grid.Height - 2);
			const double FractionX = PositionInCells.X - Column;
			const double FractionY = PositionInCells.Y - Row;

			const double NorthWest = AltitudeAt(Grid, Filled, Column, Row);
			const double NorthEast = AltitudeAt(Grid, Filled, Column + 1, Row);
			const double SouthWest = AltitudeAt(Grid, Filled, Column, Row + 1);
			const double SouthEast = AltitudeAt(Grid, Filled, Column + 1, Row + 1);

			OutGradient = FVector2d(
				FMath::Lerp(NorthEast - NorthWest, SouthEast - SouthWest, FractionY),
				FMath::Lerp(SouthWest - NorthWest, SouthEast - NorthEast, FractionX));
			return FMath::Lerp(
				FMath::Lerp(NorthWest, NorthEast, FractionX),
				FMath::Lerp(SouthWest, SouthEast, FractionX),
				FractionY);
		}

		void ScatterSediment(FVector2d PositionInCells, double AmountMeters) const
		{
			if (FMath::IsNearlyZero(AmountMeters))
			{
				return;
			}

			const int32 Column = FMath::FloorToInt32(PositionInCells.X);
			const int32 Row = FMath::FloorToInt32(PositionInCells.Y);
			const double FractionX = PositionInCells.X - Column;
			const double FractionY = PositionInCells.Y - Row;

			AddToCell(Column, Row, AmountMeters * (1.0 - FractionX) * (1.0 - FractionY));
			AddToCell(Column + 1, Row, AmountMeters * FractionX * (1.0 - FractionY));
			AddToCell(Column, Row + 1, AmountMeters * (1.0 - FractionX) * FractionY);
			AddToCell(Column + 1, Row + 1, AmountMeters * FractionX * FractionY);
		}

		void AddToCell(int32 Column, int32 Row, double AmountMeters) const
		{
			if (!Grid.Contains(Column, Row))
			{
				return;
			}

			const int64 FixedAmount = FMath::RoundToInt64(AmountMeters * SedimentFixedPointScale);
			if (FixedAmount == 0)
			{
				return;
			}

			FPlatformAtomics::InterlockedAdd(&SedimentBalanceFixed[Grid.Index(Column, Row)], FixedAmount);
		}
	};
}

namespace
{
	/**
	 * Net sediment balance to the soil actually lying there. Deposition simply
	 * adds up to the deepest soil holds, but removal only thins: bedrock keeps
	 * weathering into fresh regolith, and it weathers fastest where the cover is
	 * already thin, so scoured ground approaches bare rock rather than arriving
	 * at it. Without that a slope steep enough to be worked at all would come out
	 * as bare rock, and every scree slope in the world with it.
	 */
	double SoilDepthFromBalance(const FMassifEnvironmentBakeParams& Params, double BalanceMeters)
	{
		if (BalanceMeters >= 0.0)
		{
			return FMath::Min(Params.BaseSoilDepthMeters + BalanceMeters, Params.MaxSoilDepthMeters);
		}
		return Params.BaseSoilDepthMeters * FMath::Exp(BalanceMeters / Params.SoilRemovalScaleMeters);
	}
}

void MassifBakeCpu::RunDroplets(
	const FMassifBakeGrid& Grid,
	const FMassifEnvironmentBakeParams& Params,
	uint32 DropletSeed,
	TArrayView<const double> Filled,
	TArray<float>& OutSoilDepthMeters)
{
	TArray<int64> SedimentBalanceFixed;
	SedimentBalanceFixed.SetNumZeroed(Grid.CellCount());

	const int64 DropletCount = FMath::Max<int64>(
		FMath::RoundToInt64(Params.DropletsPerCell * Grid.CellCount()), 0);
	const FDropletRunner Runner{ Grid, Params, Filled, SedimentBalanceFixed, DropletSeed };
	const int32 BatchCount = static_cast<int32>(FMath::DivideAndRoundUp(DropletCount, DropletsPerBatch));

	ParallelFor(BatchCount, [&Runner, DropletCount](int32 BatchIndex)
	{
		const int64 FirstDroplet = BatchIndex * DropletsPerBatch;
		Runner.RunBatch(FirstDroplet, FMath::Min(DropletsPerBatch, DropletCount - FirstDroplet));
	});

	OutSoilDepthMeters.SetNumUninitialized(Grid.CellCount());
	for (int32 CellIndex = 0; CellIndex < Grid.CellCount(); ++CellIndex)
	{
		const double BalanceMeters = SedimentBalanceFixed[CellIndex] / SedimentFixedPointScale;
		OutSoilDepthMeters[CellIndex] =
			static_cast<float>(SoilDepthFromBalance(Params, BalanceMeters));
	}
}

// ---------------------------------------------------------------------------
// Wetness
// ---------------------------------------------------------------------------

namespace
{
	double MeasuredSlopeTangent(
		const FMassifBakeGrid& Grid, TArrayView<const double> Measured, int32 Column, int32 Row)
	{
		const double West = AltitudeAt(Grid, Measured, Column - 1, Row);
		const double East = AltitudeAt(Grid, Measured, Column + 1, Row);
		const double North = AltitudeAt(Grid, Measured, Column, Row - 1);
		const double South = AltitudeAt(Grid, Measured, Column, Row + 1);

		const double SlopeX = (East - West) / (2.0 * Grid.CellSizeMeters);
		const double SlopeY = (South - North) / (2.0 * Grid.CellSizeMeters);
		return FMath::Sqrt(SlopeX * SlopeX + SlopeY * SlopeY);
	}

	double CellWetness(
		const FMassifBakeGrid& Grid,
		const FMassifEnvironmentBakeParams& Params,
		TArrayView<const double> Measured,
		TArrayView<const float> AccumulationSquareMeters,
		TArrayView<const float> StandingWaterDepthMeters,
		int32 Column,
		int32 Row)
	{
		const int32 CellIndex = Grid.Index(Column, Row);
		const double SlopeTangent = FMath::Max(
			MeasuredSlopeTangent(Grid, Measured, Column, Row), Params.MinimumSlopeTangent);

		// Contributing area per unit of contour width, which is what the wetness
		// index is defined against — a wide catchment draining through a narrow
		// cell is what makes a valley floor wet.
		const double SpecificAreaMeters = FMath::Max(
			AccumulationSquareMeters[CellIndex] / Grid.CellSizeMeters, UE_DOUBLE_SMALL_NUMBER);
		const double WetnessIndex = FMath::Loge(SpecificAreaMeters / SlopeTangent);

		const double DrainedWetness =
			FMath::SmoothStep(Params.DryWetnessIndex, Params.SaturatedWetnessIndex, WetnessIndex);
		const double PondedWetness = FMath::Clamp(
			StandingWaterDepthMeters[CellIndex] / StandingWaterSaturationMeters, 0.0, 1.0);
		return FMath::Max(DrainedWetness, PondedWetness);
	}

	void ComputeWetnessRow(
		const FMassifBakeGrid& Grid,
		const FMassifEnvironmentBakeParams& Params,
		TArrayView<const double> Measured,
		TArrayView<const float> AccumulationSquareMeters,
		TArrayView<const float> StandingWaterDepthMeters,
		int32 Row,
		TArray<float>& OutWetness)
	{
		for (int32 Column = 0; Column < Grid.Width; ++Column)
		{
			OutWetness[Grid.Index(Column, Row)] = static_cast<float>(CellWetness(
				Grid, Params, Measured, AccumulationSquareMeters, StandingWaterDepthMeters, Column, Row));
		}
	}
}

void MassifBakeCpu::ComputeWetness(
	const FMassifBakeGrid& Grid,
	const FMassifEnvironmentBakeParams& Params,
	TArrayView<const double> Measured,
	TArrayView<const float> AccumulationSquareMeters,
	TArrayView<const float> StandingWaterDepthMeters,
	TArray<float>& OutWetness)
{
	OutWetness.SetNumUninitialized(Grid.CellCount());
	ParallelFor(Grid.Height, [&](int32 Row)
	{
		ComputeWetnessRow(
			Grid, Params, Measured, AccumulationSquareMeters, StandingWaterDepthMeters, Row, OutWetness);
	});
}
