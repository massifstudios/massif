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

	/** Grows the run out from the thalweg for as long as the bed stays under the water. */
	void FindWetRun(
		const FMassifChannelProfile& Profile, double SurfaceAltitudeMeters, FMassifSectionFlow& OutSection)
	{
		const int32 ThalwegOffset = MassifChannelHydraulics::FindThalwegOffset(Profile);
		if (Profile.BedAltitudeMeters[ThalwegOffset] > SurfaceAltitudeMeters)
		{
			return;
		}

		OutSection.FirstWetOffset = ThalwegOffset;
		while (OutSection.FirstWetOffset > 0
			&& Profile.BedAltitudeMeters[OutSection.FirstWetOffset - 1] <= SurfaceAltitudeMeters)
		{
			--OutSection.FirstWetOffset;
		}

		OutSection.LastWetOffset = ThalwegOffset;
		while (OutSection.LastWetOffset < MassifChannelProfileOffsetCount - 1
			&& Profile.BedAltitudeMeters[OutSection.LastWetOffset + 1] <= SurfaceAltitudeMeters)
		{
			++OutSection.LastWetOffset;
		}

		OutSection.MaxDepthMeters = SurfaceAltitudeMeters - Profile.BedAltitudeMeters[ThalwegOffset];
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

int32 MassifChannelHydraulics::FindThalwegOffset(const FMassifChannelProfile& Profile)
{
	int32 ThalwegOffset = 0;
	for (int32 OffsetIndex = 1; OffsetIndex < MassifChannelProfileOffsetCount; ++OffsetIndex)
	{
		if (Profile.BedAltitudeMeters[OffsetIndex] < Profile.BedAltitudeMeters[ThalwegOffset])
		{
			ThalwegOffset = OffsetIndex;
		}
	}
	return ThalwegOffset;
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
	double DischargeCubicMetersPerSecond, double CeilingAltitudeMeters, int32 IterationCount)
{
	const int32 ThalwegOffset = FindThalwegOffset(Profile);
	double LowMeters = Profile.BedAltitudeMeters[ThalwegOffset];
	double HighMeters = FMath::Max(CeilingAltitudeMeters, LowMeters);
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
