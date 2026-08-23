#include "MassifChannelHydraulics.h"

namespace
{
	/** What one interval between neighbouring offsets contributes to the section. */
	struct FIntervalMeasure
	{
		double AreaSquareMeters = 0.0;
		double PerimeterMeters = 0.0;
		double TopWidthMeters = 0.0;
	};

	FIntervalMeasure MeasureInterval(
		double LeftDepthMeters, double RightDepthMeters, double SpacingMeters, double BedRiseMeters)
	{
		FIntervalMeasure Measure;
		if (LeftDepthMeters <= 0.0 && RightDepthMeters <= 0.0)
		{
			return Measure;
		}
		if (LeftDepthMeters > 0.0 && RightDepthMeters > 0.0)
		{
			Measure.AreaSquareMeters = 0.5 * (LeftDepthMeters + RightDepthMeters) * SpacingMeters;
			Measure.PerimeterMeters = FMath::Sqrt(SpacingMeters * SpacingMeters + BedRiseMeters * BedRiseMeters);
			Measure.TopWidthMeters = SpacingMeters;
			return Measure;
		}

		// One end dry: the water's edge is where the bed crosses the surface, and
		// over that stretch the bed rises by exactly the depth at the wet end.
		const double WetDepthMeters = FMath::Max(LeftDepthMeters, RightDepthMeters);
		const double DryDepthMeters = FMath::Min(LeftDepthMeters, RightDepthMeters);
		const double WetSpanMeters = SpacingMeters * WetDepthMeters / (WetDepthMeters - DryDepthMeters);

		Measure.AreaSquareMeters = 0.5 * WetDepthMeters * WetSpanMeters;
		Measure.PerimeterMeters = FMath::Sqrt(WetSpanMeters * WetSpanMeters + WetDepthMeters * WetDepthMeters);
		Measure.TopWidthMeters = WetSpanMeters;
		return Measure;
	}

	/** Whether the water can reach one offset further out on this side of the run. */
	bool CanWiden(
		const FMassifChannelProfile& Profile, double SurfaceAltitudeMeters, int32 EdgeOffset, int32 Step)
	{
		const int32 NextOffset = EdgeOffset + Step;
		if (NextOffset < 0 || NextOffset >= MassifChannelProfileOffsetCount)
		{
			return false;
		}
		return Profile.BedAltitudeMeters[NextOffset] <= SurfaceAltitudeMeters;
	}

	/** Takes in the next offset on whichever side the water reaches first: the lower one. */
	void WidenRun(
		const FMassifChannelProfile& Profile, FMassifSectionFlow& InOutSection, bool bCanWidenFirst,
		bool bCanWidenLast)
	{
		if (!bCanWidenLast)
		{
			--InOutSection.FirstWetOffset;
			return;
		}
		if (!bCanWidenFirst)
		{
			++InOutSection.LastWetOffset;
			return;
		}
		if (Profile.BedAltitudeMeters[InOutSection.FirstWetOffset - 1]
			<= Profile.BedAltitudeMeters[InOutSection.LastWetOffset + 1])
		{
			--InOutSection.FirstWetOffset;
			return;
		}
		++InOutSection.LastWetOffset;
	}

	/**
	 * Grows the run out from the thalweg for as long as the bed stays under the
	 * water and the water is allowed any further across.
	 *
	 * One offset at a time to the lower side rather than each end run out in turn,
	 * so that where the width guard stops it the water has reached the ground it
	 * would really have reached first.
	 */
	void FindWetRun(
		const FMassifChannelProfile& Profile, double SurfaceAltitudeMeters, FMassifSectionFlow& OutSection)
	{
		const int32 ThalwegOffset = MassifChannelHydraulics::FindThalwegOffset(Profile);
		if (Profile.BedAltitudeMeters[ThalwegOffset] > SurfaceAltitudeMeters)
		{
			return;
		}

		OutSection.FirstWetOffset = ThalwegOffset;
		OutSection.LastWetOffset = ThalwegOffset;
		OutSection.MaxDepthMeters = SurfaceAltitudeMeters - Profile.BedAltitudeMeters[ThalwegOffset];

		const double SpacingMeters = Profile.OffsetSpacingMeters();
		const int32 MaxOffsetCount = FMath::Clamp(
			FMath::FloorToInt32(Profile.MaxWettedWidthMeters / SpacingMeters) + 1,
			2, MassifChannelProfileOffsetCount);
		while (OutSection.LastWetOffset - OutSection.FirstWetOffset + 1 < MaxOffsetCount)
		{
			const bool bCanWidenFirst = CanWiden(Profile, SurfaceAltitudeMeters, OutSection.FirstWetOffset, -1);
			const bool bCanWidenLast = CanWiden(Profile, SurfaceAltitudeMeters, OutSection.LastWetOffset, 1);
			if (!bCanWidenFirst && !bCanWidenLast)
			{
				return;
			}
			WidenRun(Profile, OutSection, bCanWidenFirst, bCanWidenLast);
		}
	}

	/**
	 * Lower, or level with it and nearer the centreline.
	 *
	 * The tie-break is the point. A section across level ground — a valley floor, or
	 * the flat ribbon a laser scan returns over water — is level at every offset, and
	 * taking the first of those puts the channel against the edge of its own profile
	 * rather than under the line it was traced along.
	 */
	bool IsLower(const float* AltitudeMeters, int32 CandidateOffset, int32 CurrentOffset)
	{
		if (AltitudeMeters[CandidateOffset] < AltitudeMeters[CurrentOffset])
		{
			return true;
		}
		if (AltitudeMeters[CandidateOffset] > AltitudeMeters[CurrentOffset])
		{
			return false;
		}
		return FMath::Abs(CandidateOffset - MassifChannelProfileMidOffset)
			< FMath::Abs(CurrentOffset - MassifChannelProfileMidOffset);
	}

	/** Highest bed between the thalweg and one end of the profile. */
	double CrestAltitudeMeters(const FMassifChannelProfile& Profile, int32 FromOffset, int32 ToOffset)
	{
		double CrestMeters = Profile.BedAltitudeMeters[FromOffset];
		const int32 Step = FMath::Sign(ToOffset - FromOffset);
		for (int32 OffsetIndex = FromOffset; OffsetIndex != ToOffset; OffsetIndex += Step)
		{
			CrestMeters = FMath::Max(CrestMeters, static_cast<double>(Profile.BedAltitudeMeters[OffsetIndex]));
		}
		return FMath::Max(CrestMeters, static_cast<double>(Profile.BedAltitudeMeters[ToOffset]));
	}
}

double FMassifSectionFlow::HydraulicRadiusMeters() const
{
	if (WettedPerimeterMeters <= UE_DOUBLE_SMALL_NUMBER)
	{
		return 0.0;
	}
	return AreaSquareMeters / WettedPerimeterMeters;
}

double FMassifSectionFlow::HydraulicDepthMeters() const
{
	if (TopWidthMeters <= UE_DOUBLE_SMALL_NUMBER)
	{
		return 0.0;
	}
	return AreaSquareMeters / TopWidthMeters;
}

void MassifChannelHydraulics::MeasureStationSpans(
	const TArray<FMassifRiverNode>& Nodes, TArray<double>& OutSpanMeters)
{
	OutSpanMeters.SetNumUninitialized(Nodes.Num());
	for (int32 NodeIndex = 1; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		OutSpanMeters[NodeIndex] =
			(Nodes[NodeIndex].PositionMeters - Nodes[NodeIndex - 1].PositionMeters).Length();
	}
	if (OutSpanMeters.Num() > 1)
	{
		OutSpanMeters[0] = OutSpanMeters[1];
		return;
	}
	OutSpanMeters[0] = 1.0;
}

int32 MassifChannelHydraulics::FindThalwegOffset(const FMassifChannelProfile& Profile)
{
	return MassifChannelHydraulics::FindLowestOffset(Profile.BedAltitudeMeters);
}

int32 MassifChannelHydraulics::FindLowestOffset(const float* AltitudeMeters)
{
	int32 LowestOffset = MassifChannelProfileMidOffset;
	for (int32 OffsetIndex = 0; OffsetIndex < MassifChannelProfileOffsetCount; ++OffsetIndex)
	{
		if (!IsLower(AltitudeMeters, OffsetIndex, LowestOffset))
		{
			continue;
		}
		LowestOffset = OffsetIndex;
	}
	return LowestOffset;
}

FMassifSectionFlow MassifChannelHydraulics::MeasureSection(
	const FMassifChannelProfile& Profile, double SurfaceAltitudeMeters)
{
	FMassifSectionFlow Section;
	FindWetRun(Profile, SurfaceAltitudeMeters, Section);
	if (Section.FirstWetOffset == INDEX_NONE)
	{
		return Section;
	}

	const double SpacingMeters = Profile.OffsetSpacingMeters();
	for (int32 OffsetIndex = Section.FirstWetOffset; OffsetIndex < Section.LastWetOffset; ++OffsetIndex)
	{
		const double LeftBedMeters = Profile.BedAltitudeMeters[OffsetIndex];
		const double RightBedMeters = Profile.BedAltitudeMeters[OffsetIndex + 1];
		const FIntervalMeasure Interval = MeasureInterval(
			SurfaceAltitudeMeters - LeftBedMeters, SurfaceAltitudeMeters - RightBedMeters, SpacingMeters,
			RightBedMeters - LeftBedMeters);

		Section.AreaSquareMeters += Interval.AreaSquareMeters;
		Section.WettedPerimeterMeters += Interval.PerimeterMeters;
		Section.TopWidthMeters += Interval.TopWidthMeters;
	}
	return Section;
}

double MassifChannelHydraulics::ManningDischarge(
	const FMassifSectionFlow& Section, double Roughness, double EnergySlope)
{
	if (!Section.IsWet() || Roughness <= UE_DOUBLE_SMALL_NUMBER || EnergySlope <= 0.0)
	{
		return 0.0;
	}
	return Section.AreaSquareMeters * FMath::Pow(Section.HydraulicRadiusMeters(), 2.0 / 3.0)
		* FMath::Sqrt(EnergySlope) / Roughness;
}

double MassifChannelHydraulics::SolveSurfaceAltitudeMeters(
	const FMassifChannelProfile& Profile, double Roughness, double EnergySlope,
	double DischargeCubicMetersPerSecond, double MinDepthMeters, int32 IterationCount)
{
	const int32 ThalwegOffset = FindThalwegOffset(Profile);
	double LowMeters = Profile.BedAltitudeMeters[ThalwegOffset];
	double HighMeters = SurfaceCeilingMeters(Profile, MinDepthMeters);
	for (int32 Iteration = 0; Iteration < IterationCount; ++Iteration)
	{
		const double MiddleMeters = 0.5 * (LowMeters + HighMeters);
		const FMassifSectionFlow Section = MeasureSection(Profile, MiddleMeters);
		if (ManningDischarge(Section, Roughness, EnergySlope) < DischargeCubicMetersPerSecond)
		{
			LowMeters = MiddleMeters;
			continue;
		}
		HighMeters = MiddleMeters;
	}
	return 0.5 * (LowMeters + HighMeters);
}

double MassifChannelHydraulics::BankCrestAltitudeMeters(const FMassifChannelProfile& Profile)
{
	const int32 ThalwegOffset = FindThalwegOffset(Profile);
	const double LeftCrestMeters = CrestAltitudeMeters(Profile, ThalwegOffset, 0);
	const double RightCrestMeters =
		CrestAltitudeMeters(Profile, ThalwegOffset, MassifChannelProfileOffsetCount - 1);
	return FMath::Min(LeftCrestMeters, RightCrestMeters);
}

double MassifChannelHydraulics::SurfaceCeilingMeters(
	const FMassifChannelProfile& Profile, double MinDepthMeters)
{
	const double ThalwegMeters = Profile.BedAltitudeMeters[FindThalwegOffset(Profile)];
	return FMath::Max(BankCrestAltitudeMeters(Profile), ThalwegMeters + MinDepthMeters);
}

double MassifChannelHydraulics::MeanSpeedMetersPerSecond(
	double DischargeCubicMetersPerSecond, double AreaSquareMeters)
{
	if (AreaSquareMeters <= UE_DOUBLE_SMALL_NUMBER)
	{
		return 0.0;
	}
	return DischargeCubicMetersPerSecond / AreaSquareMeters;
}

double MassifChannelHydraulics::FroudeNumber(double SpeedMetersPerSecond, const FMassifSectionFlow& Section)
{
	const double HydraulicDepthMeters = Section.HydraulicDepthMeters();
	if (HydraulicDepthMeters <= UE_DOUBLE_SMALL_NUMBER)
	{
		return 0.0;
	}
	return SpeedMetersPerSecond / FMath::Sqrt(GravityMetersPerSecondSquared * HydraulicDepthMeters);
}

double MassifChannelHydraulics::ShearStressPascals(double DepthMeters, double EnergySlope)
{
	if (DepthMeters <= 0.0 || EnergySlope <= 0.0)
	{
		return 0.0;
	}
	return WaterDensityKgPerCubicMeter * GravityMetersPerSecondSquared * DepthMeters * EnergySlope;
}

void MassifChannelHydraulics::ApplySectionToNode(FMassifRiverNode& InOutNode, double MinDepthMeters)
{
	FMassifChannelProfile& Profile = InOutNode.Profile;
	const FMassifSectionFlow Section = MeasureSection(Profile, InOutNode.WaterSurfaceMeters);

	Profile.WettedWidthMeters = Section.TopWidthMeters;
	Profile.FlowAreaSquareMeters = Section.AreaSquareMeters;
	Profile.DepthMeters = FMath::Max(Section.MaxDepthMeters, MinDepthMeters);
	Profile.SpeedMetersPerSecond =
		MeanSpeedMetersPerSecond(InOutNode.DischargeCubicMetersPerSecond, Section.AreaSquareMeters);
	Profile.FroudeNumber = FroudeNumber(Profile.SpeedMetersPerSecond, Section);

	InOutNode.WidthMeters = Profile.WettedWidthMeters;
	InOutNode.DepthMeters = Profile.DepthMeters;
	InOutNode.SpeedMetersPerSecond = Profile.SpeedMetersPerSecond;
	InOutNode.BedAltitudeMeters = InOutNode.WaterSurfaceMeters - Profile.DepthMeters;

	// The ground the node reports becomes the one its own section was cut from,
	// which is the surface the terrain is shaped by. It arrived as the hydrology
	// grid's cell average, and a bed under a channel five metres wide is not
	// comparable with the average of the eight-metre cell it sits in.
	InOutNode.GroundAltitudeMeters = Profile.GroundAltitudeMeters[MassifChannelProfileOffsetCount / 2];
}
