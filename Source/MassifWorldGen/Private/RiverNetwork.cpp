#include "RiverNetwork.h"

#include "MassifSeed.h"
#include "MassifUpliftField.h"
#include "WorldGenConfig.h"

namespace
{
	FVector2d RotateVector(FVector2d Direction, double AngleRadians)
	{
		const double CosAngle = FMath::Cos(AngleRadians);
		const double SinAngle = FMath::Sin(AngleRadians);
		return FVector2d(
			Direction.X * CosAngle - Direction.Y * SinAngle,
			Direction.X * SinAngle + Direction.Y * CosAngle);
	}

	/** How far upstream a node sits, 0 at a mouth and 1 once it counts as a headwater. */
	double HeadwaterProgress(const FWorldGenConfig& Config, int32 DownstreamDepth)
	{
		const double Steps = FMath::Max(1, Config.StepsToHeadwater);
		return FMath::Clamp(static_cast<double>(DownstreamDepth) / Steps, 0.0, 1.0);
	}

	/** River gradient, gentle near the mouth and steep in the headwaters. */
	double RiverSlopeAtDepth(const FWorldGenConfig& Config, int32 DownstreamDepth)
	{
		return FMath::Lerp(
			static_cast<double>(Config.RiverSlopeNearMouth),
			static_cast<double>(Config.RiverSlopeHeadwater),
			HeadwaterProgress(Config, DownstreamDepth));
	}

	/** Segments shorten upstream, so one graph carries trunk valleys and the streams feeding them. */
	double SegmentLengthAtDepth(const FWorldGenConfig& Config, int32 DownstreamDepth)
	{
		return FMath::Lerp(
			static_cast<double>(Config.TrunkSegmentLengthMeters),
			static_cast<double>(Config.HeadwaterSegmentLengthMeters),
			HeadwaterProgress(Config, DownstreamDepth));
	}

	bool IsInsideWorld(const FWorldGenConfig& Config, FVector2d PositionMeters)
	{
		const double Limit = Config.WorldSizeMeters * 0.5;
		return FMath::Abs(PositionMeters.X) <= Limit && FMath::Abs(PositionMeters.Y) <= Limit;
	}

	/**
	 * Height the ground rises to at a given distance from a watercourse, as a
	 * fraction of the relief available above it: a flat floor out to the floor
	 * half-width, then a side that leaves the floor at zero gradient, steepens,
	 * and eases off into a rounded crest.
	 *
	 * The steepest point sits partway up the side, which is what a valley looks
	 * like. A profile that saturates from the water outwards is steepest right
	 * at the water instead, so every watercourse ends up in a one-cell V-notch
	 * with no floor to it.
	 */
	double ValleySideFraction(double DistanceMeters, double FloorHalfWidthMeters, double SideWidthMeters)
	{
		const double SideDistance = FMath::Max(0.0, DistanceMeters - FloorHalfWidthMeters);
		const double Climb = SideDistance / FMath::Max(1.0, SideWidthMeters);
		const double ClimbSquared = Climb * Climb;
		return ClimbSquared / (1.0 + ClimbSquared);
	}

	/** Cross-section of the valley one watercourse has cut, set by how much water it carries. */
	struct FValleyShape
	{
		double FloorHalfWidthMeters = 0.0;
		double SideWidthMeters = 0.0;
		/** Share of the relief up to the crest this watercourse is responsible for. */
		double ReliefClaim = 1.0;
	};

	/**
	 * A trunk carrying a whole catchment cuts a broad, deep valley; a headwater
	 * stream only notches the slope it sits on.
	 *
	 * The relief claim is what separates a landform from a sponge. Letting every
	 * watercourse cut down to its own water level, headwaters included, leaves
	 * the ground dissected uniformly at the node spacing: the ridge between two
	 * adjacent headwaters is only as high as their profiles reach halfway
	 * between them, which is barely above the streams. Giving the small ones a
	 * small claim leaves the interfluves to the trunks, so the trunk's valley
	 * sides carry the relief and the streams sit in shallow notches cut into
	 * them, which is the shape a real catchment has.
	 */
	FValleyShape ValleyShapeForNode(const FRiverNode& Node, const FWorldGenConfig& Config)
	{
		const double Discharge = FMath::Loge(1.0 + static_cast<double>(Node.UpstreamNodeCount));
		const double TrunkDischarge = FMath::Loge(1.0 + static_cast<double>(Config.RiverMaxNodeCount));
		const double Fraction = FMath::Clamp(Discharge / FMath::Max(1.0, TrunkDischarge), 0.0, 1.0);

		FValleyShape Shape;
		Shape.FloorHalfWidthMeters = FMath::Lerp(
			static_cast<double>(Config.HeadwaterFloorHalfWidthMeters),
			static_cast<double>(Config.TrunkFloorHalfWidthMeters), Fraction);
		Shape.SideWidthMeters = FMath::Lerp(
			static_cast<double>(Config.HeadwaterValleyWidthMeters),
			static_cast<double>(Config.TrunkValleyWidthMeters), Fraction);
		Shape.ReliefClaim = FMath::Lerp(
			static_cast<double>(Config.HeadwaterReliefClaim), 1.0, Fraction);
		return Shape;
	}

	/**
	 * Rounds the crest where two valley sides meet. Blending only the two lowest
	 * proposals keeps this exact: an accumulating blend over every segment drags
	 * the whole surface down by roughly the blend width times the log of the
	 * segment count.
	 */
	double PolynomialSmoothMin(double LowerHeight, double HigherHeight, double BlendMeters)
	{
		const double Blend = FMath::Clamp(0.5 + 0.5 * (HigherHeight - LowerHeight) / BlendMeters, 0.0, 1.0);
		return FMath::Lerp(HigherHeight, LowerHeight, Blend) - BlendMeters * Blend * (1.0 - Blend);
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
	 * Nodes bucketed by cell during construction, as a head index per cell plus
	 * a next link per node. The spacing test runs once per candidate and would
	 * otherwise walk every node placed so far, which over a world this size is
	 * the difference between a graph that builds in milliseconds and one that
	 * does not build at all.
	 */
	struct FNodeSpacingIndex
	{
		double CellSizeMeters = 0.0;
		int32 CellsPerSide = 0;
		double OriginMeters = 0.0;
		TArray<int32> CellHead;
		TArray<int32> NodeNext;

		FNodeSpacingIndex(const FWorldGenConfig& Config)
			: CellSizeMeters(FMath::Max(1.0, static_cast<double>(Config.RiverMinNodeSpacingMeters)))
			, OriginMeters(Config.WorldSizeMeters * 0.5)
		{
			CellsPerSide = FMath::Max(1, FMath::CeilToInt32(Config.WorldSizeMeters / CellSizeMeters) + 1);
			CellHead.Init(INDEX_NONE, CellsPerSide * CellsPerSide);
		}

		int32 CellCoordinate(double PositionMeters) const
		{
			return FMath::Clamp(FMath::FloorToInt32((PositionMeters + OriginMeters) / CellSizeMeters), 0, CellsPerSide - 1);
		}

		void Add(int32 NodeIndex, FVector2d PositionMeters)
		{
			const int32 Cell = CellCoordinate(PositionMeters.Y) * CellsPerSide + CellCoordinate(PositionMeters.X);
			NodeNext.SetNum(FMath::Max(NodeNext.Num(), NodeIndex + 1));
			NodeNext[NodeIndex] = CellHead[Cell];
			CellHead[Cell] = NodeIndex;
		}

		bool IsClear(const TArray<FRiverNode>& Nodes, FVector2d PositionMeters, double MinSpacingMeters) const
		{
			const int32 CenterX = CellCoordinate(PositionMeters.X);
			const int32 CenterY = CellCoordinate(PositionMeters.Y);
			const double MinSpacingSquared = MinSpacingMeters * MinSpacingMeters;
			for (int32 Neighbor = 0; Neighbor < 9; ++Neighbor)
			{
				const int32 CellX = FMath::Clamp(CenterX + (Neighbor % 3) - 1, 0, CellsPerSide - 1);
				const int32 CellY = FMath::Clamp(CenterY + (Neighbor / 3) - 1, 0, CellsPerSide - 1);
				if (!IsCellClear(Nodes, CellY * CellsPerSide + CellX, PositionMeters, MinSpacingSquared))
				{
					return false;
				}
			}
			return true;
		}

		bool IsCellClear(const TArray<FRiverNode>& Nodes, int32 Cell, FVector2d PositionMeters, double MinSpacingSquared) const
		{
			for (int32 NodeIndex = CellHead[Cell]; NodeIndex != INDEX_NONE; NodeIndex = NodeNext[NodeIndex])
			{
				if (FVector2d::DistSquared(Nodes[NodeIndex].PositionMeters, PositionMeters) < MinSpacingSquared)
				{
					return false;
				}
			}
			return true;
		}
	};

	/** Mouths sit on the world border, where water leaves the map at its lowest. */
	void AppendMouths(
		TArray<FRiverNode>& Nodes, TArray<FVector2d>& GrowthDirections, FNodeSpacingIndex& SpacingIndex,
		const FWorldGenConfig& Config, FRandomStream& Stream)
	{
		const double HalfWorld = Config.WorldSizeMeters * 0.5;
		const int32 Attempts = FMath::Max(1, Config.RiverMouthCount) * 64;
		for (int32 Attempt = 0; Attempt < Attempts && Nodes.Num() < Config.RiverMouthCount; ++Attempt)
		{
			const int32 Side = Stream.RandRange(0, 3);
			const double Along = Stream.FRandRange(-0.92f, 0.92f) * HalfWorld;
			const FVector2d SideDirections[4] = {
				FVector2d(0.0, 1.0), FVector2d(0.0, -1.0), FVector2d(1.0, 0.0), FVector2d(-1.0, 0.0) };
			const FVector2d SidePositions[4] = {
				FVector2d(Along, -HalfWorld), FVector2d(Along, HalfWorld), FVector2d(-HalfWorld, Along), FVector2d(HalfWorld, Along) };

			if (!SpacingIndex.IsClear(Nodes, SidePositions[Side], Config.RiverMouthSpacingMeters))
			{
				continue;
			}
			Nodes.Add(FRiverNode{ SidePositions[Side], static_cast<double>(Config.MinAltitudeMeters), INDEX_NONE, 0, 0 });
			GrowthDirections.Add(SideDirections[Side]);
			SpacingIndex.Add(Nodes.Num() - 1, SidePositions[Side]);
		}
	}

	struct FGrowthContext
	{
		TArray<FRiverNode>& Nodes;
		TArray<FVector2d>& GrowthDirections;
		TArray<int32>& Frontier;
		FNodeSpacingIndex& SpacingIndex;
		const FWorldGenConfig& Config;
		const FMassifUpliftField& Uplift;
		FRandomStream& Stream;
	};

	/** One upstream step: turns off the parent's heading, climbs by the local river gradient. */
	bool TryAppendChild(FGrowthContext& Context, int32 ParentIndex, double TurnRadians)
	{
		const FRiverNode& Parent = Context.Nodes[ParentIndex];
		const double Length = SegmentLengthAtDepth(Context.Config, Parent.DownstreamDepth) * Context.Stream.FRandRange(0.75f, 1.25f);
		const FVector2d Direction = RotateVector(Context.GrowthDirections[ParentIndex], TurnRadians).GetSafeNormal();
		const FVector2d Position = Parent.PositionMeters + Direction * Length;
		if (!IsInsideWorld(Context.Config, Position))
		{
			return false;
		}

		// The uplift field decides how high the ground stands here, so it also
		// decides how far a watercourse may climb before it would be running
		// over the range instead of through it. A watercourse approaches that
		// ceiling asymptotically rather than being cut off at it: cutting off
		// leaves every low basin undrained, because a river crossing one has
		// already climbed above the ceiling a basin sets and can never enter.
		// Easing in instead lets rivers run flat across basins and climb hard
		// through ranges, which is what sets a valley floor's gradient anyway.
		const double Headroom = Context.Uplift.MaxWaterAltitudeMeters(Position) - Parent.ElevationMeters;
		if (Headroom <= 0.0)
		{
			return false;
		}
		const double Climb = FMath::Min(
			Length * RiverSlopeAtDepth(Context.Config, Parent.DownstreamDepth),
			Headroom * Context.Config.RiverHeadroomClimbFraction);
		if (Climb < UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			return false;
		}
		const double Elevation = Parent.ElevationMeters + Climb;

		if (!Context.SpacingIndex.IsClear(Context.Nodes, Position, Context.Config.RiverMinNodeSpacingMeters))
		{
			return false;
		}

		Context.Nodes.Add(FRiverNode{ Position, Elevation, ParentIndex, Parent.DownstreamDepth + 1, 0 });
		Context.GrowthDirections.Add(Direction);
		Context.SpacingIndex.Add(Context.Nodes.Num() - 1, Position);
		Context.Frontier.Add(Context.Nodes.Num() - 1);
		return true;
	}

	/** A branch that fails on its first heading tries a few others before dying out. */
	bool TryAppendChildAnyHeading(FGrowthContext& Context, int32 ParentIndex, double BaseTurnRadians)
	{
		const double MaxTurn = FMath::DegreesToRadians(static_cast<double>(Context.Config.RiverTurnDegrees));
		for (int32 Attempt = 0; Attempt < 5; ++Attempt)
		{
			const double TurnRadians = BaseTurnRadians + Context.Stream.FRandRange(-MaxTurn, MaxTurn);
			if (TryAppendChild(Context, ParentIndex, TurnRadians))
			{
				return true;
			}
		}
		return false;
	}

	void AppendChildren(FGrowthContext& Context, int32 ParentIndex)
	{
		if (Context.Stream.FRand() >= Context.Config.RiverBranchProbability)
		{
			TryAppendChildAnyHeading(Context, ParentIndex, 0.0);
			return;
		}

		// A fork sends its two branches to either side of the parent's heading.
		const double ForkTurn = FMath::DegreesToRadians(static_cast<double>(Context.Config.RiverForkDegrees)) * Context.Stream.FRandRange(0.7f, 1.3f);
		TryAppendChildAnyHeading(Context, ParentIndex, ForkTurn);
		TryAppendChildAnyHeading(Context, ParentIndex, -ForkTurn);
	}

	/**
	 * Children are always appended after their parent, so one reverse pass
	 * accumulates every node's drainage into the one below it.
	 */
	void AccumulateUpstreamCounts(TArray<FRiverNode>& Nodes)
	{
		for (int32 NodeIndex = Nodes.Num() - 1; NodeIndex >= 0; --NodeIndex)
		{
			const int32 ParentIndex = Nodes[NodeIndex].ParentIndex;
			if (ParentIndex != INDEX_NONE)
			{
				Nodes[ParentIndex].UpstreamNodeCount += Nodes[NodeIndex].UpstreamNodeCount + 1;
			}
		}
	}
}

FRiverNetwork FRiverNetwork::Build(const FWorldGenConfig& Config, const FMassifUpliftField& Uplift)
{
	FRandomStream Stream = FMassifSeed(static_cast<uint32>(Config.MasterSeed)).MakeChannelStream(TEXT("WorldGen.RiverNetwork"));

	FRiverNetwork Network;
	Network.Nodes.Reserve(Config.RiverMaxNodeCount);
	TArray<FVector2d> GrowthDirections;
	TArray<int32> Frontier;
	FNodeSpacingIndex SpacingIndex(Config);

	AppendMouths(Network.Nodes, GrowthDirections, SpacingIndex, Config, Stream);
	for (int32 MouthIndex = 0; MouthIndex < Network.Nodes.Num(); ++MouthIndex)
	{
		Frontier.Add(MouthIndex);
	}

	FGrowthContext Context{ Network.Nodes, GrowthDirections, Frontier, SpacingIndex, Config, Uplift, Stream };

	// Breadth-first, so the trunks are laid down before the headwaters compete for space.
	for (int32 FrontierIndex = 0; FrontierIndex < Frontier.Num() && Network.Nodes.Num() < Config.RiverMaxNodeCount; ++FrontierIndex)
	{
		AppendChildren(Context, Frontier[FrontierIndex]);
	}

	AccumulateUpstreamCounts(Network.Nodes);
	Network.BuildSegmentIndex(Config);
	return Network;
}

void FRiverNetwork::BuildSegmentIndex(const FWorldGenConfig& Config)
{
	IndexCellSizeMeters = FMath::Max(1.0, static_cast<double>(Config.TrunkSegmentLengthMeters) * 0.5);
	IndexOriginMeters = Config.WorldSizeMeters * 0.5;
	IndexCellsPerSide = FMath::Max(1, FMath::CeilToInt32(Config.WorldSizeMeters / IndexCellSizeMeters) + 1);

	// Two passes so the segment lists land in one flat array: count per cell,
	// prefix-sum into starts, then fill.
	const int32 CellCount = IndexCellsPerSide * IndexCellsPerSide;
	TArray<int32> CellCounts;
	CellCounts.Init(0, CellCount);

	auto ForEachOverlappedCell = [this](FVector2d Start, FVector2d End, auto&& Visit)
	{
		const int32 LowX = FMath::Clamp(FMath::FloorToInt32((FMath::Min(Start.X, End.X) + IndexOriginMeters) / IndexCellSizeMeters), 0, IndexCellsPerSide - 1);
		const int32 HighX = FMath::Clamp(FMath::FloorToInt32((FMath::Max(Start.X, End.X) + IndexOriginMeters) / IndexCellSizeMeters), 0, IndexCellsPerSide - 1);
		const int32 LowY = FMath::Clamp(FMath::FloorToInt32((FMath::Min(Start.Y, End.Y) + IndexOriginMeters) / IndexCellSizeMeters), 0, IndexCellsPerSide - 1);
		const int32 HighY = FMath::Clamp(FMath::FloorToInt32((FMath::Max(Start.Y, End.Y) + IndexOriginMeters) / IndexCellSizeMeters), 0, IndexCellsPerSide - 1);
		for (int32 CellY = LowY; CellY <= HighY; ++CellY)
		{
			for (int32 CellX = LowX; CellX <= HighX; ++CellX)
			{
				Visit(CellY * IndexCellsPerSide + CellX);
			}
		}
	};

	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		if (Nodes[NodeIndex].ParentIndex == INDEX_NONE)
		{
			continue;
		}
		ForEachOverlappedCell(Nodes[NodeIndex].PositionMeters, Nodes[Nodes[NodeIndex].ParentIndex].PositionMeters,
			[&CellCounts](int32 Cell) { ++CellCounts[Cell]; });
	}

	IndexCellStarts.SetNumUninitialized(CellCount + 1);
	int32 RunningTotal = 0;
	for (int32 Cell = 0; Cell < CellCount; ++Cell)
	{
		IndexCellStarts[Cell] = RunningTotal;
		RunningTotal += CellCounts[Cell];
	}
	IndexCellStarts[CellCount] = RunningTotal;

	IndexedSegments.SetNumUninitialized(RunningTotal);
	IndexCellMinWaterElevation.Init(TNumericLimits<float>::Max(), CellCount);
	TArray<int32> FillCursors = IndexCellStarts;
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		if (Nodes[NodeIndex].ParentIndex == INDEX_NONE)
		{
			continue;
		}
		const float LowestWater = static_cast<float>(FMath::Min(
			Nodes[NodeIndex].ElevationMeters, Nodes[Nodes[NodeIndex].ParentIndex].ElevationMeters));
		ForEachOverlappedCell(Nodes[NodeIndex].PositionMeters, Nodes[Nodes[NodeIndex].ParentIndex].PositionMeters,
			[this, &FillCursors, NodeIndex, LowestWater](int32 Cell)
			{
				IndexedSegments[FillCursors[Cell]++] = NodeIndex;
				IndexCellMinWaterElevation[Cell] = FMath::Min(IndexCellMinWaterElevation[Cell], LowestWater);
			});
	}
}

double FRiverNetwork::CellMinDistanceMeters(int32 Cell, FVector2d PositionMeters) const
{
	const double LowX = -IndexOriginMeters + (Cell % IndexCellsPerSide) * IndexCellSizeMeters;
	const double LowY = -IndexOriginMeters + (Cell / IndexCellsPerSide) * IndexCellSizeMeters;
	const double OutsideX = FMath::Max3(LowX - PositionMeters.X, 0.0, PositionMeters.X - (LowX + IndexCellSizeMeters));
	const double OutsideY = FMath::Max3(LowY - PositionMeters.Y, 0.0, PositionMeters.Y - (LowY + IndexCellSizeMeters));
	return FMath::Sqrt(OutsideX * OutsideX + OutsideY * OutsideY);
}

void FRiverNetwork::GatherRingCells(int32 RingIndex, int32 CenterCellX, int32 CenterCellY, TArray<int32>& OutCells) const
{
	for (int32 OffsetY = -RingIndex; OffsetY <= RingIndex; ++OffsetY)
	{
		// Only the ring's own edge, so a cell is visited by exactly one ring.
		int32 StepX = 1;
		if (FMath::Abs(OffsetY) != RingIndex)
		{
			StepX = FMath::Max(1, 2 * RingIndex);
		}
		for (int32 OffsetX = -RingIndex; OffsetX <= RingIndex; OffsetX += StepX)
		{
			const int32 CellX = CenterCellX + OffsetX;
			const int32 CellY = CenterCellY + OffsetY;
			const bool bInsideIndex = CellX >= 0 && CellX < IndexCellsPerSide && CellY >= 0 && CellY < IndexCellsPerSide;
			if (bInsideIndex)
			{
				OutCells.Add(CellY * IndexCellsPerSide + CellX);
			}
		}
	}
}

double FRiverNetwork::LowestPossibleProposalBeyond(double RadiusMeters, double CrestAltitudeMeters, const FWorldGenConfig& Config) const
{
	// Lowest water anywhere, in the widest valley there is: nothing farther out
	// can propose less than this. Widening the valley and lowering the water
	// both only lower the proposal, so this is a true bound.
	const double LowestWater = Config.MinAltitudeMeters;
	const double Available = FMath::Max(0.0, CrestAltitudeMeters - LowestWater);
	const double Fraction = ValleySideFraction(RadiusMeters, Config.TrunkFloorHalfWidthMeters, Config.TrunkValleyWidthMeters);
	return LowestWater + Available * Fraction + Config.InterfluveRiseSlope * RadiusMeters;
}

void FRiverNetwork::AccumulateCell(
	int32 Cell, FVector2d PositionMeters, double CrestAltitudeMeters, const FWorldGenConfig& Config,
	double& LowestHeight, double& SecondLowestHeight, FRiverTerrainSample& Result) const
{
	const double CellDistance = CellMinDistanceMeters(Cell, PositionMeters);
	const double LowestWaterHere = IndexCellMinWaterElevation[Cell];
	const double BestPossible = LowestWaterHere
		+ FMath::Max(0.0, CrestAltitudeMeters - LowestWaterHere) * ValleySideFraction(CellDistance, Config.TrunkFloorHalfWidthMeters, Config.TrunkValleyWidthMeters)
		+ Config.InterfluveRiseSlope * CellDistance;
	if (BestPossible > SecondLowestHeight)
	{
		return;
	}

	for (int32 Slot = IndexCellStarts[Cell]; Slot < IndexCellStarts[Cell + 1]; ++Slot)
	{
		const FRiverNode& Node = Nodes[IndexedSegments[Slot]];
		const FRiverNode& Downstream = Nodes[Node.ParentIndex];
		double AlongFraction = 0.0;
		const double Distance = DistanceToSegment(PositionMeters, Node.PositionMeters, Downstream.PositionMeters, AlongFraction);
		const double WaterElevation = FMath::Lerp(Node.ElevationMeters, Downstream.ElevationMeters, AlongFraction);

		const FValleyShape Shape = ValleyShapeForNode(Node, Config);
		const double Available = FMath::Max(0.0, CrestAltitudeMeters - WaterElevation) * Shape.ReliefClaim;
		const double Fraction = ValleySideFraction(Distance, Shape.FloorHalfWidthMeters, Shape.SideWidthMeters);
		const double ProposedHeight = WaterElevation + Available * Fraction + Config.InterfluveRiseSlope * Distance;

		if (ProposedHeight < LowestHeight)
		{
			SecondLowestHeight = LowestHeight;
			LowestHeight = ProposedHeight;
		}
		else if (ProposedHeight < SecondLowestHeight)
		{
			SecondLowestHeight = ProposedHeight;
		}

		if (Distance < Result.DistanceToWaterMeters)
		{
			Result.DistanceToWaterMeters = Distance;
			Result.NearestWaterElevationMeters = WaterElevation;
		}
	}
}

FRiverTerrainSample FRiverNetwork::Sample(FVector2d PositionMeters, const FWorldGenConfig& Config, const FMassifUpliftField& Uplift) const
{
	const double CrestAltitudeMeters = Uplift.Sample(PositionMeters).CrestAltitudeMeters;

	// Every watercourse proposes a height for this point. The lowest wins,
	// because water cuts down to the valley it belongs to, and the crest
	// between two catchments is where the two lowest proposals meet.
	double LowestHeight = TNumericLimits<double>::Max();
	double SecondLowestHeight = TNumericLimits<double>::Max();
	FRiverTerrainSample Result;
	Result.DistanceToWaterMeters = TNumericLimits<double>::Max();

	const int32 CenterCellX = FMath::Clamp(FMath::FloorToInt32((PositionMeters.X + IndexOriginMeters) / IndexCellSizeMeters), 0, IndexCellsPerSide - 1);
	const int32 CenterCellY = FMath::Clamp(FMath::FloorToInt32((PositionMeters.Y + IndexOriginMeters) / IndexCellSizeMeters), 0, IndexCellsPerSide - 1);

	TArray<int32> RingCells;
	for (int32 RingIndex = 0; RingIndex < IndexCellsPerSide; ++RingIndex)
	{
		const double ClearedRadius = FMath::Max(0.0, (RingIndex - 1) * IndexCellSizeMeters);
		if (LowestPossibleProposalBeyond(ClearedRadius, CrestAltitudeMeters, Config) > SecondLowestHeight)
		{
			break;
		}

		RingCells.Reset();
		GatherRingCells(RingIndex, CenterCellX, CenterCellY, RingCells);
		for (const int32 Cell : RingCells)
		{
			AccumulateCell(Cell, PositionMeters, CrestAltitudeMeters, Config, LowestHeight, SecondLowestHeight, Result);
		}
	}

	if (LowestHeight == TNumericLimits<double>::Max())
	{
		// No watercourse in the whole world: flat ground at the crest altitude.
		Result.DistanceToWaterMeters = Config.TrunkValleyWidthMeters * 4.0;
		Result.NearestWaterElevationMeters = Config.MinAltitudeMeters;
		Result.HeightMeters = CrestAltitudeMeters;
		Result.ReliefFraction = 1.0f;
		return Result;
	}

	Result.HeightMeters = PolynomialSmoothMin(LowestHeight, SecondLowestHeight, FMath::Max(1.0, static_cast<double>(Config.RidgeBlendMeters)));

	const double AvailableHere = FMath::Max(1.0, CrestAltitudeMeters - Result.NearestWaterElevationMeters);
	Result.ReliefFraction = static_cast<float>(
		FMath::Clamp((Result.HeightMeters - Result.NearestWaterElevationMeters) / AvailableHere, 0.0, 1.0));
	return Result;
}
