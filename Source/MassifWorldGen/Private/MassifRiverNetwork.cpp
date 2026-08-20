#include "MassifRiverNetwork.h"

#include "Algo/Sort.h"
#include "Async/ParallelFor.h"
#include "MassifEnvironmentBakeGrid.h"
#include "MassifStateHash.h"

// ---------------------------------------------------------------------------
// The network as data
// ---------------------------------------------------------------------------

TSharedRef<const FMassifRiverNetwork> FMassifRiverNetwork::Create(TArray<FMassifRiverReach>&& InReaches)
{
	TSharedRef<FMassifRiverNetwork> Network(new FMassifRiverNetwork());
	Network->Reaches = MoveTemp(InReaches);
	Network->MeasureTotals();
	return Network;
}

void FMassifRiverNetwork::MeasureTotals()
{
	for (const FMassifRiverReach& Reach : Reaches)
	{
		SourceCount += Reach.bStartsAtSource ? 1 : 0;
		MaxStrahlerOrder = FMath::Max(MaxStrahlerOrder, Reach.StrahlerOrder);
		for (int32 NodeIndex = 0; NodeIndex < Reach.Nodes.Num(); ++NodeIndex)
		{
			const FMassifRiverNode& Node = Reach.Nodes[NodeIndex];
			MaxDischargeCubicMetersPerSecond =
				FMath::Max(MaxDischargeCubicMetersPerSecond, Node.DischargeCubicMetersPerSecond);
			MaxWidthMeters = FMath::Max(MaxWidthMeters, Node.WidthMeters);
			if (NodeIndex > 0)
			{
				TotalLengthMeters += (Node.PositionMeters - Reach.Nodes[NodeIndex - 1].PositionMeters).Length();
			}
		}
	}
}

uint64 FMassifRiverNetwork::ComputeContentHash() const
{
	FMassifStateHash Hash;
	Hash.AppendInt(Reaches.Num());
	for (const FMassifRiverReach& Reach : Reaches)
	{
		Hash.AppendInt(Reach.StrahlerOrder);
		Hash.AppendInt(Reach.bStartsAtSource ? 1 : 0);
		Hash.AppendInt(Reach.Nodes.Num());
		for (const FMassifRiverNode& Node : Reach.Nodes)
		{
			// Millimetres and millilitres per second: fine enough that a real
			// difference registers, coarse enough that the last bit of a double
			// does not make two identical solves hash apart.
			Hash.AppendInt(FMath::RoundToInt64(Node.PositionMeters.X * 1000.0));
			Hash.AppendInt(FMath::RoundToInt64(Node.PositionMeters.Y * 1000.0));
			Hash.AppendInt(FMath::RoundToInt64(Node.WaterSurfaceMeters * 1000.0));
			Hash.AppendInt(FMath::RoundToInt64(Node.DischargeCubicMetersPerSecond * 1000.0));
			Hash.AppendInt(FMath::RoundToInt64(Node.WidthMeters * 1000.0));
		}
	}
	return Hash.Finalize();
}

// ---------------------------------------------------------------------------
// Hydraulic geometry: what a discharge makes of a channel
// ---------------------------------------------------------------------------

namespace
{
	void ShapeChannel(const FMassifRiverParams& Params, FMassifRiverNode& Node)
	{
		const double Discharge = FMath::Max(Node.DischargeCubicMetersPerSecond, 0.0);
		Node.WidthMeters = FMath::Max(
			Params.WidthCoefficient * FMath::Pow(Discharge, Params.WidthExponent), Params.MinWidthMeters);
		Node.DepthMeters = FMath::Max(
			Params.DepthCoefficient * FMath::Pow(Discharge, Params.DepthExponent), Params.MinDepthMeters);
		Node.SpeedMetersPerSecond = Params.SpeedCoefficient * FMath::Pow(Discharge, Params.SpeedExponent);
	}
}

// ---------------------------------------------------------------------------
// Dropping nodes a straight run does not need
// ---------------------------------------------------------------------------

namespace
{
	double DistanceFromSegmentMeters(FVector2d Point, FVector2d Start, FVector2d End)
	{
		const FVector2d Along = End - Start;
		const double LengthSquared = Along.SquaredLength();
		if (LengthSquared <= UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			return (Point - Start).Length();
		}

		const double Fraction = FMath::Clamp((Point - Start).Dot(Along) / LengthSquared, 0.0, 1.0);
		return (Point - (Start + Fraction * Along)).Length();
	}

	/** The node inside a span that the chord through its ends misses by the most. */
	int32 FindFurthestNode(
		TArrayView<const FMassifRiverNode> Nodes, int32 FirstIndex, int32 LastIndex, double& OutDistanceMeters)
	{
		OutDistanceMeters = 0.0;
		int32 FurthestIndex = INDEX_NONE;
		for (int32 NodeIndex = FirstIndex + 1; NodeIndex < LastIndex; ++NodeIndex)
		{
			const double DistanceMeters = DistanceFromSegmentMeters(
				Nodes[NodeIndex].PositionMeters, Nodes[FirstIndex].PositionMeters, Nodes[LastIndex].PositionMeters);
			if (DistanceMeters > OutDistanceMeters)
			{
				OutDistanceMeters = DistanceMeters;
				FurthestIndex = NodeIndex;
			}
		}
		return FurthestIndex;
	}

	/**
	 * Douglas-Peucker over the node positions. One node per bake cell is far more
	 * geometry than a channel's shape needs, and the attributes along a reach vary
	 * smoothly enough that dropping a node between two kept ones loses nothing.
	 * End points are always kept, so reaches still meet where they met before.
	 */
	void SimplifyReach(TArray<FMassifRiverNode>& Nodes, double ToleranceMeters)
	{
		if (Nodes.Num() <= 2)
		{
			return;
		}

		TBitArray<> Keep(false, Nodes.Num());
		Keep[0] = true;
		Keep[Nodes.Num() - 1] = true;

		TArray<TPair<int32, int32>> Spans;
		Spans.Push({ 0, Nodes.Num() - 1 });
		while (Spans.Num() > 0)
		{
			const TPair<int32, int32> Span = Spans.Pop(EAllowShrinking::No);
			double DistanceMeters = 0.0;
			const int32 FurthestIndex = FindFurthestNode(Nodes, Span.Key, Span.Value, DistanceMeters);
			if (FurthestIndex == INDEX_NONE || DistanceMeters <= ToleranceMeters)
			{
				continue;
			}

			Keep[FurthestIndex] = true;
			Spans.Push({ Span.Key, FurthestIndex });
			Spans.Push({ FurthestIndex, Span.Value });
		}

		TArray<FMassifRiverNode> Kept;
		Kept.Reserve(Nodes.Num());
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			if (Keep[NodeIndex])
			{
				Kept.Add(Nodes[NodeIndex]);
			}
		}
		Nodes = MoveTemp(Kept);
	}
}

// ---------------------------------------------------------------------------
// Tracing the channels out of the solved drainage
// ---------------------------------------------------------------------------

namespace
{
	/**
	 * Reads rivers off the filled surface the depression fill produced. Because
	 * that surface has no local minima, following the steepest neighbour from any
	 * cell descends without interruption to the world border: the walk always
	 * terminates and the receiver graph is a forest, not a graph with cycles.
	 */
	struct FRiverExtraction
	{
		const FMassifBakeGrid& Grid;
		const FMassifRiverParams& Params;
		TArrayView<const double> Measured;
		TArrayView<const double> Filled;
		TArrayView<const float> AccumulationSquareMeters;

		/** Steepest downhill neighbour of every cell in the world, or INDEX_NONE where water leaves. */
		TArray<int32> ReceiverOfCell;

		/** Compact index of a cell among the river cells, or INDEX_NONE if it carries no channel. */
		TArray<int32> RiverIndexOfCell;

		/** Cell index of each river cell, and the per-river-cell state solved over them. */
		TArray<int32> RiverCells;
		TArray<int32> UpstreamCount;
		TArray<int32> StrahlerOrder;
		TArray<int32> HighestUpstreamOrder;
		TArray<int32> HighestUpstreamOrderCount;
		TArray<double> WaterSurfaceMeters;

		TArray<FMassifRiverReach> Run()
		{
			ComputeReceivers();
			MarkRiverCells();
			if (RiverCells.IsEmpty())
			{
				return {};
			}

			CountUpstreamContributors();
			SolveOrdersAndWaterSurface();
			return TraceReaches();
		}

		// --- The receiver graph ---------------------------------------------

		int32 FindSteepestReceiver(int32 Column, int32 Row) const
		{
			const double CellAltitudeMeters = Filled[Grid.Index(Column, Row)];
			int32 SteepestCell = INDEX_NONE;
			double SteepestSlope = 0.0;
			for (int32 Direction = 0; Direction < MassifBakeNeighbours::Count; ++Direction)
			{
				ConsiderReceiver(Column, Row, CellAltitudeMeters, Direction, SteepestCell, SteepestSlope);
			}
			return SteepestCell;
		}

		void ConsiderReceiver(
			int32 Column, int32 Row, double CellAltitudeMeters, int32 Direction,
			int32& InOutSteepestCell, double& InOutSteepestSlope) const
		{
			const int32 NeighbourColumn = Column + MassifBakeNeighbours::OffsetX[Direction];
			const int32 NeighbourRow = Row + MassifBakeNeighbours::OffsetY[Direction];
			if (!Grid.Contains(NeighbourColumn, NeighbourRow))
			{
				return;
			}

			const int32 NeighbourCell = Grid.Index(NeighbourColumn, NeighbourRow);
			const double DistanceMeters = MassifBakeNeighbours::DistanceInCells[Direction] * Grid.CellSizeMeters;
			const double Slope = (CellAltitudeMeters - Filled[NeighbourCell]) / DistanceMeters;

			// Strictly greater, so an exact tie keeps the lower direction index and
			// the graph does not depend on the order neighbours happen to be visited in.
			if (Slope > InOutSteepestSlope)
			{
				InOutSteepestSlope = Slope;
				InOutSteepestCell = NeighbourCell;
			}
		}

		void ComputeReceivers()
		{
			ReceiverOfCell.SetNumUninitialized(Grid.CellCount());
			ParallelFor(Grid.Height, [this](int32 Row)
			{
				for (int32 Column = 0; Column < Grid.Width; ++Column)
				{
					ReceiverOfCell[Grid.Index(Column, Row)] = FindSteepestReceiver(Column, Row);
				}
			});
		}

		// --- Which cells carry a channel -------------------------------------

		/**
		 * A cell carries a channel once enough catchment drains through it, and so
		 * does everything downstream of one. The second half matters: flow is
		 * routed to several neighbours at once, so the steepest of them does not
		 * always inherit the larger share, and a threshold alone would leave a
		 * river with gaps in it.
		 */
		void MarkRiverCells()
		{
			RiverIndexOfCell.Init(INDEX_NONE, Grid.CellCount());
			const double ThresholdSquareMeters = Params.ChannelInitiationAreaSquareMeters;
			for (int32 CellIndex = 0; CellIndex < Grid.CellCount(); ++CellIndex)
			{
				if (AccumulationSquareMeters[CellIndex] >= ThresholdSquareMeters)
				{
					MarkFromHereDownstream(CellIndex);
				}
			}

			UpstreamCount.SetNumZeroed(RiverCells.Num());
			StrahlerOrder.SetNumZeroed(RiverCells.Num());
			HighestUpstreamOrder.SetNumZeroed(RiverCells.Num());
			HighestUpstreamOrderCount.SetNumZeroed(RiverCells.Num());
			WaterSurfaceMeters.SetNumZeroed(RiverCells.Num());
		}

		void MarkFromHereDownstream(int32 CellIndex)
		{
			int32 WalkCell = CellIndex;
			while (WalkCell != INDEX_NONE && RiverIndexOfCell[WalkCell] == INDEX_NONE)
			{
				RiverIndexOfCell[WalkCell] = RiverCells.Add(WalkCell);
				WalkCell = ReceiverOfCell[WalkCell];
			}
		}

		void CountUpstreamContributors()
		{
			for (int32 RiverIndex = 0; RiverIndex < RiverCells.Num(); ++RiverIndex)
			{
				const int32 ReceiverCell = ReceiverOfCell[RiverCells[RiverIndex]];
				if (ReceiverCell == INDEX_NONE)
				{
					continue;
				}

				const int32 ReceiverRiverIndex = RiverIndexOfCell[ReceiverCell];
				UpstreamCount[ReceiverRiverIndex] += 1;
			}
		}

		// --- Order and water level, in one pass down the network -------------

		/**
		 * River cells highest first, which on a surface with no local minima is a
		 * drainage order: everything draining into a cell is visited before it.
		 */
		TArray<int32> MakeDrainageOrder() const
		{
			TArray<int32> Order;
			Order.SetNumUninitialized(RiverCells.Num());
			for (int32 RiverIndex = 0; RiverIndex < RiverCells.Num(); ++RiverIndex)
			{
				Order[RiverIndex] = RiverIndex;
			}

			Algo::Sort(Order, [this](int32 Left, int32 Right)
			{
				const double LeftAltitude = Filled[RiverCells[Left]];
				const double RightAltitude = Filled[RiverCells[Right]];
				if (LeftAltitude != RightAltitude)
				{
					return LeftAltitude > RightAltitude;
				}
				return RiverCells[Left] < RiverCells[Right];
			});
			return Order;
		}

		void SolveOrdersAndWaterSurface()
		{
			for (int32 RiverIndex = 0; RiverIndex < RiverCells.Num(); ++RiverIndex)
			{
				WaterSurfaceMeters[RiverIndex] = Measured[RiverCells[RiverIndex]] + Params.SurfaceRiseMeters;
			}

			for (int32 RiverIndex : MakeDrainageOrder())
			{
				StrahlerOrder[RiverIndex] = ResolveStrahlerOrder(RiverIndex);
				HandDownToReceiver(RiverIndex);
			}
		}

		int32 ResolveStrahlerOrder(int32 RiverIndex) const
		{
			if (UpstreamCount[RiverIndex] == 0)
			{
				return 1;
			}

			// Two channels of equal order meeting make the next order up; one
			// joined by anything smaller stays as it was.
			if (HighestUpstreamOrderCount[RiverIndex] >= 2)
			{
				return HighestUpstreamOrder[RiverIndex] + 1;
			}
			return HighestUpstreamOrder[RiverIndex];
		}

		/**
		 * Passes this cell's order and water level to the one below it. Clamping the
		 * level down is what keeps the surface from climbing as it goes: the ground
		 * under a channel is measured and noisy, and water that rose downstream
		 * would read as flowing uphill.
		 */
		void HandDownToReceiver(int32 RiverIndex)
		{
			const int32 ReceiverCell = ReceiverOfCell[RiverCells[RiverIndex]];
			if (ReceiverCell == INDEX_NONE)
			{
				return;
			}

			const int32 ReceiverRiverIndex = RiverIndexOfCell[ReceiverCell];
			WaterSurfaceMeters[ReceiverRiverIndex] =
				FMath::Min(WaterSurfaceMeters[ReceiverRiverIndex], WaterSurfaceMeters[RiverIndex]);
			AccumulateUpstreamOrder(ReceiverRiverIndex, StrahlerOrder[RiverIndex]);
		}

		void AccumulateUpstreamOrder(int32 ReceiverRiverIndex, int32 Order)
		{
			if (Order > HighestUpstreamOrder[ReceiverRiverIndex])
			{
				HighestUpstreamOrder[ReceiverRiverIndex] = Order;
				HighestUpstreamOrderCount[ReceiverRiverIndex] = 1;
				return;
			}
			if (Order == HighestUpstreamOrder[ReceiverRiverIndex])
			{
				HighestUpstreamOrderCount[ReceiverRiverIndex] += 1;
			}
		}

		// --- Cutting the network into reaches --------------------------------

		/** A reach starts at a channel head and at every confluence, and nowhere else. */
		bool IsReachHead(int32 RiverIndex) const
		{
			return UpstreamCount[RiverIndex] != 1;
		}

		TArray<FMassifRiverReach> TraceReaches()
		{
			TArray<FMassifRiverReach> Reaches;
			for (int32 RiverIndex = 0; RiverIndex < RiverCells.Num(); ++RiverIndex)
			{
				if (!IsReachHead(RiverIndex))
				{
					continue;
				}

				FMassifRiverReach Reach = TraceReachFrom(RiverIndex);
				if (Reach.Nodes.Num() >= 2)
				{
					Reaches.Add(MoveTemp(Reach));
				}
			}
			return Reaches;
		}

		FMassifRiverReach TraceReachFrom(int32 HeadRiverIndex)
		{
			FMassifRiverReach Reach;
			Reach.StrahlerOrder = StrahlerOrder[HeadRiverIndex];
			Reach.bStartsAtSource = UpstreamCount[HeadRiverIndex] == 0;
			Reach.Nodes.Add(MakeNode(HeadRiverIndex));

			int32 RiverIndex = HeadRiverIndex;
			while (StepDownstream(RiverIndex))
			{
				Reach.Nodes.Add(MakeNode(RiverIndex));
				if (IsReachHead(RiverIndex))
				{
					break;
				}
			}

			SimplifyReach(Reach.Nodes, Params.SimplificationToleranceMeters);
			return Reach;
		}

		bool StepDownstream(int32& InOutRiverIndex) const
		{
			const int32 ReceiverCell = ReceiverOfCell[RiverCells[InOutRiverIndex]];
			if (ReceiverCell == INDEX_NONE)
			{
				return false;
			}

			InOutRiverIndex = RiverIndexOfCell[ReceiverCell];
			return true;
		}

		FMassifRiverNode MakeNode(int32 RiverIndex) const
		{
			const int32 CellIndex = RiverCells[RiverIndex];
			FMassifRiverNode Node;
			Node.PositionMeters = Grid.CellCentreMeters(CellIndex % Grid.Width, CellIndex / Grid.Width);
			Node.BedAltitudeMeters = Measured[CellIndex];
			Node.WaterSurfaceMeters = WaterSurfaceMeters[RiverIndex];
			Node.DischargeCubicMetersPerSecond =
				AccumulationSquareMeters[CellIndex] * Params.SpecificRunoffMetersPerSecond;
			ShapeChannel(Params, Node);
			return Node;
		}
	};
}

TSharedRef<const FMassifRiverNetwork> MassifBakeCpu::ExtractRivers(
	const FMassifBakeGrid& Grid,
	const FMassifRiverParams& Params,
	TArrayView<const double> Measured,
	TArrayView<const double> Filled,
	TArrayView<const float> AccumulationSquareMeters)
{
	FRiverExtraction Extraction{ Grid, Params, Measured, Filled, AccumulationSquareMeters };
	return FMassifRiverNetwork::Create(Extraction.Run());
}
