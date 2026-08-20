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
	/** Channel shape and the trench under it, both read off the discharge alone. */
	void ShapeChannel(const FMassifRiverParams& Params, FMassifRiverNode& Node)
	{
		const double Discharge = FMath::Max(Node.DischargeCubicMetersPerSecond, 0.0);
		Node.WidthMeters = FMath::Max(
			Params.WidthCoefficient * FMath::Pow(Discharge, Params.WidthExponent), Params.MinWidthMeters);
		Node.DepthMeters = FMath::Max(
			Params.DepthCoefficient * FMath::Pow(Discharge, Params.DepthExponent), Params.MinDepthMeters);
		Node.SpeedMetersPerSecond = Params.SpeedCoefficient * FMath::Pow(Discharge, Params.SpeedExponent);
		Node.BedAltitudeMeters =
			Node.WaterSurfaceMeters - FMath::Max(Node.DepthMeters, Params.MinCarveDepthMeters);
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

	/** How far a node may be moved by dropping it: a share of the channel it stands in. */
	double SimplificationToleranceAt(const FMassifRiverParams& Params, const FMassifRiverNode& Node)
	{
		return FMath::Clamp(
			Params.SimplificationWidthFraction * Node.WidthMeters,
			Params.MinSimplificationToleranceMeters,
			Params.MaxSimplificationToleranceMeters);
	}

	/**
	 * Douglas-Peucker over the node positions. One node per bake cell is far more
	 * geometry than a channel's shape needs, and the attributes along a reach vary
	 * smoothly enough that dropping a node between two kept ones loses nothing.
	 * End points are always kept, so reaches still meet where they met before.
	 */
	void SimplifyReach(TArray<FMassifRiverNode>& Nodes, const FMassifRiverParams& Params)
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
			if (FurthestIndex == INDEX_NONE
				|| DistanceMeters <= SimplificationToleranceAt(Params, Nodes[FurthestIndex]))
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
// Curving the line back through the corners the staircase left
// ---------------------------------------------------------------------------

namespace
{
	/** Catmull-Rom through four nodes, clamped at the ends so the curve keeps them. */
	FVector2d SplinePosition(TArrayView<const FMassifRiverNode> Nodes, int32 SpanIndex, double Fraction)
	{
		const FVector2d Before = Nodes[FMath::Max(SpanIndex - 1, 0)].PositionMeters;
		const FVector2d Start = Nodes[SpanIndex].PositionMeters;
		const FVector2d End = Nodes[SpanIndex + 1].PositionMeters;
		const FVector2d After = Nodes[FMath::Min(SpanIndex + 2, Nodes.Num() - 1)].PositionMeters;

		const FVector2d StartTangent = 0.5 * (End - Before);
		const FVector2d EndTangent = 0.5 * (After - Start);
		return FMath::CubicInterp(Start, StartTangent, End, EndTangent, Fraction);
	}

	/**
	 * A cubic Hermite through the samples with Fritsch-Carlson tangents, which is
	 * the interpolant that never leaves the range its own data spans.
	 *
	 * Catmull-Rom does leave it, and here that matters twice over: an overshoot
	 * on the water surface is water running uphill, which is the one property the
	 * downstream clamp exists to give the profile, and an overshoot on discharge
	 * is a channel carrying more than its catchment yields.
	 */
	struct FMonotoneSpline
	{
		TArray<double> AlongMeters;
		TArray<double> Values;
		TArray<double> Tangents;

		void Solve(TArrayView<const double> InAlongMeters, TArrayView<const double> InValues)
		{
			AlongMeters.Reset(InAlongMeters.Num());
			AlongMeters.Append(InAlongMeters.GetData(), InAlongMeters.Num());
			Values.Reset(InValues.Num());
			Values.Append(InValues.GetData(), InValues.Num());
			Tangents.SetNumUninitialized(Values.Num());
			for (int32 SampleIndex = 0; SampleIndex < Values.Num(); ++SampleIndex)
			{
				Tangents[SampleIndex] = SolveTangent(SampleIndex);
			}
		}

		double Evaluate(int32 SpanIndex, double Fraction) const
		{
			const double SpanMeters = AlongMeters[SpanIndex + 1] - AlongMeters[SpanIndex];
			const double FractionSquared = Fraction * Fraction;
			const double FractionCubed = FractionSquared * Fraction;
			return (2.0 * FractionCubed - 3.0 * FractionSquared + 1.0) * Values[SpanIndex]
				+ (FractionCubed - 2.0 * FractionSquared + Fraction) * SpanMeters * Tangents[SpanIndex]
				+ (-2.0 * FractionCubed + 3.0 * FractionSquared) * Values[SpanIndex + 1]
				+ (FractionCubed - FractionSquared) * SpanMeters * Tangents[SpanIndex + 1];
		}

	private:
		double SecantSlope(int32 SpanIndex) const
		{
			const double SpanMeters = AlongMeters[SpanIndex + 1] - AlongMeters[SpanIndex];
			if (SpanMeters <= UE_DOUBLE_KINDA_SMALL_NUMBER)
			{
				return 0.0;
			}
			return (Values[SpanIndex + 1] - Values[SpanIndex]) / SpanMeters;
		}

		/**
		 * The weighted harmonic mean of the two secants around a sample, which is
		 * what keeps the cubic inside them. A sample the data turns at takes a flat
		 * tangent, so a local extreme stays where it is instead of being rounded
		 * past; the ends take their one secant, which cannot overshoot either.
		 */
		double SolveTangent(int32 SampleIndex) const
		{
			if (Values.Num() < 2)
			{
				return 0.0;
			}
			if (SampleIndex == 0 || SampleIndex == Values.Num() - 1)
			{
				return SecantSlope(FMath::Min(SampleIndex, Values.Num() - 2));
			}

			const double Incoming = SecantSlope(SampleIndex - 1);
			const double Outgoing = SecantSlope(SampleIndex);
			if (Incoming * Outgoing <= 0.0)
			{
				return 0.0;
			}
			return HarmonicMeanSlope(SampleIndex, Incoming, Outgoing);
		}

		double HarmonicMeanSlope(int32 SampleIndex, double Incoming, double Outgoing) const
		{
			const double IncomingSpan = AlongMeters[SampleIndex] - AlongMeters[SampleIndex - 1];
			const double OutgoingSpan = AlongMeters[SampleIndex + 1] - AlongMeters[SampleIndex];
			const double Weight = 3.0 * (IncomingSpan + OutgoingSpan);
			const double Denominator = (2.0 * OutgoingSpan + IncomingSpan) / Incoming
				+ (OutgoingSpan + 2.0 * IncomingSpan) / Outgoing;
			return Weight / Denominator;
		}
	};

	void GatherAttribute(
		TArrayView<const FMassifRiverNode> Nodes,
		TFunctionRef<double(const FMassifRiverNode&)> Read,
		TArray<double>& OutValues)
	{
		OutValues.Reset(Nodes.Num());
		for (const FMassifRiverNode& Node : Nodes)
		{
			OutValues.Add(Read(Node));
		}
	}

	/**
	 * Resamples a reach along a spline through its own nodes: the line curves back
	 * through the corners the routing's staircase left, and every attribute
	 * follows a monotone cubic over the distance already run.
	 *
	 * Width, depth and speed are not interpolated at all. They are re-derived
	 * from the interpolated discharge, so hydraulic continuity holds exactly at a
	 * resampled node instead of approximately, and the channel cannot change
	 * shape without the water in it changing first.
	 */
	struct FReachResampler
	{
		const FMassifRiverParams& Params;
		const TArray<FMassifRiverNode>& Nodes;

		TArray<double> AlongMeters;
		FMonotoneSpline GroundAltitude;
		FMonotoneSpline WaterSurface;
		FMonotoneSpline Discharge;

		void Prepare()
		{
			MeasureArclength();

			TArray<double> Column;
			GatherAttribute(Nodes, [](const FMassifRiverNode& Node) { return Node.GroundAltitudeMeters; }, Column);
			GroundAltitude.Solve(AlongMeters, Column);
			GatherAttribute(Nodes, [](const FMassifRiverNode& Node) { return Node.WaterSurfaceMeters; }, Column);
			WaterSurface.Solve(AlongMeters, Column);
			GatherAttribute(
				Nodes, [](const FMassifRiverNode& Node) { return Node.DischargeCubicMetersPerSecond; }, Column);
			Discharge.Solve(AlongMeters, Column);
		}

		TArray<FMassifRiverNode> Run() const
		{
			TArray<FMassifRiverNode> Resampled;
			Resampled.Reserve(Nodes.Num() * 2);
			for (int32 SpanIndex = 0; SpanIndex + 1 < Nodes.Num(); ++SpanIndex)
			{
				AppendSpan(SpanIndex, Resampled);
			}

			Resampled.Add(Nodes.Last());
			return Resampled;
		}

	private:
		void MeasureArclength()
		{
			AlongMeters.Reset(Nodes.Num());
			AlongMeters.Add(0.0);
			for (int32 NodeIndex = 1; NodeIndex < Nodes.Num(); ++NodeIndex)
			{
				const double ChordMeters =
					(Nodes[NodeIndex].PositionMeters - Nodes[NodeIndex - 1].PositionMeters).Length();
				AlongMeters.Add(AlongMeters.Last() + FMath::Max(ChordMeters, UE_DOUBLE_KINDA_SMALL_NUMBER));
			}
		}

		void AppendSpan(int32 SpanIndex, TArray<FMassifRiverNode>& OutResampled) const
		{
			const double SpanMeters = AlongMeters[SpanIndex + 1] - AlongMeters[SpanIndex];
			const int32 StepCount =
				FMath::Max(FMath::CeilToInt32(SpanMeters / Params.SmoothingSpacingMeters), 1);
			for (int32 Step = 0; Step < StepCount; ++Step)
			{
				OutResampled.Add(MakeNode(SpanIndex, static_cast<double>(Step) / StepCount));
			}
		}

		FMassifRiverNode MakeNode(int32 SpanIndex, double Fraction) const
		{
			FMassifRiverNode Node;
			Node.PositionMeters = SplinePosition(Nodes, SpanIndex, Fraction);
			Node.GroundAltitudeMeters = GroundAltitude.Evaluate(SpanIndex, Fraction);
			Node.WaterSurfaceMeters = WaterSurface.Evaluate(SpanIndex, Fraction);
			Node.DischargeCubicMetersPerSecond = Discharge.Evaluate(SpanIndex, Fraction);
			ShapeChannel(Params, Node);
			return Node;
		}
	};

	/** End points are kept exactly, so reaches still meet where they met before. */
	void SmoothReach(TArray<FMassifRiverNode>& Nodes, const FMassifRiverParams& Params)
	{
		if (Nodes.Num() < 3 || Params.SmoothingSpacingMeters <= 0.0)
		{
			return;
		}

		FReachResampler Resampler{ Params, Nodes };
		Resampler.Prepare();
		Nodes = Resampler.Run();
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
			// The measured altitude along a channel is the top of the water: a laser
			// scan gets no return off a river bed. So the surface starts as the
			// ground and the bed is put back under it by the carve depth.
			for (int32 RiverIndex = 0; RiverIndex < RiverCells.Num(); ++RiverIndex)
			{
				WaterSurfaceMeters[RiverIndex] = Measured[RiverCells[RiverIndex]];
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

			SimplifyReach(Reach.Nodes, Params);
			SmoothReach(Reach.Nodes, Params);
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
			Node.GroundAltitudeMeters = Measured[CellIndex];
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
