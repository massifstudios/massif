#include "MassifFogField.h"

#include "MassifEnvironmentGraph.h"
#include "MassifSunParams.h"
#include "MassifSunProcesses.h"
#include "MassifWindProcesses.h"

namespace
{
	double Saturate(double Value)
	{
		return FMath::Clamp(Value, 0.0, 1.0);
	}

	/** Where in the day the sun rises and sets, as fractions of it. */
	struct FDaylightBounds
	{
		double SunriseFraction = 0.25;
		double SunsetFraction = 0.75;

		double GetNightHours() const
		{
			return (1.0 - (SunsetFraction - SunriseFraction)) * 24.0;
		}
	};

	FDaylightBounds SolveDaylightBounds(double LatitudeDegrees, double DayOfYear)
	{
		const double SunsetHourAngle = MassifSunsetHourAngleRadians(
			FMath::DegreesToRadians(LatitudeDegrees), MassifSolarDeclinationRadians(DayOfYear));
		const double HalfDay = SunsetHourAngle / UE_DOUBLE_TWO_PI;

		FDaylightBounds Bounds;
		Bounds.SunriseFraction = 0.5 - HalfDay;
		Bounds.SunsetFraction = 0.5 + HalfDay;
		return Bounds;
	}

	/** Hours of darkness elapsed at this point in the day, counting from the previous sunset. */
	double HoursSinceSunset(const FDaylightBounds& Bounds, double DayFraction)
	{
		if (DayFraction >= Bounds.SunsetFraction)
		{
			return (DayFraction - Bounds.SunsetFraction) * 24.0;
		}

		// Before sunrise the night started yesterday, so it wraps through midnight.
		return (1.0 - Bounds.SunsetFraction + DayFraction) * 24.0;
	}

	/**
	 * How much fog the night has made by this point in it.
	 *
	 * Rises through the darkness and falls back once the sun is up, and the two
	 * meet at sunrise, so the whole day is continuous without carrying any state
	 * across it.
	 */
	double SolveStrength(const FMassifFogParams& Params, const FDaylightBounds& Bounds, double DayFraction)
	{
		const double BuildHours = FMath::Max(Params.BuildHours, UE_DOUBLE_KINDA_SMALL_NUMBER);
		const bool bDark = DayFraction < Bounds.SunriseFraction || DayFraction >= Bounds.SunsetFraction;
		if (bDark)
		{
			return Saturate(HoursSinceSunset(Bounds, DayFraction) / BuildHours);
		}

		const double PeakAtSunrise = Saturate(Bounds.GetNightHours() / BuildHours);
		const double ClearHours = FMath::Max(Params.ClearHours, UE_DOUBLE_KINDA_SMALL_NUMBER);
		const double HoursSinceSunrise = (DayFraction - Bounds.SunriseFraction) * 24.0;
		return PeakAtSunrise * Saturate(1.0 - HoursSinceSunrise / ClearHours);
	}
}

FMassifFogConditions MassifSolveFogConditions(
	const FMassifFogParams& Params, double FloorAltitudeMeters, double LatitudeDegrees, double DayOfYear,
	double DayFraction)
{
	const FDaylightBounds Bounds = SolveDaylightBounds(LatitudeDegrees, DayOfYear);

	FMassifFogConditions Conditions;
	Conditions.Strength = SolveStrength(Params, Bounds, DayFraction);

	// The lid rises with the fog under it: a shallow layer early in the night fills
	// only the deepest ground, and by dawn it has reached the shoulders.
	Conditions.LidAltitudeMeters = FloorAltitudeMeters + Params.LidHeightMeters * Conditions.Strength;
	return Conditions;
}

FMassifFogField::FMassifFogField(
	TSharedRef<const FMassifEnvironmentState> InEnvironment, const FMassifFogParams& InParams)
	: Environment(InEnvironment)
	, Params(InParams)
	, AltitudeChannel(InEnvironment->FindChannel(MassifEnvironmentChannels::Altitude()))
	, WetnessChannel(InEnvironment->FindChannel(MassifEnvironmentChannels::Wetness()))
	, SkyViewChannel(InEnvironment->FindChannel(MassifEnvironmentChannels::SkyViewFactor()))
	, HorizonChannel(InEnvironment->FindChannel(MassifEnvironmentChannels::Horizon()))
	, WindExposureChannel(InEnvironment->FindChannel(MassifEnvironmentChannels::WindExposure()))
{
}

double FMassifFogField::GetAltitudeAt(FVector2d PositionMeters) const
{
	return Environment->SampleScalar(AltitudeChannel, PositionMeters);
}

double FMassifFogField::PotentialAt(FVector2d PositionMeters) const
{
	// The channel is already a saturation fraction rather than a raw index: the
	// bake smoothsteps the topographic wetness index between its own parched and
	// saturated readings before storing it.
	const double Moisture = Saturate(Environment->SampleScalar(WetnessChannel, PositionMeters));

	// An open sky is what lets the ground radiate its heat away, so the same view
	// factor that makes a place cold at night is what makes it foggy in the morning.
	const double SkyView = Saturate(Environment->SampleScalar(SkyViewChannel, PositionMeters));

	const double ScouringExposure = FMath::Max(Params.ScouringExposure, UE_DOUBLE_KINDA_SMALL_NUMBER);
	const double Exposure = FMath::Max(Environment->SampleScalar(WindExposureChannel, PositionMeters), 0.0);
	const double Shelter = Saturate(1.0 - Exposure / ScouringExposure);

	return Moisture * SkyView * Shelter;
}

bool FMassifFogField::IsSunlitAt(FVector2d PositionMeters, const FMassifSunPosition& Sun) const
{
	if (!Sun.IsAboveHorizon())
	{
		return false;
	}

	// Nearest stored bearing rather than a blend of two, which is how the bake's own
	// shading reads the same channel: a skyline is a ridge or it is not, and
	// averaging two of them would report one between them.
	const int32 BearingIndex =
		FMath::RoundToInt32(Sun.GetAzimuthRadians() / UE_DOUBLE_TWO_PI * MassifHorizonAzimuthCount)
		% MassifHorizonAzimuthCount;
	const double SkylineDegrees =
		Environment->SampleScalar(HorizonChannel, PositionMeters, BearingIndex);

	return FMath::RadiansToDegrees(Sun.AltitudeRadians) > SkylineDegrees;
}

double FMassifFogField::DensityAt(
	FVector2d PositionMeters, const FMassifFogConditions& Conditions, const FMassifSunPosition& Sun) const
{
	if (!Conditions.IsAnyFog())
	{
		return 0.0;
	}

	const double DepthBelowLid = Conditions.LidAltitudeMeters - GetAltitudeAt(PositionMeters);
	if (DepthBelowLid <= 0.0)
	{
		return 0.0;
	}

	const double FillDepth = FMath::Max(Params.FillDepthMeters, UE_DOUBLE_KINDA_SMALL_NUMBER);
	const double Fill = Saturate(DepthBelowLid / FillDepth);

	double BurnOff = 1.0;
	if (IsSunlitAt(PositionMeters, Sun))
	{
		BurnOff = 1.0 - Params.SunBurnFraction;
	}

	return Conditions.Strength * Fill * PotentialAt(PositionMeters) * BurnOff;
}
