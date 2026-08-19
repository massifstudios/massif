#include "RiverNetwork.h"

#include "MassifSeed.h"

namespace
{
	FVector2f RotateVector(FVector2f Direction, float AngleRadians)
	{
		const float CosAngle = FMath::Cos(AngleRadians);
		const float SinAngle = FMath::Sin(AngleRadians);
		return FVector2f(
			Direction.X * CosAngle - Direction.Y * SinAngle,
			Direction.X * SinAngle + Direction.Y * CosAngle);
	}

	/** River gradient, gentle near the mouth and steep in the headwaters. */
	float RiverSlopeAtDepth(const FWorldGenConfig& Config, int32 DownstreamDepth)
	{
		const float Progress = FMath::Clamp(static_cast<float>(DownstreamDepth) / 8.0f, 0.0f, 1.0f);
		return FMath::Lerp(Config.RiverSlopeNearMouth, Config.RiverSlopeHeadwater, Progress);
	}

	float MaxSourceElevation(const FWorldGenConfig& Config)
	{
		const float AltitudeRange = Config.MaxAltitudeMeters - Config.MinAltitudeMeters;
		return Config.MinAltitudeMeters + Config.RiverMaxSourceAltitudeFraction * AltitudeRange;
	}

	bool IsInsideDomain(const FWorldGenConfig& Config, FVector2f PositionMeters)
	{
		const float Limit = Config.WorldSizeMeters * 0.5f;
		return FMath::Abs(PositionMeters.X) <= Limit && FMath::Abs(PositionMeters.Y) <= Limit;
	}

	bool IsClearOfExistingNodes(const TArray<FRiverNode>& Nodes, FVector2f PositionMeters, float MinSpacingMeters)
	{
		const float MinSpacingSquared = MinSpacingMeters * MinSpacingMeters;
		for (const FRiverNode& Node : Nodes)
		{
			if (FVector2f::DistSquared(Node.PositionMeters, PositionMeters) < MinSpacingSquared)
			{
				return false;
			}
		}
		return true;
	}

	/** Mouths sit on the map border, where water leaves the map at its lowest. */
	void AppendMouths(TArray<FRiverNode>& Nodes, TArray<FVector2f>& GrowthDirections, const FWorldGenConfig& Config, FRandomStream& Stream)
	{
		const float HalfWorld = Config.WorldSizeMeters * 0.5f;
		const int32 Attempts = FMath::Max(1, Config.RiverMouthCount) * 32;
		for (int32 Attempt = 0; Attempt < Attempts && Nodes.Num() < Config.RiverMouthCount; ++Attempt)
		{
			const int32 Side = Stream.RandRange(0, 3);
			const float Along = Stream.FRandRange(-0.8f, 0.8f) * HalfWorld;
			const FVector2f SideDirections[4] = {
				FVector2f(0.0f, 1.0f), FVector2f(0.0f, -1.0f), FVector2f(1.0f, 0.0f), FVector2f(-1.0f, 0.0f) };
			const FVector2f SidePositions[4] = {
				FVector2f(Along, -HalfWorld), FVector2f(Along, HalfWorld), FVector2f(-HalfWorld, Along), FVector2f(HalfWorld, Along) };

			if (!IsClearOfExistingNodes(Nodes, SidePositions[Side], Config.RiverMouthSpacingMeters))
			{
				continue;
			}
			Nodes.Add(FRiverNode{ SidePositions[Side], Config.MinAltitudeMeters, INDEX_NONE, 0, 0 });
			GrowthDirections.Add(SideDirections[Side]);
		}
	}

	/** One upstream step: turns off the parent's heading, climbs by the local river gradient. */
	bool TryAppendChild(
		TArray<FRiverNode>& Nodes, TArray<FVector2f>& GrowthDirections, TArray<int32>& Frontier,
		int32 ParentIndex, float TurnRadians, const FWorldGenConfig& Config, FRandomStream& Stream)
	{
		const FRiverNode& Parent = Nodes[ParentIndex];
		const float Length = Config.RiverSegmentLengthMeters * Stream.FRandRange(0.75f, 1.25f);
		const FVector2f Direction = RotateVector(GrowthDirections[ParentIndex], TurnRadians).GetSafeNormal();
		const FVector2f Position = Parent.PositionMeters + Direction * Length;
		const float Elevation = Parent.ElevationMeters + Length * RiverSlopeAtDepth(Config, Parent.DownstreamDepth);

		const bool bFits = IsInsideDomain(Config, Position)
			&& Elevation <= MaxSourceElevation(Config)
			&& IsClearOfExistingNodes(Nodes, Position, Config.RiverMinNodeSpacingMeters);
		if (!bFits)
		{
			return false;
		}

		Nodes.Add(FRiverNode{ Position, Elevation, ParentIndex, Parent.DownstreamDepth + 1, 0 });
		GrowthDirections.Add(Direction);
		Frontier.Add(Nodes.Num() - 1);
		return true;
	}

	/** A branch that fails on its first heading tries a few others before dying out. */
	bool TryAppendChildAnyHeading(
		TArray<FRiverNode>& Nodes, TArray<FVector2f>& GrowthDirections, TArray<int32>& Frontier,
		int32 ParentIndex, float BaseTurnRadians, const FWorldGenConfig& Config, FRandomStream& Stream)
	{
		const float MaxTurn = FMath::DegreesToRadians(Config.RiverTurnDegrees);
		for (int32 Attempt = 0; Attempt < 5; ++Attempt)
		{
			const float TurnRadians = BaseTurnRadians + Stream.FRandRange(-MaxTurn, MaxTurn);
			if (TryAppendChild(Nodes, GrowthDirections, Frontier, ParentIndex, TurnRadians, Config, Stream))
			{
				return true;
			}
		}
		return false;
	}

	void AppendChildren(
		TArray<FRiverNode>& Nodes, TArray<FVector2f>& GrowthDirections, TArray<int32>& Frontier,
		int32 ParentIndex, const FWorldGenConfig& Config, FRandomStream& Stream)
	{
		if (Stream.FRand() >= Config.RiverBranchProbability)
		{
			TryAppendChildAnyHeading(Nodes, GrowthDirections, Frontier, ParentIndex, 0.0f, Config, Stream);
			return;
		}

		// A fork sends its two branches to either side of the parent's heading.
		const float ForkTurn = FMath::DegreesToRadians(Config.RiverForkDegrees) * Stream.FRandRange(0.7f, 1.3f);
		TryAppendChildAnyHeading(Nodes, GrowthDirections, Frontier, ParentIndex, ForkTurn, Config, Stream);
		TryAppendChildAnyHeading(Nodes, GrowthDirections, Frontier, ParentIndex, -ForkTurn, Config, Stream);
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

	/**
	 * Valley width for one watercourse. A trunk carrying a whole catchment cuts
	 * a broad valley; a headwater stream only notches the slope it sits on, so
	 * its sides climb away steeply and the trunk keeps defining the landform.
	 */
	float ValleyWidthForNode(const FRiverNode& Node, const FWorldGenConfig& Config)
	{
		const float Discharge = FMath::Loge(1.0f + static_cast<float>(Node.UpstreamNodeCount));
		const float TrunkDischarge = FMath::Loge(1.0f + static_cast<float>(Config.RiverMaxNodeCount));
		const float Fraction = FMath::Clamp(Discharge / FMath::Max(1.0f, TrunkDischarge), 0.0f, 1.0f);
		return FMath::Lerp(Config.HeadwaterValleyWidthMeters, Config.TrunkValleyWidthMeters, Fraction);
	}

	/**
	 * Height the ground rises to at a given distance from a watercourse.
	 * Saturating rather than linear, so valley sides ease off into rounded
	 * crests instead of meeting in a spike.
	 */
	float ValleySideProfile(float DistanceMeters, float ValleyWidthMeters, const FWorldGenConfig& Config)
	{
		const float Width = FMath::Max(1.0f, ValleyWidthMeters);
		const float ValleySide = Config.ValleyReliefMeters * (1.0f - FMath::Exp(-DistanceMeters / Width));
		return ValleySide + Config.InterfluveRiseSlope * DistanceMeters;
	}

	/**
	 * Rounds the crest where two valley sides meet. Blending only the two
	 * lowest proposals keeps this exact: an accumulating blend over every
	 * segment drags the whole surface down by roughly the blend width times
	 * the log of the segment count.
	 */
	float PolynomialSmoothMin(float LowerHeight, float HigherHeight, float BlendMeters)
	{
		const float Blend = FMath::Clamp(0.5f + 0.5f * (HigherHeight - LowerHeight) / BlendMeters, 0.0f, 1.0f);
		return FMath::Lerp(HigherHeight, LowerHeight, Blend) - BlendMeters * Blend * (1.0f - Blend);
	}

	float DistanceToSegment(FVector2f Position, FVector2f Start, FVector2f End, float& OutAlongFraction)
	{
		const FVector2f Segment = End - Start;
		const float LengthSquared = Segment.SizeSquared();
		if (LengthSquared <= UE_SMALL_NUMBER)
		{
			OutAlongFraction = 0.0f;
			return FVector2f::Distance(Position, Start);
		}
		OutAlongFraction = FMath::Clamp(FVector2f::DotProduct(Position - Start, Segment) / LengthSquared, 0.0f, 1.0f);
		return FVector2f::Distance(Position, Start + Segment * OutAlongFraction);
	}
}

FRiverNetwork FRiverNetwork::Build(const FWorldGenConfig& Config)
{
	FRandomStream Stream = FMassifSeed(static_cast<uint32>(Config.MasterSeed)).MakeChannelStream(TEXT("WorldGen.RiverNetwork"));

	FRiverNetwork Network;
	Network.Nodes.Reserve(Config.RiverMaxNodeCount);
	TArray<FVector2f> GrowthDirections;
	TArray<int32> Frontier;

	AppendMouths(Network.Nodes, GrowthDirections, Config, Stream);
	for (int32 MouthIndex = 0; MouthIndex < Network.Nodes.Num(); ++MouthIndex)
	{
		Frontier.Add(MouthIndex);
	}

	// Breadth-first, so the trunks are laid down before the headwaters compete for space.
	for (int32 FrontierIndex = 0; FrontierIndex < Frontier.Num() && Network.Nodes.Num() < Config.RiverMaxNodeCount; ++FrontierIndex)
	{
		AppendChildren(Network.Nodes, GrowthDirections, Frontier, Frontier[FrontierIndex], Config, Stream);
	}

	AccumulateUpstreamCounts(Network.Nodes);
	return Network;
}

FRiverTerrainSample FRiverNetwork::Sample(FVector2f PositionMeters, const FWorldGenConfig& Config) const
{
	// Every watercourse proposes a height for this point. The lowest wins,
	// because water cuts down to the valley it belongs to, and the crest
	// between two catchments is where the two lowest proposals meet.
	float LowestHeight = TNumericLimits<float>::Max();
	float SecondLowestHeight = TNumericLimits<float>::Max();
	FRiverTerrainSample Result;
	Result.DistanceToWaterMeters = TNumericLimits<float>::Max();

	for (const FRiverNode& Node : Nodes)
	{
		if (Node.ParentIndex == INDEX_NONE)
		{
			continue;
		}
		const FRiverNode& Downstream = Nodes[Node.ParentIndex];
		float AlongFraction = 0.0f;
		const float Distance = DistanceToSegment(PositionMeters, Node.PositionMeters, Downstream.PositionMeters, AlongFraction);
		const float WaterElevation = FMath::Lerp(Node.ElevationMeters, Downstream.ElevationMeters, AlongFraction);
		const float ProposedHeight = WaterElevation + ValleySideProfile(Distance, ValleyWidthForNode(Node, Config), Config);

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

	if (LowestHeight == TNumericLimits<float>::Max())
	{
		// No watercourse at all: flat ground at the height a valley side levels off to.
		Result.DistanceToWaterMeters = Config.TrunkValleyWidthMeters * 4.0f;
		Result.NearestWaterElevationMeters = Config.MinAltitudeMeters;
		Result.BaseHeightMeters = Config.MinAltitudeMeters + Config.ValleyReliefMeters;
		return Result;
	}

	Result.BaseHeightMeters = PolynomialSmoothMin(LowestHeight, SecondLowestHeight, FMath::Max(1.0f, Config.RidgeBlendMeters));
	return Result;
}
