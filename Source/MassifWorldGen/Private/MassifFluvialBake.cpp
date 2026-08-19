#include "MassifFluvialBake.h"

#include "MassifNoise.h"
#include "MassifSeed.h"
#include "MassifStateHash.h"
#include "MassifUpliftField.h"
#include "WorldGenConfig.h"

namespace
{
	/**
	 * Raise a flooded cell gets over the cell that flooded it, so filled lakes
	 * still drain instead of sitting flat with no steepest descent.
	 */
	const double FillEpsilonMeters = 0.0001;

	/**
	 * Noise added to the initial surface so the uplift field's smooth ceilings
	 * do not hand the flood fill kilometre-wide plateaus with no descent to
	 * choose. Small against the relief; the solve carves far past it.
	 */
	const double TieBreakAmplitudeMeters = 1.0;
	const double TieBreakWavelengthCells = 3.0;

	const int32 NeighborStepsX[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
	const int32 NeighborStepsY[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };

	float CubicInterpolate(float Before, float Low, float High, float After, float Fraction)
	{
		const float Slope = 0.5f * (High - Before);
		const float NextSlope = 0.5f * (After - Low);
		const float Delta = High - Low;
		const float Cubic = Slope + NextSlope - 2.0f * Delta;
		const float Quadratic = 3.0f * Delta - 2.0f * Slope - NextSlope;
		return Low + Fraction * (Slope + Fraction * (Quadratic + Fraction * Cubic));
	}

	double CubicInterpolateDouble(double Before, double Low, double High, double After, double Fraction)
	{
		const double Slope = 0.5 * (High - Before);
		const double NextSlope = 0.5 * (After - Low);
		const double Delta = High - Low;
		const double Cubic = Slope + NextSlope - 2.0 * Delta;
		const double Quadratic = 3.0 * Delta - 2.0 * Slope - NextSlope;
		return Low + Fraction * (Slope + Fraction * (Quadratic + Fraction * Cubic));
	}

	double DistanceToSegment(FVector2d Position, FVector2d Start, FVector2d End, double& OutAlongFraction)
	{
		const FVector2d Segment = End - Start;
		const double LengthSquared = Segment.SquaredLength();
		if (LengthSquared <= UE_DOUBLE_SMALL_NUMBER)
		{
			OutAlongFraction = 0.0;
			return FVector2d::Distance(Position, Start);
		}
		OutAlongFraction = FMath::Clamp(FVector2d::DotProduct(Position - Start, Segment) / LengthSquared, 0.0, 1.0);
		return FVector2d::Distance(Position, Start + Segment * OutAlongFraction);
	}

	/**
	 * Rounds the crease where two channel cuts meet. Blending only the two
	 * deepest keeps this exact: an accumulating blend over every segment in
	 * reach drags the whole cut down wherever channels cluster.
	 */
	double PolynomialSmoothMin(double LowerValue, double HigherValue, double BlendWidth)
	{
		const double Blend = FMath::Clamp(0.5 + 0.5 * (HigherValue - LowerValue) / BlendWidth, 0.0, 1.0);
		return FMath::Lerp(HigherValue, LowerValue, Blend) - BlendWidth * Blend * (1.0 - Blend);
	}

	struct FFloodEntry
	{
		double FilledHeightMeters = 0.0;
		int32 CellIndex = INDEX_NONE;
	};

	/** Lowest filled height pops first; ties break on cell index so the pop order is fully determined. */
	struct FFloodHeapOrder
	{
		bool operator()(const FFloodEntry& Left, const FFloodEntry& Right) const
		{
			if (Left.FilledHeightMeters != Right.FilledHeightMeters)
			{
				return Left.FilledHeightMeters < Right.FilledHeightMeters;
			}
			return Left.CellIndex < Right.CellIndex;
		}
	};

	/**
	 * Transient state of the stream-power solve, after Braun & Willett (2013).
	 * Each iteration re-routes water over the current surface and applies one
	 * implicit erosion step; the scheme is unconditionally stable, so the step
	 * size is folded into the stream-power factor and a few dozen iterations
	 * reach a quasi-steady mountain range.
	 */
	struct FFluvialSolver
	{
		const FWorldGenConfig& Config;
		int32 CellsPerSide = 0;
		int32 CellCount = 0;
		double CellSizeMeters = 0.0;
		double OriginMeters = 0.0;

		TArray<double> HeightMeters;
		TArray<double> UpliftStepMeters;
		TArray<double> FilledHeightMeters;
		TArray<double> DrainageAreaSquareMeters;
		TArray<double> ReceiverDistanceMeters;
		TArray<int32> ReceiverIndices;
		/** Cells in the order the flood popped them: non-decreasing filled height, receivers first. */
		TArray<int32> PopOrder;
		TBitArray<> Flooded;
		TArray<FFloodEntry> FloodHeap;

		explicit FFluvialSolver(const FWorldGenConfig& InConfig)
			: Config(InConfig)
		{
			CellSizeMeters = FMath::Max(8.0, static_cast<double>(Config.BakeCellSizeMeters));
			CellsPerSide = FMath::Max(4, FMath::CeilToInt32(Config.WorldSizeMeters / CellSizeMeters) + 1);
			CellCount = CellsPerSide * CellsPerSide;
			OriginMeters = Config.WorldSizeMeters * 0.5;
		}

		bool IsBorder(int32 CellIndex) const
		{
			const int32 CellX = CellIndex % CellsPerSide;
			const int32 CellY = CellIndex / CellsPerSide;
			return CellX == 0 || CellY == 0 || CellX == CellsPerSide - 1 || CellY == CellsPerSide - 1;
		}

		int32 NeighborCell(int32 CellIndex, int32 NeighborIndex) const
		{
			const int32 CellX = CellIndex % CellsPerSide + NeighborStepsX[NeighborIndex];
			const int32 CellY = CellIndex / CellsPerSide + NeighborStepsY[NeighborIndex];
			const bool bInside = CellX >= 0 && CellX < CellsPerSide && CellY >= 0 && CellY < CellsPerSide;
			if (!bInside)
			{
				return INDEX_NONE;
			}
			return CellY * CellsPerSide + CellX;
		}

		double NeighborDistanceMeters(int32 NeighborIndex) const
		{
			if (NeighborStepsX[NeighborIndex] != 0 && NeighborStepsY[NeighborIndex] != 0)
			{
				return CellSizeMeters * UE_DOUBLE_SQRT_2;
			}
			return CellSizeMeters;
		}

		FVector2d CellPosition(int32 CellIndex) const
		{
			return FVector2d(
				-OriginMeters + (CellIndex % CellsPerSide) * CellSizeMeters,
				-OriginMeters + (CellIndex / CellsPerSide) * CellSizeMeters);
		}

		void InitializeSurface(const FMassifUpliftField& Uplift, uint32 TieBreakSeed)
		{
			HeightMeters.SetNumUninitialized(CellCount);
			UpliftStepMeters.SetNumUninitialized(CellCount);
			ParallelFor(CellsPerSide, [this, &Uplift, TieBreakSeed](int32 CellY)
			{
				InitializeSurfaceRow(CellY, Uplift, TieBreakSeed);
			});
		}

		void InitializeSurfaceRow(int32 CellY, const FMassifUpliftField& Uplift, uint32 TieBreakSeed)
		{
			const double NoiseScale = 1.0 / (CellSizeMeters * TieBreakWavelengthCells);
			for (int32 CellX = 0; CellX < CellsPerSide; ++CellX)
			{
				const int32 CellIndex = CellY * CellsPerSide + CellX;
				const FVector2d Position = CellPosition(CellIndex);
				const FMassifUpliftSample UpliftSample = Uplift.Sample(Position);
				const double TieBreak = TieBreakAmplitudeMeters
					* MassifNoise::GradientNoise(Position.X * NoiseScale, Position.Y * NoiseScale, TieBreakSeed);
				HeightMeters[CellIndex] = UpliftSample.CrestAltitudeMeters + TieBreak;
				UpliftStepMeters[CellIndex] = static_cast<double>(Config.BakeUpliftMetersPerStep) * UpliftSample.RangeFraction;
				if (IsBorder(CellIndex))
				{
					HeightMeters[CellIndex] = Config.MinAltitudeMeters;
					UpliftStepMeters[CellIndex] = 0.0;
				}
			}
		}

		/**
		 * Priority flood, epsilon variant after Barnes et al. (2014). One pass
		 * yields pit-free routing heights and the topological processing order:
		 * cells pop in non-decreasing filled height, so every cell pops after
		 * the cell it drains to.
		 */
		void FloodFill()
		{
			FilledHeightMeters = HeightMeters;
			PopOrder.Reset(CellCount);
			FloodHeap.Reset();
			Flooded.Init(false, CellCount);
			for (int32 CellIndex = 0; CellIndex < CellCount; ++CellIndex)
			{
				SeedBorderCell(CellIndex);
			}
			while (FloodHeap.Num() > 0)
			{
				FFloodEntry Lowest;
				FloodHeap.HeapPop(Lowest, FFloodHeapOrder(), EAllowShrinking::No);
				PopOrder.Add(Lowest.CellIndex);
				FloodNeighbors(Lowest.CellIndex, Lowest.FilledHeightMeters);
			}
		}

		void SeedBorderCell(int32 CellIndex)
		{
			if (!IsBorder(CellIndex))
			{
				return;
			}
			Flooded[CellIndex] = true;
			FloodHeap.HeapPush(FFloodEntry{ FilledHeightMeters[CellIndex], CellIndex }, FFloodHeapOrder());
		}

		void FloodNeighbors(int32 CellIndex, double PoppedFilledHeightMeters)
		{
			for (int32 NeighborIndex = 0; NeighborIndex < 8; ++NeighborIndex)
			{
				const int32 Neighbor = NeighborCell(CellIndex, NeighborIndex);
				if (Neighbor == INDEX_NONE || Flooded[Neighbor])
				{
					continue;
				}
				Flooded[Neighbor] = true;
				FilledHeightMeters[Neighbor] = FMath::Max(
					HeightMeters[Neighbor], PoppedFilledHeightMeters + FillEpsilonMeters);
				FloodHeap.HeapPush(FFloodEntry{ FilledHeightMeters[Neighbor], Neighbor }, FFloodHeapOrder());
			}
		}

		void ComputeReceivers()
		{
			ReceiverIndices.SetNumUninitialized(CellCount);
			ReceiverDistanceMeters.SetNumUninitialized(CellCount);
			ParallelFor(CellsPerSide, [this](int32 CellY)
			{
				ComputeReceiverRow(CellY);
			});
		}

		void ComputeReceiverRow(int32 CellY)
		{
			for (int32 CellX = 0; CellX < CellsPerSide; ++CellX)
			{
				ComputeReceiverForCell(CellY * CellsPerSide + CellX);
			}
		}

		/**
		 * Steepest descent on the filled surface, scanning neighbours in a fixed
		 * order so exact ties resolve the same way every run. The epsilon fill
		 * guarantees every interior cell a strictly lower neighbour.
		 */
		void ComputeReceiverForCell(int32 CellIndex)
		{
			ReceiverIndices[CellIndex] = CellIndex;
			ReceiverDistanceMeters[CellIndex] = CellSizeMeters;
			if (IsBorder(CellIndex))
			{
				return;
			}
			double SteepestDrop = 0.0;
			for (int32 NeighborIndex = 0; NeighborIndex < 8; ++NeighborIndex)
			{
				const int32 Neighbor = NeighborCell(CellIndex, NeighborIndex);
				const double Distance = NeighborDistanceMeters(NeighborIndex);
				const double Drop = (FilledHeightMeters[CellIndex] - FilledHeightMeters[Neighbor]) / Distance;
				if (Drop <= SteepestDrop)
				{
					continue;
				}
				SteepestDrop = Drop;
				ReceiverIndices[CellIndex] = Neighbor;
				ReceiverDistanceMeters[CellIndex] = Distance;
			}
		}

		void AccumulateDrainage()
		{
			const double CellAreaSquareMeters = CellSizeMeters * CellSizeMeters;
			DrainageAreaSquareMeters.Init(CellAreaSquareMeters, CellCount);
			for (int32 OrderIndex = PopOrder.Num() - 1; OrderIndex >= 0; --OrderIndex)
			{
				const int32 CellIndex = PopOrder[OrderIndex];
				const int32 Receiver = ReceiverIndices[CellIndex];
				if (Receiver != CellIndex)
				{
					DrainageAreaSquareMeters[Receiver] += DrainageAreaSquareMeters[CellIndex];
				}
			}
		}

		/**
		 * Implicit stream-power update in pop order, so every receiver holds its
		 * new height before the cells draining into it read it. Erosion works on
		 * the real surface, not the filled one: lakes erode out over iterations
		 * instead of freezing at their spill level.
		 */
		double ApplyErosionStep()
		{
			double MaxChangeMeters = 0.0;
			for (const int32 CellIndex : PopOrder)
			{
				MaxChangeMeters = FMath::Max(MaxChangeMeters, UpdateCellHeight(CellIndex));
			}
			return MaxChangeMeters;
		}

		double UpdateCellHeight(int32 CellIndex)
		{
			if (IsBorder(CellIndex))
			{
				return 0.0;
			}
			const int32 Receiver = ReceiverIndices[CellIndex];
			const double ErosionFactor = static_cast<double>(Config.BakeStreamPowerFactor)
				* FMath::Pow(DrainageAreaSquareMeters[CellIndex], static_cast<double>(Config.BakeDrainageExponent))
				/ ReceiverDistanceMeters[CellIndex];
			const double PreviousHeight = HeightMeters[CellIndex];
			const double RaisedHeight = PreviousHeight + UpliftStepMeters[CellIndex];
			const double ErodedHeight = (RaisedHeight + ErosionFactor * HeightMeters[Receiver]) / (1.0 + ErosionFactor);
			HeightMeters[CellIndex] = FMath::Min(ErodedHeight, static_cast<double>(Config.MaxAltitudeMeters));
			return FMath::Abs(HeightMeters[CellIndex] - PreviousHeight);
		}

		/** Optional blend towards the four-neighbour mean, for softer interfluves than pure stream power leaves. */
		void ApplyHillslopeSmoothing()
		{
			const double Blend = FMath::Clamp(static_cast<double>(Config.BakeHillslopeSmoothing), 0.0, 1.0);
			if (Blend <= 0.0)
			{
				return;
			}
			TArray<double> Smoothed = HeightMeters;
			ParallelFor(CellsPerSide, [this, &Smoothed, Blend](int32 CellY)
			{
				SmoothRow(CellY, Blend, Smoothed);
			});
			HeightMeters = MoveTemp(Smoothed);
		}

		void SmoothRow(int32 CellY, double Blend, TArray<double>& Smoothed) const
		{
			if (CellY == 0 || CellY == CellsPerSide - 1)
			{
				return;
			}
			for (int32 CellX = 1; CellX < CellsPerSide - 1; ++CellX)
			{
				const int32 CellIndex = CellY * CellsPerSide + CellX;
				const double NeighborMean = 0.25 * (
					HeightMeters[CellIndex - 1] + HeightMeters[CellIndex + 1]
					+ HeightMeters[CellIndex - CellsPerSide] + HeightMeters[CellIndex + CellsPerSide]);
				Smoothed[CellIndex] = FMath::Lerp(HeightMeters[CellIndex], NeighborMean, Blend);
			}
		}

		void Solve()
		{
			for (int32 Iteration = 0; Iteration < Config.BakeMaxIterationCount; ++Iteration)
			{
				FloodFill();
				ComputeReceivers();
				AccumulateDrainage();
				const double MaxChangeMeters = ApplyErosionStep();
				ApplyHillslopeSmoothing();
				if (MaxChangeMeters < Config.BakeConvergenceMeters)
				{
					break;
				}
			}
			// Route once more so receivers, drainage and pop order describe the
			// final surface the sampling products are derived from.
			FloodFill();
			ComputeReceivers();
			AccumulateDrainage();
		}
	};
}

FMassifFluvialBake FMassifFluvialBake::Build(const FWorldGenConfig& Config, const FMassifUpliftField& Uplift)
{
	FFluvialSolver Solver(Config);
	const uint32 BakeSeed = FMassifSeed(static_cast<uint32>(Config.MasterSeed)).MakeChannelSeed(TEXT("WorldGen.FluvialBake"));
	Solver.InitializeSurface(Uplift, BakeSeed);
	Solver.Solve();

	FMassifFluvialBake Bake;
	Bake.CellsPerSide = Solver.CellsPerSide;
	Bake.CellSizeMeters = Solver.CellSizeMeters;
	Bake.OriginMeters = Solver.OriginMeters;
	Bake.HeightGridMeters = MoveTemp(Solver.HeightMeters);
	Bake.DrainageAreaSquareMeters = MoveTemp(Solver.DrainageAreaSquareMeters);
	Bake.ReceiverIndices = MoveTemp(Solver.ReceiverIndices);

	const double ChannelAreaThreshold = FMath::Max(1.0, static_cast<double>(Config.RiverDrainageAreaThresholdSquareMeters));
	Bake.ComputeGradientGrid();
	Bake.ComputeDrainWaterGrid(Solver.PopOrder, ChannelAreaThreshold);
	Bake.ExtractRiverPolylines(Solver.PopOrder, ChannelAreaThreshold);
	Bake.SmoothRiverPolylines();
	Bake.BuildRiverSegmentIndex(Config);
	return Bake;
}

bool FMassifFluvialBake::IsBorderCell(int32 CellIndex) const
{
	const int32 CellX = CellIndex % CellsPerSide;
	const int32 CellY = CellIndex / CellsPerSide;
	return CellX == 0 || CellY == 0 || CellX == CellsPerSide - 1 || CellY == CellsPerSide - 1;
}

double FMassifFluvialBake::CarveReachMeters(const FWorldGenConfig& Config)
{
	return static_cast<double>(Config.ChannelMaxFloorHalfWidthMeters) + static_cast<double>(Config.ChannelMaxSideWidthMeters);
}

int32 FMassifFluvialBake::ClampGridNode(int32 NodeIndex) const
{
	return FMath::Clamp(NodeIndex, 0, CellsPerSide - 1);
}

FVector2d FMassifFluvialBake::CellPositionMeters(int32 CellIndex) const
{
	return FVector2d(
		-OriginMeters + (CellIndex % CellsPerSide) * CellSizeMeters,
		-OriginMeters + (CellIndex / CellsPerSide) * CellSizeMeters);
}

void FMassifFluvialBake::ComputeGradientGrid()
{
	GradientMetersPerMeter.SetNumUninitialized(CellsPerSide * CellsPerSide);
	ParallelFor(CellsPerSide, [this](int32 CellY)
	{
		for (int32 CellX = 0; CellX < CellsPerSide; ++CellX)
		{
			const int32 EastIndex = CellY * CellsPerSide + ClampGridNode(CellX + 1);
			const int32 WestIndex = CellY * CellsPerSide + ClampGridNode(CellX - 1);
			const int32 NorthIndex = ClampGridNode(CellY + 1) * CellsPerSide + CellX;
			const int32 SouthIndex = ClampGridNode(CellY - 1) * CellsPerSide + CellX;
			const double RunX = (ClampGridNode(CellX + 1) - ClampGridNode(CellX - 1)) * CellSizeMeters;
			const double RunY = (ClampGridNode(CellY + 1) - ClampGridNode(CellY - 1)) * CellSizeMeters;
			GradientMetersPerMeter[CellY * CellsPerSide + CellX] = FVector2f(
				static_cast<float>((HeightGridMeters[EastIndex] - HeightGridMeters[WestIndex]) / FMath::Max(RunX, CellSizeMeters)),
				static_cast<float>((HeightGridMeters[NorthIndex] - HeightGridMeters[SouthIndex]) / FMath::Max(RunY, CellSizeMeters)));
		}
	});
}

void FMassifFluvialBake::ComputeDrainWaterGrid(const TArray<int32>& PopOrder, double ChannelAreaThresholdSquareMeters)
{
	// In pop order every receiver is resolved before the cells draining into
	// it, so one forward pass hands every hillside cell the water elevation of
	// the channel it actually drains to.
	DrainWaterElevationMeters.SetNumUninitialized(CellsPerSide * CellsPerSide);
	for (const int32 CellIndex : PopOrder)
	{
		const bool bIsChannel = DrainageAreaSquareMeters[CellIndex] >= ChannelAreaThresholdSquareMeters;
		if (bIsChannel || IsBorderCell(CellIndex))
		{
			DrainWaterElevationMeters[CellIndex] = HeightGridMeters[CellIndex];
			continue;
		}
		DrainWaterElevationMeters[CellIndex] = DrainWaterElevationMeters[ReceiverIndices[CellIndex]];
	}
}

void FMassifFluvialBake::ExtractRiverPolylines(const TArray<int32>& PopOrder, double ChannelAreaThresholdSquareMeters)
{
	const int32 CellCount = CellsPerSide * CellsPerSide;
	TArray<int32> NodeOfCell;
	NodeOfCell.Init(INDEX_NONE, CellCount);
	for (int32 CellIndex = 0; CellIndex < CellCount; ++CellIndex)
	{
		if (DrainageAreaSquareMeters[CellIndex] < ChannelAreaThresholdSquareMeters)
		{
			continue;
		}
		NodeOfCell[CellIndex] = RiverNodes.Num();
		FMassifFluvialRiverNode& Node = RiverNodes.AddDefaulted_GetRef();
		Node.PositionMeters = CellPositionMeters(CellIndex);
		Node.WaterElevationMeters = HeightGridMeters[CellIndex];
		Node.DrainageAreaSquareMeters = DrainageAreaSquareMeters[CellIndex];
	}

	// A channel cell's receiver gets its whole drainage added, so it is a
	// channel cell too, except where the channel leaves at the border and the
	// cell receives itself: that is an outlet. Water is clamped in pop order —
	// receivers first — so it never rises downstream even where the solved
	// surface keeps a residual bump.
	for (const int32 CellIndex : PopOrder)
	{
		const int32 NodeIndex = NodeOfCell[CellIndex];
		if (NodeIndex == INDEX_NONE)
		{
			continue;
		}
		const int32 Receiver = ReceiverIndices[CellIndex];
		if (Receiver == CellIndex)
		{
			continue;
		}
		RiverNodes[NodeIndex].ParentIndex = NodeOfCell[Receiver];
		RiverNodes[NodeIndex].WaterElevationMeters = FMath::Max(
			RiverNodes[NodeIndex].WaterElevationMeters, RiverNodes[NodeOfCell[Receiver]].WaterElevationMeters);
	}
}

void FMassifFluvialBake::SmoothRiverPolylines()
{
	// Two corner-cutting passes knock the receiver graph's 45-degree staircase
	// off before the segments are indexed. Only chain interiors move: junction
	// nodes, outlets and headwater tips stay pinned, and elevations do not
	// change, so water stays monotone downstream.
	TArray<int32> SingleChildOfNode;
	SingleChildOfNode.Init(INDEX_NONE, RiverNodes.Num());
	TArray<int32> ChildCounts;
	ChildCounts.Init(0, RiverNodes.Num());
	for (int32 NodeIndex = 0; NodeIndex < RiverNodes.Num(); ++NodeIndex)
	{
		const int32 ParentIndex = RiverNodes[NodeIndex].ParentIndex;
		if (ParentIndex == INDEX_NONE)
		{
			continue;
		}
		ChildCounts[ParentIndex] += 1;
		SingleChildOfNode[ParentIndex] = NodeIndex;
	}

	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		TArray<FVector2d> SmoothedPositions;
		SmoothedPositions.SetNumUninitialized(RiverNodes.Num());
		for (int32 NodeIndex = 0; NodeIndex < RiverNodes.Num(); ++NodeIndex)
		{
			SmoothedPositions[NodeIndex] = SmoothedNodePosition(NodeIndex, SingleChildOfNode, ChildCounts);
		}
		for (int32 NodeIndex = 0; NodeIndex < RiverNodes.Num(); ++NodeIndex)
		{
			RiverNodes[NodeIndex].PositionMeters = SmoothedPositions[NodeIndex];
		}
	}
}

FVector2d FMassifFluvialBake::SmoothedNodePosition(
	int32 NodeIndex, const TArray<int32>& SingleChildOfNode, const TArray<int32>& ChildCounts) const
{
	const FMassifFluvialRiverNode& Node = RiverNodes[NodeIndex];
	const bool bChainInterior = Node.ParentIndex != INDEX_NONE && ChildCounts[NodeIndex] == 1;
	if (!bChainInterior)
	{
		return Node.PositionMeters;
	}
	const FVector2d ParentPosition = RiverNodes[Node.ParentIndex].PositionMeters;
	const FVector2d ChildPosition = RiverNodes[SingleChildOfNode[NodeIndex]].PositionMeters;
	return ChildPosition * 0.25 + Node.PositionMeters * 0.5 + ParentPosition * 0.25;
}

void FMassifFluvialBake::BuildRiverSegmentIndex(const FWorldGenConfig& Config)
{
	// Index cells the size of the carve reach, so a query only ever scans the
	// three-by-three block around it: any segment able to cut a point is
	// registered in that block by construction.
	RiverIndexCellSizeMeters = FMath::Max(1.0, CarveReachMeters(Config));
	RiverIndexOriginMeters = OriginMeters;
	RiverIndexCellsPerSide = FMath::Max(1, FMath::CeilToInt32(Config.WorldSizeMeters / RiverIndexCellSizeMeters) + 1);

	const int32 IndexCellCount = RiverIndexCellsPerSide * RiverIndexCellsPerSide;
	TArray<int32> CellCounts;
	CellCounts.Init(0, IndexCellCount);

	auto ForEachOverlappedCell = [this](FVector2d Start, FVector2d End, auto&& Visit)
	{
		const int32 LowX = FMath::Clamp(FMath::FloorToInt32((FMath::Min(Start.X, End.X) + RiverIndexOriginMeters) / RiverIndexCellSizeMeters), 0, RiverIndexCellsPerSide - 1);
		const int32 HighX = FMath::Clamp(FMath::FloorToInt32((FMath::Max(Start.X, End.X) + RiverIndexOriginMeters) / RiverIndexCellSizeMeters), 0, RiverIndexCellsPerSide - 1);
		const int32 LowY = FMath::Clamp(FMath::FloorToInt32((FMath::Min(Start.Y, End.Y) + RiverIndexOriginMeters) / RiverIndexCellSizeMeters), 0, RiverIndexCellsPerSide - 1);
		const int32 HighY = FMath::Clamp(FMath::FloorToInt32((FMath::Max(Start.Y, End.Y) + RiverIndexOriginMeters) / RiverIndexCellSizeMeters), 0, RiverIndexCellsPerSide - 1);
		for (int32 CellY = LowY; CellY <= HighY; ++CellY)
		{
			for (int32 CellX = LowX; CellX <= HighX; ++CellX)
			{
				Visit(CellY * RiverIndexCellsPerSide + CellX);
			}
		}
	};

	for (int32 NodeIndex = 0; NodeIndex < RiverNodes.Num(); ++NodeIndex)
	{
		if (RiverNodes[NodeIndex].ParentIndex == INDEX_NONE)
		{
			continue;
		}
		ForEachOverlappedCell(RiverNodes[NodeIndex].PositionMeters, RiverNodes[RiverNodes[NodeIndex].ParentIndex].PositionMeters,
			[&CellCounts](int32 Cell) { ++CellCounts[Cell]; });
	}

	RiverIndexCellStarts.SetNumUninitialized(IndexCellCount + 1);
	int32 RunningTotal = 0;
	for (int32 Cell = 0; Cell < IndexCellCount; ++Cell)
	{
		RiverIndexCellStarts[Cell] = RunningTotal;
		RunningTotal += CellCounts[Cell];
	}
	RiverIndexCellStarts[IndexCellCount] = RunningTotal;

	RiverIndexedSegments.SetNumUninitialized(RunningTotal);
	TArray<int32> FillCursors = RiverIndexCellStarts;
	for (int32 NodeIndex = 0; NodeIndex < RiverNodes.Num(); ++NodeIndex)
	{
		if (RiverNodes[NodeIndex].ParentIndex == INDEX_NONE)
		{
			continue;
		}
		ForEachOverlappedCell(RiverNodes[NodeIndex].PositionMeters, RiverNodes[RiverNodes[NodeIndex].ParentIndex].PositionMeters,
			[this, &FillCursors, NodeIndex](int32 Cell) { RiverIndexedSegments[FillCursors[Cell]++] = NodeIndex; });
	}
}

double FMassifFluvialBake::SampleHeightMeters(FVector2d PositionMeters) const
{
	const double GridX = (PositionMeters.X + OriginMeters) / CellSizeMeters;
	const double GridY = (PositionMeters.Y + OriginMeters) / CellSizeMeters;
	const int32 BaseX = FMath::FloorToInt32(FMath::Clamp(GridX, -1.0, static_cast<double>(CellsPerSide)));
	const int32 BaseY = FMath::FloorToInt32(FMath::Clamp(GridY, -1.0, static_cast<double>(CellsPerSide)));
	const double FractionX = FMath::Clamp(GridX - BaseX, 0.0, 1.0);
	const double FractionY = FMath::Clamp(GridY - BaseY, 0.0, 1.0);

	double Rows[4] = { 0.0, 0.0, 0.0, 0.0 };
	for (int32 Row = 0; Row < 4; ++Row)
	{
		const int32 RowBase = ClampGridNode(BaseY - 1 + Row) * CellsPerSide;
		Rows[Row] = CubicInterpolateDouble(
			HeightGridMeters[RowBase + ClampGridNode(BaseX - 1)],
			HeightGridMeters[RowBase + ClampGridNode(BaseX)],
			HeightGridMeters[RowBase + ClampGridNode(BaseX + 1)],
			HeightGridMeters[RowBase + ClampGridNode(BaseX + 2)],
			FractionX);
	}
	return CubicInterpolateDouble(Rows[0], Rows[1], Rows[2], Rows[3], FractionY);
}

FVector2d FMassifFluvialBake::SampleGradientMetersPerMeter(FVector2d PositionMeters) const
{
	const double GridX = (PositionMeters.X + OriginMeters) / CellSizeMeters;
	const double GridY = (PositionMeters.Y + OriginMeters) / CellSizeMeters;
	const int32 BaseX = FMath::FloorToInt32(FMath::Clamp(GridX, -1.0, static_cast<double>(CellsPerSide)));
	const int32 BaseY = FMath::FloorToInt32(FMath::Clamp(GridY, -1.0, static_cast<double>(CellsPerSide)));
	const float FractionX = static_cast<float>(FMath::Clamp(GridX - BaseX, 0.0, 1.0));
	const float FractionY = static_cast<float>(FMath::Clamp(GridY - BaseY, 0.0, 1.0));

	// Bicubic: a lerp between grid nodes is continuous in value but not in
	// gradient, and the seams show as creases along the gullies laid over it.
	float RowsX[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	float RowsY[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	for (int32 Row = 0; Row < 4; ++Row)
	{
		const int32 RowBase = ClampGridNode(BaseY - 1 + Row) * CellsPerSide;
		const FVector2f Tap0 = GradientMetersPerMeter[RowBase + ClampGridNode(BaseX - 1)];
		const FVector2f Tap1 = GradientMetersPerMeter[RowBase + ClampGridNode(BaseX)];
		const FVector2f Tap2 = GradientMetersPerMeter[RowBase + ClampGridNode(BaseX + 1)];
		const FVector2f Tap3 = GradientMetersPerMeter[RowBase + ClampGridNode(BaseX + 2)];
		RowsX[Row] = CubicInterpolate(Tap0.X, Tap1.X, Tap2.X, Tap3.X, FractionX);
		RowsY[Row] = CubicInterpolate(Tap0.Y, Tap1.Y, Tap2.Y, Tap3.Y, FractionX);
	}
	return FVector2d(
		CubicInterpolate(RowsX[0], RowsX[1], RowsX[2], RowsX[3], FractionY),
		CubicInterpolate(RowsY[0], RowsY[1], RowsY[2], RowsY[3], FractionY));
}

double FMassifFluvialBake::SampleDrainWaterElevationMeters(FVector2d PositionMeters) const
{
	const double GridX = (PositionMeters.X + OriginMeters) / CellSizeMeters;
	const double GridY = (PositionMeters.Y + OriginMeters) / CellSizeMeters;
	const int32 BaseX = FMath::FloorToInt32(FMath::Clamp(GridX, 0.0, static_cast<double>(CellsPerSide - 1)));
	const int32 BaseY = FMath::FloorToInt32(FMath::Clamp(GridY, 0.0, static_cast<double>(CellsPerSide - 1)));
	const double FractionX = FMath::Clamp(GridX - BaseX, 0.0, 1.0);
	const double FractionY = FMath::Clamp(GridY - BaseY, 0.0, 1.0);

	const int32 LowRow = BaseY * CellsPerSide;
	const int32 HighRow = ClampGridNode(BaseY + 1) * CellsPerSide;
	const int32 EastNode = ClampGridNode(BaseX + 1);
	const double LowEdge = FMath::Lerp(
		DrainWaterElevationMeters[LowRow + BaseX], DrainWaterElevationMeters[LowRow + EastNode], FractionX);
	const double HighEdge = FMath::Lerp(
		DrainWaterElevationMeters[HighRow + BaseX], DrainWaterElevationMeters[HighRow + EastNode], FractionX);
	return FMath::Lerp(LowEdge, HighEdge, FractionY);
}

double FMassifFluvialBake::SampleChannelCarveMeters(
	FVector2d PositionMeters, double MacroHeightMeters, const FWorldGenConfig& Config) const
{
	double DeepestCutMeters = 0.0;
	double SecondDeepestCutMeters = 0.0;
	GatherChannelCuts(PositionMeters, MacroHeightMeters, Config, DeepestCutMeters, SecondDeepestCutMeters);
	if (DeepestCutMeters <= 0.0)
	{
		return 0.0;
	}

	// Two crossing channels merge smoothly instead of leaving a knob standing
	// between their trenches: the deepest two cuts blend as a smooth maximum.
	const double DepthCap = static_cast<double>(Config.ChannelNotchDepthCapMeters);
	const double MergedCutMeters = -PolynomialSmoothMin(
		-DeepestCutMeters, -SecondDeepestCutMeters, FMath::Max(1.0, DepthCap * 0.3));
	return FMath::Clamp(MergedCutMeters, 0.0, DepthCap);
}

void FMassifFluvialBake::GatherChannelCuts(
	FVector2d PositionMeters, double MacroHeightMeters, const FWorldGenConfig& Config,
	double& OutDeepestCutMeters, double& OutSecondDeepestCutMeters) const
{
	const int32 CenterCellX = FMath::Clamp(
		FMath::FloorToInt32((PositionMeters.X + RiverIndexOriginMeters) / RiverIndexCellSizeMeters), 0, RiverIndexCellsPerSide - 1);
	const int32 CenterCellY = FMath::Clamp(
		FMath::FloorToInt32((PositionMeters.Y + RiverIndexOriginMeters) / RiverIndexCellSizeMeters), 0, RiverIndexCellsPerSide - 1);
	for (int32 Neighbor = 0; Neighbor < 9; ++Neighbor)
	{
		const int32 CellX = FMath::Clamp(CenterCellX + (Neighbor % 3) - 1, 0, RiverIndexCellsPerSide - 1);
		const int32 CellY = FMath::Clamp(CenterCellY + (Neighbor / 3) - 1, 0, RiverIndexCellsPerSide - 1);
		AccumulateChannelCell(
			CellY * RiverIndexCellsPerSide + CellX, PositionMeters, MacroHeightMeters, Config,
			OutDeepestCutMeters, OutSecondDeepestCutMeters);
	}
}

void FMassifFluvialBake::AccumulateChannelCell(
	int32 IndexCell, FVector2d PositionMeters, double MacroHeightMeters, const FWorldGenConfig& Config,
	double& InOutDeepestCutMeters, double& InOutSecondDeepestCutMeters) const
{
	for (int32 Slot = RiverIndexCellStarts[IndexCell]; Slot < RiverIndexCellStarts[IndexCell + 1]; ++Slot)
	{
		const double CutMeters = ChannelCutMeters(RiverIndexedSegments[Slot], PositionMeters, MacroHeightMeters, Config);
		if (CutMeters > InOutDeepestCutMeters)
		{
			InOutSecondDeepestCutMeters = InOutDeepestCutMeters;
			InOutDeepestCutMeters = CutMeters;
		}
		else if (CutMeters > InOutSecondDeepestCutMeters)
		{
			InOutSecondDeepestCutMeters = CutMeters;
		}
	}
}

double FMassifFluvialBake::ChannelCutMeters(
	int32 NodeIndex, FVector2d PositionMeters, double MacroHeightMeters, const FWorldGenConfig& Config) const
{
	const FMassifFluvialRiverNode& Node = RiverNodes[NodeIndex];
	const FMassifFluvialRiverNode& Downstream = RiverNodes[Node.ParentIndex];
	double AlongFraction = 0.0;
	const double Distance = DistanceToSegment(PositionMeters, Node.PositionMeters, Downstream.PositionMeters, AlongFraction);

	// Hydraulic-geometry scaling: width and depth grow with the square root of
	// the drained area, so a headwater stream scratches a notch and a trunk
	// river cuts a broad floor, each capped by the channel config.
	const double SqrtArea = FMath::Sqrt(Node.DrainageAreaSquareMeters);
	const double FloorHalfWidth = FMath::Min(
		static_cast<double>(Config.ChannelWidthPerSqrtArea) * SqrtArea, static_cast<double>(Config.ChannelMaxFloorHalfWidthMeters));
	const double SideWidth = FMath::Min(
		static_cast<double>(Config.ChannelSideWidthPerSqrtArea) * SqrtArea, static_cast<double>(Config.ChannelMaxSideWidthMeters));
	if (Distance >= FloorHalfWidth + SideWidth)
	{
		return 0.0;
	}

	// Down to the water, never deeper than the discharge supports, gone at the
	// end of the side width. Zero where the water stands above the macro
	// surface, so a channel can only ever lower ground.
	const double WaterElevation = FMath::Lerp(Node.WaterElevationMeters, Downstream.WaterElevationMeters, AlongFraction);
	const double DepthAvailable = MacroHeightMeters - WaterElevation;
	if (DepthAvailable <= 0.0)
	{
		return 0.0;
	}
	const double DepthCap = FMath::Min(
		static_cast<double>(Config.ChannelDepthPerSqrtArea) * SqrtArea, static_cast<double>(Config.ChannelNotchDepthCapMeters));
	const double DepthMeters = FMath::Min(DepthAvailable, DepthCap);
	const double Side = FMath::Clamp((Distance - FloorHalfWidth) / FMath::Max(1.0, SideWidth), 0.0, 1.0);
	const double Fade = 1.0 - Side * Side * (3.0 - 2.0 * Side);
	return DepthMeters * Fade;
}

uint64 FMassifFluvialBake::ComputeGridHash() const
{
	FMassifStateHash Hash;
	Hash.Append(HeightGridMeters.GetData(), HeightGridMeters.Num() * sizeof(double));
	Hash.AppendInt(RiverNodes.Num());
	return Hash.Finalize();
}
