#include "MassifChannelProfile.h"

#include "Async/ParallelFor.h"
#include "MassifChannelHydraulics.h"
#include "MassifElevationSource.h"
#include "MassifRiverNetwork.h"

namespace
{
	// -----------------------------------------------------------------------
	// Taking the section
	// -----------------------------------------------------------------------

	/** Downstream at one node, from the stations either side of it. */
	FVector2d FlowDirectionAt(const TArray<FMassifRiverNode>& Nodes, int32 NodeIndex)
	{
		const int32 BeforeIndex = FMath::Max(NodeIndex - 1, 0);
		const int32 AfterIndex = FMath::Min(NodeIndex + 1, Nodes.Num() - 1);
		const FVector2d Along = Nodes[AfterIndex].PositionMeters - Nodes[BeforeIndex].PositionMeters;
		if (Along.SquaredLength() <= UE_DOUBLE_SMALL_NUMBER)
		{
			return FVector2d(1.0, 0.0);
		}
		return Along.GetSafeNormal();
	}

	double ProfileHalfSpanMeters(const FMassifChannelSolveParams& Params, const FMassifRiverNode& Node)
	{
		const double SpanMeters =
			FMath::Max(Params.ProfileSpanFactor * Node.WidthMeters, Params.MinProfileSpanMeters);
		return 0.5 * SpanMeters;
	}

	/**
	 * The ground across the flow at one station.
	 *
	 * Sampled at the offset spacing rather than at the model's finest, so the
	 * section is the surface something built at this spacing would stand on and not
	 * a finer one aliased down onto it.
	 */
	void SampleSection(
		const IMassifElevationSource& Elevation, const FMassifRiverNode& Node, FVector2d FlowDirection,
		FMassifChannelProfile& OutProfile)
	{
		const FVector2d AcrossFlow(-FlowDirection.Y, FlowDirection.X);
		const double SpacingMeters =
			FMath::Max(OutProfile.OffsetSpacingMeters(), Elevation.GetFinestSpacingMeters());

		for (int32 OffsetIndex = 0; OffsetIndex < MassifChannelProfileOffsetCount; ++OffsetIndex)
		{
			const FVector2d PositionMeters =
				Node.PositionMeters + AcrossFlow * OutProfile.OffsetMeters(OffsetIndex);
			OutProfile.GroundAltitudeMeters[OffsetIndex] =
				static_cast<float>(Elevation.SampleAltitudeMeters(PositionMeters, SpacingMeters));
			OutProfile.BedAltitudeMeters[OffsetIndex] = OutProfile.GroundAltitudeMeters[OffsetIndex];
		}
	}

	// -----------------------------------------------------------------------
	// Which of the section the trench is cut through
	// -----------------------------------------------------------------------

	/** The run of offsets the trench spans, and the lowest ground in it. */
	struct FTrenchBand
	{
		int32 FirstOffset = INDEX_NONE;
		int32 LastOffset = INDEX_NONE;
		int32 DeepestOffset = 0;
		double LowestGroundMeters = 0.0;

		bool IsValid() const
		{
			return FirstOffset != INDEX_NONE;
		}

		int32 OffsetCount() const
		{
			return LastOffset - FirstOffset + 1;
		}
	};

	int32 FindLowestGroundOffset(const FMassifChannelProfile& Profile)
	{
		int32 DeepestOffset = 0;
		for (int32 OffsetIndex = 1; OffsetIndex < MassifChannelProfileOffsetCount; ++OffsetIndex)
		{
			if (Profile.GroundAltitudeMeters[OffsetIndex] < Profile.GroundAltitudeMeters[DeepestOffset])
			{
				DeepestOffset = OffsetIndex;
			}
		}
		return DeepestOffset;
	}

	/**
	 * The offsets under the water, taken as the run around the deepest of them.
	 *
	 * A run rather than every offset below the surface: a section wide enough to
	 * find both banks is often wide enough to clip a ditch or a side channel beyond
	 * one of them, and a width measured across both would be the distance between
	 * two separate waters.
	 */
	FTrenchBand FindTrenchBand(const FMassifChannelProfile& Profile, double SurfaceMeters)
	{
		FTrenchBand Band;
		Band.DeepestOffset = FindLowestGroundOffset(Profile);
		Band.LowestGroundMeters = Profile.GroundAltitudeMeters[Band.DeepestOffset];
		if (Band.LowestGroundMeters > SurfaceMeters)
		{
			return Band;
		}

		Band.FirstOffset = Band.DeepestOffset;
		while (Band.FirstOffset > 0 && Profile.GroundAltitudeMeters[Band.FirstOffset - 1] <= SurfaceMeters)
		{
			--Band.FirstOffset;
		}

		Band.LastOffset = Band.DeepestOffset;
		while (Band.LastOffset < MassifChannelProfileOffsetCount - 1
			&& Profile.GroundAltitudeMeters[Band.LastOffset + 1] <= SurfaceMeters)
		{
			++Band.LastOffset;
		}
		return Band;
	}

	/** Drops the end further from the deepest offset, so the trench stays over the thalweg. */
	void DropFurtherEnd(FTrenchBand& InOutBand)
	{
		const int32 BeforeDeepest = InOutBand.DeepestOffset - InOutBand.FirstOffset;
		const int32 AfterDeepest = InOutBand.LastOffset - InOutBand.DeepestOffset;
		if (BeforeDeepest > AfterDeepest)
		{
			++InOutBand.FirstOffset;
			return;
		}
		--InOutBand.LastOffset;
	}

	bool ShouldGrowFirst(const FTrenchBand& Band, bool bCanGrowLast)
	{
		if (!bCanGrowLast)
		{
			return true;
		}
		return Band.DeepestOffset - Band.FirstOffset <= Band.LastOffset - Band.DeepestOffset;
	}

	/** Adds one offset to the shorter side, or reports that the profile has run out. */
	bool GrowShorterEnd(FTrenchBand& InOutBand)
	{
		const bool bCanGrowFirst = InOutBand.FirstOffset > 0;
		const bool bCanGrowLast = InOutBand.LastOffset < MassifChannelProfileOffsetCount - 1;
		if (bCanGrowFirst && ShouldGrowFirst(InOutBand, bCanGrowLast))
		{
			--InOutBand.FirstOffset;
			return true;
		}
		if (bCanGrowLast)
		{
			++InOutBand.LastOffset;
			return true;
		}
		return false;
	}

	/**
	 * Holds the band inside a band of the width the discharge alone predicts.
	 *
	 * A guard, not a model. A section with no channel cut into it reads as either
	 * the whole valley floor or a single offset, and a river should be allowed to
	 * be neither.
	 */
	void ClampBandWidth(
		FTrenchBand& InOutBand, const FMassifChannelProfile& Profile, const FMassifChannelSolveParams& Params,
		double HydraulicWidthMeters)
	{
		const double SpacingMeters = Profile.OffsetSpacingMeters();
		const int32 MinOffsetCount = FMath::Clamp(
			FMath::CeilToInt32(Params.MinWettedWidthFactor * HydraulicWidthMeters / SpacingMeters) + 1,
			2, MassifChannelProfileOffsetCount);
		const int32 MaxOffsetCount = FMath::Clamp(
			FMath::FloorToInt32(Params.MaxWettedWidthFactor * HydraulicWidthMeters / SpacingMeters) + 1,
			MinOffsetCount, MassifChannelProfileOffsetCount);

		while (InOutBand.OffsetCount() > MaxOffsetCount)
		{
			DropFurtherEnd(InOutBand);
		}
		while (InOutBand.OffsetCount() < MinOffsetCount && GrowShorterEnd(InOutBand))
		{
		}
	}

	// -----------------------------------------------------------------------
	// Cutting it
	// -----------------------------------------------------------------------

	/**
	 * The trench cut at one offset, as a share of its depth at the middle.
	 *
	 * Round rather than flat-bottomed. A rectangular cut stands the whole column
	 * against a wall at each bank, which reads as a hard line down both sides of
	 * every river in the world.
	 */
	double TrenchShare(const FTrenchBand& Band, int32 OffsetIndex)
	{
		if (Band.OffsetCount() <= 1)
		{
			return 1.0;
		}
		const double Centre = 0.5 * (Band.FirstOffset + Band.LastOffset);
		const double HalfCount = 0.5 * (Band.OffsetCount() - 1);
		const double AcrossFraction = (OffsetIndex - Centre) / HalfCount;
		return FMath::Max(1.0 - AcrossFraction * AcrossFraction, 0.0);
	}

	/** Puts the bed back to the ground with the trench cut through the band. */
	void CutTrench(FMassifChannelProfile& InOutProfile, const FTrenchBand& Band, double TrenchDepthMeters)
	{
		for (int32 OffsetIndex = 0; OffsetIndex < MassifChannelProfileOffsetCount; ++OffsetIndex)
		{
			InOutProfile.BedAltitudeMeters[OffsetIndex] = InOutProfile.GroundAltitudeMeters[OffsetIndex];
		}
		for (int32 OffsetIndex = Band.FirstOffset; OffsetIndex <= Band.LastOffset; ++OffsetIndex)
		{
			InOutProfile.BedAltitudeMeters[OffsetIndex] = static_cast<float>(
				InOutProfile.GroundAltitudeMeters[OffsetIndex]
					- TrenchDepthMeters * TrenchShare(Band, OffsetIndex));
		}
	}

	/**
	 * How far the band has to be cut for the section to carry the discharge.
	 *
	 * Bisected over a fixed count rather than to a tolerance (ARCHITECTURE §4).
	 * What is solved for is only the column the elevation model could not see:
	 * whatever trench it did record is already carrying part of the flow.
	 */
	double SolveTrenchDepthMeters(
		FMassifChannelProfile& InOutProfile, const FTrenchBand& Band, const FMassifChannelSolveParams& Params,
		double SurfaceMeters, double DischargeCubicMetersPerSecond, double MaxDepthMeters)
	{
		double LowMeters = 0.0;
		double HighMeters = MaxDepthMeters;
		for (int32 Iteration = 0; Iteration < Params.DepthIterations; ++Iteration)
		{
			const double MiddleMeters = 0.5 * (LowMeters + HighMeters);
			CutTrench(InOutProfile, Band, MiddleMeters);
			const FMassifSectionFlow Section =
				MassifChannelHydraulics::MeasureSection(InOutProfile, SurfaceMeters);
			if (MassifChannelHydraulics::ManningDischarge(Section, Params.Roughness, InOutProfile.EnergySlope)
				< DischargeCubicMetersPerSecond)
			{
				LowMeters = MiddleMeters;
				continue;
			}
			HighMeters = MiddleMeters;
		}
		return 0.5 * (LowMeters + HighMeters);
	}

	// -----------------------------------------------------------------------
	// One station, start to finish
	// -----------------------------------------------------------------------

	/**
	 * What a station falls back to where its section holds no water at all.
	 *
	 * The surface is clamped monotone downstream, so a station standing in a hollow
	 * can end up below every point of its own section. The channel is still there —
	 * the clamp moved the surface, not the river — so the hydraulic geometry stands
	 * in, cut as a trench of its own width around the lowest ground.
	 */
	void FallBackToHydraulicGeometry(
		const FMassifChannelSolveParams& Params, const FMassifRiverNode& Node, FMassifChannelProfile& OutProfile)
	{
		OutProfile.WettedWidthMeters = Node.WidthMeters;
		OutProfile.DepthMeters = FMath::Max(Node.DepthMeters, Params.MinDepthMeters);

		FTrenchBand Band;
		Band.DeepestOffset = FindLowestGroundOffset(OutProfile);
		Band.LowestGroundMeters = OutProfile.GroundAltitudeMeters[Band.DeepestOffset];
		Band.FirstOffset = Band.DeepestOffset;
		Band.LastOffset = Band.DeepestOffset;
		ClampBandWidth(Band, OutProfile, Params, Node.WidthMeters);
		CutTrench(OutProfile, Band, OutProfile.DepthMeters);

		// The round trench again: two thirds of the box it sits in.
		OutProfile.FlowAreaSquareMeters =
			(2.0 / 3.0) * OutProfile.WettedWidthMeters * OutProfile.DepthMeters;
		OutProfile.SpeedMetersPerSecond = MassifChannelHydraulics::MeanSpeedMetersPerSecond(
			Node.DischargeCubicMetersPerSecond, OutProfile.FlowAreaSquareMeters);

		FMassifSectionFlow Section;
		Section.AreaSquareMeters = OutProfile.FlowAreaSquareMeters;
		Section.TopWidthMeters = OutProfile.WettedWidthMeters;
		OutProfile.FroudeNumber =
			MassifChannelHydraulics::FroudeNumber(OutProfile.SpeedMetersPerSecond, Section);
	}

	/**
	 * The section's answer becomes the channel's.
	 *
	 * Width, depth and speed arrive as the hydraulic geometry's guess and leave as
	 * what fits the ground here, so everything that already reads a node — the
	 * field, the terrain carve, the water mesh, the masks — picks the section up
	 * without being taught about it.
	 */
	void AdoptProfile(FMassifRiverNode& InOutNode)
	{
		InOutNode.WidthMeters = InOutNode.Profile.WettedWidthMeters;
		InOutNode.DepthMeters = InOutNode.Profile.DepthMeters;
		InOutNode.SpeedMetersPerSecond = InOutNode.Profile.SpeedMetersPerSecond;
		InOutNode.BedAltitudeMeters = InOutNode.WaterSurfaceMeters - InOutNode.Profile.DepthMeters;
	}

	void SolveNodeProfile(
		const IMassifElevationSource& Elevation, const FMassifChannelSolveParams& Params,
		FVector2d FlowDirection, double EnergySlope, FMassifRiverNode& InOutNode)
	{
		FMassifChannelProfile& Profile = InOutNode.Profile;
		Profile.HalfSpanMeters = ProfileHalfSpanMeters(Params, InOutNode);
		Profile.EnergySlope = EnergySlope;
		SampleSection(Elevation, InOutNode, FlowDirection, Profile);

		const double SurfaceMeters = InOutNode.WaterSurfaceMeters;
		FTrenchBand Band = FindTrenchBand(Profile, SurfaceMeters);
		if (!Band.IsValid())
		{
			FallBackToHydraulicGeometry(Params, InOutNode, Profile);
			AdoptProfile(InOutNode);
			return;
		}
		ClampBandWidth(Band, Profile, Params, InOutNode.WidthMeters);

		const double TrenchDepthMeters = SolveTrenchDepthMeters(
			Profile, Band, Params, SurfaceMeters, InOutNode.DischargeCubicMetersPerSecond,
			Params.MaxDepthFactor * InOutNode.DepthMeters);
		CutTrench(Profile, Band, TrenchDepthMeters);

		const FMassifSectionFlow Section = MassifChannelHydraulics::MeasureSection(Profile, SurfaceMeters);
		Profile.WettedWidthMeters = Section.TopWidthMeters;
		Profile.FlowAreaSquareMeters = Section.AreaSquareMeters;
		Profile.DepthMeters = FMath::Max(Section.MaxDepthMeters, Params.MinDepthMeters);
		Profile.SpeedMetersPerSecond = MassifChannelHydraulics::MeanSpeedMetersPerSecond(
			InOutNode.DischargeCubicMetersPerSecond, Section.AreaSquareMeters);
		Profile.FroudeNumber = MassifChannelHydraulics::FroudeNumber(Profile.SpeedMetersPerSecond, Section);
		AdoptProfile(InOutNode);
	}

	// -----------------------------------------------------------------------
	// The slope the depth is solved against
	// -----------------------------------------------------------------------

	double AlongDistanceMeters(const TArray<FMassifRiverNode>& Nodes, int32 FirstIndex, int32 LastIndex)
	{
		double DistanceMeters = 0.0;
		for (int32 NodeIndex = FirstIndex; NodeIndex < LastIndex; ++NodeIndex)
		{
			DistanceMeters += (Nodes[NodeIndex + 1].PositionMeters - Nodes[NodeIndex].PositionMeters).Length();
		}
		return DistanceMeters;
	}

	double EnergySlopeAt(
		const TArray<FMassifRiverNode>& Nodes, int32 NodeIndex, const FMassifChannelSolveParams& Params)
	{
		const int32 FirstIndex = FMath::Max(NodeIndex - Params.SlopeWindowNodes, 0);
		const int32 LastIndex = FMath::Min(NodeIndex + Params.SlopeWindowNodes, Nodes.Num() - 1);
		const double DropMeters = Nodes[FirstIndex].WaterSurfaceMeters - Nodes[LastIndex].WaterSurfaceMeters;
		const double RunMeters = AlongDistanceMeters(Nodes, FirstIndex, LastIndex);
		if (RunMeters <= UE_DOUBLE_SMALL_NUMBER)
		{
			return Params.MinEnergySlope;
		}
		return FMath::Max(DropMeters / RunMeters, Params.MinEnergySlope);
	}

	void SolveReachProfiles(
		const IMassifElevationSource& Elevation, const FMassifChannelSolveParams& Params,
		FMassifRiverReach& InOutReach)
	{
		for (int32 NodeIndex = 0; NodeIndex < InOutReach.Nodes.Num(); ++NodeIndex)
		{
			SolveNodeProfile(
				Elevation, Params, FlowDirectionAt(InOutReach.Nodes, NodeIndex),
				EnergySlopeAt(InOutReach.Nodes, NodeIndex, Params), InOutReach.Nodes[NodeIndex]);
		}
	}
}

void MassifSolveChannelProfiles(
	const IMassifElevationSource& Elevation,
	const FMassifChannelSolveParams& Params,
	TArray<FMassifRiverReach>& InOutReaches)
{
	ParallelFor(InOutReaches.Num(), [&Elevation, &Params, &InOutReaches](int32 ReachIndex)
	{
		SolveReachProfiles(Elevation, Params, InOutReaches[ReachIndex]);
	});
}
