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

	/**
	 * 0 for a full river, 1 for a headwater trickle.
	 *
	 * Squared, so a watercourse only takes on headwater gradients once it really
	 * is one. Straight off the strength, the first tributary off a trunk is
	 * already half way to a mountain torrent, and its valley floor climbs away
	 * from the trunk fast enough to close off hollows along the junction.
	 */
	double HeadwaterProgress(float StreamStrength)
	{
		const double Weakness = FMath::Clamp(1.0 - static_cast<double>(StreamStrength), 0.0, 1.0);
		return Weakness * Weakness;
	}

	/** River gradient, gentle on a full river and steep in the headwaters. */
	double RiverSlopeForStrength(const FWorldGenConfig& Config, float StreamStrength)
	{
		return FMath::Lerp(
			static_cast<double>(Config.RiverSlopeNearMouth),
			static_cast<double>(Config.RiverSlopeHeadwater),
			HeadwaterProgress(StreamStrength));
	}

	/** Segments shorten as a watercourse weakens, so one graph carries trunk valleys and the streams feeding them. */
	double SegmentLengthForStrength(const FWorldGenConfig& Config, float StreamStrength)
	{
		return FMath::Lerp(
			static_cast<double>(Config.TrunkSegmentLengthMeters),
			static_cast<double>(Config.HeadwaterSegmentLengthMeters),
			HeadwaterProgress(StreamStrength));
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
	};

	/** Log-scaled share of the biggest possible discharge, 0 at a headwater, 1 on the widest trunk. */
	double DischargeFraction(const FRiverNode& Node, const FWorldGenConfig& Config)
	{
		const double Discharge = FMath::Loge(1.0 + static_cast<double>(Node.UpstreamNodeCount));
		const double TrunkDischarge = FMath::Loge(1.0 + static_cast<double>(Config.RiverMaxNodeCount));
		return FMath::Clamp(Discharge / FMath::Max(1.0, TrunkDischarge), 0.0, 1.0);
	}

	FValleyShape ValleyShapeForFraction(double Fraction, const FWorldGenConfig& Config)
	{
		FValleyShape Shape;
		Shape.FloorHalfWidthMeters = FMath::Lerp(
			static_cast<double>(Config.HeadwaterFloorHalfWidthMeters),
			static_cast<double>(Config.TrunkFloorHalfWidthMeters), Fraction);
		Shape.SideWidthMeters = FMath::Lerp(
			static_cast<double>(Config.HeadwaterValleyWidthMeters),
			static_cast<double>(Config.TrunkValleyWidthMeters), Fraction);
		return Shape;
	}

	/**
	 * Farthest a notch reaches from its stream. Notch fades use a smoothstep
	 * that ends exactly here, not the asymptotic valley profile, so a query
	 * only ever has to look this far for streams that could still cut it.
	 */
	double NotchReachMeters(const FWorldGenConfig& Config)
	{
		return static_cast<double>(Config.TrunkFloorHalfWidthMeters) + static_cast<double>(Config.HeadwaterValleyWidthMeters);
	}

	/**
	 * Depth a small stream cuts below the backbone surface at its recorded
	 * distance: down to its own water, but never more than the configured notch
	 * depth, gone entirely at the end of its side width. Zero when the stream
	 * stands above the backbone, so a notch can only ever lower ground.
	 */
	double NotchCutMeters(const FRiverNotchCandidate& Notch, double BackboneHeightMeters, const FWorldGenConfig& Config)
	{
		const double DepthAvailable = BackboneHeightMeters - Notch.WaterElevationMeters;
		if (DepthAvailable <= 0.0)
		{
			return 0.0;
		}
		const double DepthMeters = FMath::Min(DepthAvailable, static_cast<double>(Config.StreamNotchDepthMeters));
		const double SideDistance = FMath::Max(0.0, Notch.DistanceMeters - Notch.FloorHalfWidthMeters);
		const double Side = FMath::Clamp(SideDistance / FMath::Max(1.0, Notch.SideWidthMeters), 0.0, 1.0);
		const double Fade = 1.0 - Side * Side * (3.0 - 2.0 * Side);
		return DepthMeters * Fade;
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
			Nodes.Add(FRiverNode{ SidePositions[Side], static_cast<double>(Config.MinAltitudeMeters), INDEX_NONE, 0, 0, 1.0f });
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
	bool TryAppendChild(FGrowthContext& Context, int32 ParentIndex, double TurnRadians, float ChildStrength)
	{
		const FRiverNode& Parent = Context.Nodes[ParentIndex];
		const double Length = SegmentLengthForStrength(Context.Config, ChildStrength) * Context.Stream.FRandRange(0.75f, 1.25f);
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
			Length * RiverSlopeForStrength(Context.Config, ChildStrength),
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

		Context.Nodes.Add(FRiverNode{ Position, Elevation, ParentIndex, Parent.DownstreamDepth + 1, 0, ChildStrength });
		Context.GrowthDirections.Add(Direction);
		Context.SpacingIndex.Add(Context.Nodes.Num() - 1, Position);
		Context.Frontier.Add(Context.Nodes.Num() - 1);
		return true;
	}

	/** A branch that fails on its first heading tries a few others before dying out. */
	bool TryAppendChildAnyHeading(FGrowthContext& Context, int32 ParentIndex, double BaseTurnRadians, float ChildStrength)
	{
		const double MaxTurn = FMath::DegreesToRadians(static_cast<double>(Context.Config.RiverTurnDegrees));
		for (int32 Attempt = 0; Attempt < 5; ++Attempt)
		{
			const double TurnRadians = BaseTurnRadians + Context.Stream.FRandRange(-MaxTurn, MaxTurn);
			if (TryAppendChild(Context, ParentIndex, TurnRadians, ChildStrength))
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * A watercourse continues, and now and then a tributary joins it.
	 *
	 * Not a symmetric fork. Sending both children off at equal angles with equal
	 * standing gives dichotomous branching — the shape of a lung, not of a river
	 * basin — and no trunk survives more than a couple of steps, so nothing ever
	 * runs far enough through the land to read as a river. Here one child
	 * carries the stem on with barely a turn and keeps almost all its strength,
	 * and the other leaves at a wide angle with a fraction of it.
	 */
	void AppendChildren(FGrowthContext& Context, int32 ParentIndex)
	{
		const float ParentStrength = Context.Nodes[ParentIndex].StreamStrength;
		const float StemStrength = ParentStrength * Context.Config.MainStemStrengthRetention;
		TryAppendChildAnyHeading(Context, ParentIndex, 0.0, StemStrength);

		if (Context.Stream.FRand() >= Context.Config.TributaryProbability)
		{
			return;
		}

		const double JoinAngle = FMath::DegreesToRadians(static_cast<double>(Context.Config.TributaryJoinDegrees)) * Context.Stream.FRandRange(0.7f, 1.3f);
		double TurnRadians = JoinAngle;
		if (Context.Stream.FRand() < 0.5f)
		{
			TurnRadians = -JoinAngle;
		}
		TryAppendChildAnyHeading(Context, ParentIndex, TurnRadians, ParentStrength * Context.Config.TributaryStrengthFactor);
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
	FRiverSampleAccumulator& Accumulator) const
{
	const double CellDistance = CellMinDistanceMeters(Cell, PositionMeters);
	const double LowestWaterHere = IndexCellMinWaterElevation[Cell];
	const double BestPossible = LowestWaterHere
		+ FMath::Max(0.0, CrestAltitudeMeters - LowestWaterHere) * ValleySideFraction(CellDistance, Config.TrunkFloorHalfWidthMeters, Config.TrunkValleyWidthMeters)
		+ Config.InterfluveRiseSlope * CellDistance;
	const bool bCouldStillCut = CellDistance <= NotchReachMeters(Config);
	if (BestPossible > Accumulator.SecondLowestBackboneHeight && !bCouldStillCut)
	{
		return;
	}

	for (int32 Slot = IndexCellStarts[Cell]; Slot < IndexCellStarts[Cell + 1]; ++Slot)
	{
		AccumulateSegment(Nodes[IndexedSegments[Slot]], PositionMeters, CrestAltitudeMeters, Config, Accumulator);
	}
}

void FRiverNetwork::AccumulateSegment(
	const FRiverNode& Node, FVector2d PositionMeters, double CrestAltitudeMeters, const FWorldGenConfig& Config,
	FRiverSampleAccumulator& Accumulator) const
{
	const FRiverNode& Downstream = Nodes[Node.ParentIndex];
	double AlongFraction = 0.0;
	const double Distance = DistanceToSegment(PositionMeters, Node.PositionMeters, Downstream.PositionMeters, AlongFraction);
	const double WaterElevation = FMath::Lerp(Node.ElevationMeters, Downstream.ElevationMeters, AlongFraction);

	if (Distance < Accumulator.Result.DistanceToWaterMeters)
	{
		Accumulator.Result.DistanceToWaterMeters = Distance;
		Accumulator.Result.NearestWaterElevationMeters = WaterElevation;
	}

	const double Fraction = DischargeFraction(Node, Config);
	const FValleyShape Shape = ValleyShapeForFraction(Fraction, Config);

	if (Fraction < Config.BackboneDischargeFraction)
	{
		// Too small to shape the mountainside: remember it as a notch to cut
		// once the backbone height here is known. The side width is capped at
		// the headwater width so no notch reaches past NotchReachMeters.
		const double SideWidthMeters = FMath::Min(Shape.SideWidthMeters, static_cast<double>(Config.HeadwaterValleyWidthMeters));
		if (Distance < Shape.FloorHalfWidthMeters + SideWidthMeters)
		{
			Accumulator.Notches.Add({ Distance, WaterElevation, Shape.FloorHalfWidthMeters, SideWidthMeters });
		}
		return;
	}

	const double Available = FMath::Max(0.0, CrestAltitudeMeters - WaterElevation);
	const double SideFraction = ValleySideFraction(Distance, Shape.FloorHalfWidthMeters, Shape.SideWidthMeters);
	const double ProposedHeight = WaterElevation + Available * SideFraction + Config.InterfluveRiseSlope * Distance;

	if (ProposedHeight < Accumulator.LowestBackboneHeight)
	{
		Accumulator.SecondLowestBackboneHeight = Accumulator.LowestBackboneHeight;
		Accumulator.LowestBackboneHeight = ProposedHeight;
	}
	else if (ProposedHeight < Accumulator.SecondLowestBackboneHeight)
	{
		Accumulator.SecondLowestBackboneHeight = ProposedHeight;
	}
}

FRiverTerrainSample FRiverNetwork::Sample(FVector2d PositionMeters, const FWorldGenConfig& Config, const FMassifUpliftField& Uplift) const
{
	const double CrestAltitudeMeters = Uplift.Sample(PositionMeters).CrestAltitudeMeters;

	// Big watercourses propose the mountainside: the lowest proposal wins,
	// because water cuts down to the valley it belongs to, and the crest
	// between two catchments is where the two lowest proposals meet. Small
	// watercourses do not shape the mountainside at all — they cut notches
	// into whatever surface the big ones settled on.
	FRiverSampleAccumulator Accumulator;
	Accumulator.Result.DistanceToWaterMeters = TNumericLimits<double>::Max();

	const int32 CenterCellX = FMath::Clamp(FMath::FloorToInt32((PositionMeters.X + IndexOriginMeters) / IndexCellSizeMeters), 0, IndexCellsPerSide - 1);
	const int32 CenterCellY = FMath::Clamp(FMath::FloorToInt32((PositionMeters.Y + IndexOriginMeters) / IndexCellSizeMeters), 0, IndexCellsPerSide - 1);

	TArray<int32> RingCells;
	for (int32 RingIndex = 0; RingIndex < IndexCellsPerSide; ++RingIndex)
	{
		const double ClearedRadius = FMath::Max(0.0, (RingIndex - 1) * IndexCellSizeMeters);
		const bool bBackboneSettled =
			LowestPossibleProposalBeyond(ClearedRadius, CrestAltitudeMeters, Config) > Accumulator.SecondLowestBackboneHeight;
		if (bBackboneSettled && ClearedRadius > NotchReachMeters(Config))
		{
			break;
		}

		RingCells.Reset();
		GatherRingCells(RingIndex, CenterCellX, CenterCellY, RingCells);
		for (const int32 Cell : RingCells)
		{
			AccumulateCell(Cell, PositionMeters, CrestAltitudeMeters, Config, Accumulator);
		}
	}

	FRiverTerrainSample Result = Accumulator.Result;
	if (Accumulator.LowestBackboneHeight == TNumericLimits<double>::Max())
	{
		// No big watercourse in the whole world: flat ground at the crest altitude.
		Result.DistanceToWaterMeters = Config.TrunkValleyWidthMeters * 4.0;
		Result.NearestWaterElevationMeters = Config.MinAltitudeMeters;
		Result.HeightMeters = CrestAltitudeMeters;
		Result.ReliefFraction = 1.0f;
		return Result;
	}

	const double BackboneHeight = PolynomialSmoothMin(
		Accumulator.LowestBackboneHeight, Accumulator.SecondLowestBackboneHeight,
		FMath::Max(1.0, static_cast<double>(Config.RidgeBlendMeters)));

	double DeepestCutMeters = 0.0;
	for (const FRiverNotchCandidate& Notch : Accumulator.Notches)
	{
		DeepestCutMeters = FMath::Max(DeepestCutMeters, NotchCutMeters(Notch, BackboneHeight, Config));
	}
	Result.HeightMeters = BackboneHeight - DeepestCutMeters;

	const double AvailableHere = FMath::Max(1.0, CrestAltitudeMeters - Result.NearestWaterElevationMeters);
	Result.ReliefFraction = static_cast<float>(
		FMath::Clamp((Result.HeightMeters - Result.NearestWaterElevationMeters) / AvailableHere, 0.0, 1.0));
	return Result;
}
