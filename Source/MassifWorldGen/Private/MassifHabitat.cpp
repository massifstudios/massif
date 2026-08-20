#include "MassifHabitat.h"

#include "MassifSeed.h"
#include "MassifTerrainField.h"

namespace
{
	namespace Climate
	{
		/** Growing-season mean at sea level, falling with altitude: the treeline lands near 2000 m. */
		constexpr double SeaLevelTemperatureCelsius = 19.0;
		constexpr double LapseRateCelsiusPerMeter = 0.0065;

		/** Warmth gained on ground facing full south and lost facing full north, scaled by steepness. */
		constexpr double SolarAspectGainCelsius = 2.5;

		constexpr double BaseHumidity = 0.55;

		/** Orographic lift: air pushed higher over the massif drops more of its water. */
		constexpr double HumidityPerMeterOfLift = 0.00014;
		constexpr double LiftReferenceAltitudeMeters = 1200.0;

		/** Ground facing into the prevailing wind takes the rain; the lee side stays dry. */
		constexpr double WindwardHumidityGain = 0.18;

		/** Water gathers in hollows, runs off steep ground, and evaporates where it is hot. */
		constexpr double ConvergenceHumidityGain = 0.35;
		constexpr double DrainageHumidityPerSlopeDegree = 0.005;
		constexpr double EvaporationHumidityPerCelsius = 0.02;
		constexpr double EvaporationTemperatureCelsius = 8.0;

		/** Relief across the convergence radius that reads as a full hollow or a full ridge. */
		constexpr double ConvergenceRadiusMeters = 45.0;
		constexpr double ConvergenceReliefMeters = 12.0;

		/** Wet and dry regions of the massif, as two octaves of seeded noise. */
		constexpr double RegionalHumidityAmplitude = 0.12;
		constexpr double RegionalWeatherCellMeters = 3000.0;
		constexpr double LocalWeatherCellMeters = 900.0;
		constexpr double LocalWeatherWeight = 0.3;

		/** Ground too steep to hold anything, and the looser debris slope below it. */
		constexpr double RockSlopeDegrees = 45.0;
		constexpr double ScreeSlopeDegrees = 34.0;

		/** Above the cold end of the forest, steep ground is loose debris rather than soil. */
		constexpr double ScreeTemperatureCelsius = 9.0;

		constexpr double PermanentSnowTemperatureCelsius = 0.0;
		constexpr double WetlandHumidity = 0.75;
		constexpr double WetlandSlopeDegrees = 6.0;
	}

	double LatticeValue(uint32 Seed, int32 CellX, int32 CellY)
	{
		const uint32 Hash = HashCombine(HashCombine(Seed, GetTypeHash(CellX)), GetTypeHash(CellY));
		return Hash / static_cast<double>(TNumericLimits<uint32>::Max());
	}

	double SmoothFraction(double Fraction)
	{
		return Fraction * Fraction * (3.0 - 2.0 * Fraction);
	}

	/** Smooth 0..1 value noise on a unit lattice, hashed rather than tabulated so it is seedable. */
	double ValueNoise(uint32 Seed, FVector2d LatticePosition)
	{
		const int32 CellX = FMath::FloorToInt32(LatticePosition.X);
		const int32 CellY = FMath::FloorToInt32(LatticePosition.Y);
		const double FractionX = SmoothFraction(LatticePosition.X - CellX);
		const double FractionY = SmoothFraction(LatticePosition.Y - CellY);

		const double SouthEdge = FMath::Lerp(
			LatticeValue(Seed, CellX, CellY), LatticeValue(Seed, CellX + 1, CellY), FractionX);
		const double NorthEdge = FMath::Lerp(
			LatticeValue(Seed, CellX, CellY + 1), LatticeValue(Seed, CellX + 1, CellY + 1), FractionX);
		return FMath::Lerp(SouthEdge, NorthEdge, FractionY);
	}

	double SlopeDegreesFromNormal(FVector3d Normal)
	{
		return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Normal.Z, -1.0, 1.0)));
	}

	/** Compass bearing of the direction the ground faces, from the normal's horizontal lean. */
	double AspectDegreesFromNormal(FVector3d Normal)
	{
		const double Bearing = FMath::RadiansToDegrees(FMath::Atan2(Normal.X, Normal.Y));
		return FRotator3d::ClampAxis(Bearing);
	}

	/**
	 * How much of the sun the ground takes: 1 where it leans full south, -1 full
	 * north, 0 where it is flat. The normal's south component already carries the
	 * steepness, so a gentle south slope gains less than a headwall does.
	 */
	double Southness(FVector3d Normal)
	{
		return -Normal.Y;
	}

	double TemperatureCelsius(double AltitudeMeters, FVector3d Normal)
	{
		return Climate::SeaLevelTemperatureCelsius
			- Climate::LapseRateCelsiusPerMeter * AltitudeMeters
			+ Climate::SolarAspectGainCelsius * Southness(Normal);
	}

	double HumidityFraction(
		double AltitudeMeters,
		double SlopeDegrees,
		double TemperatureCelsius,
		double Convergence,
		double Windwardness,
		double WeatherVariation)
	{
		const double Evaporation = FMath::Max(TemperatureCelsius - Climate::EvaporationTemperatureCelsius, 0.0);
		const double Humidity = Climate::BaseHumidity
			+ Climate::HumidityPerMeterOfLift * (AltitudeMeters - Climate::LiftReferenceAltitudeMeters)
			+ Climate::WindwardHumidityGain * Windwardness
			+ Climate::ConvergenceHumidityGain * Convergence
			- Climate::DrainageHumidityPerSlopeDegree * SlopeDegrees
			- Climate::EvaporationHumidityPerCelsius * Evaporation
			+ Climate::RegionalHumidityAmplitude * WeatherVariation;
		return FMath::Clamp(Humidity, 0.0, 1.0);
	}

	/**
	 * What the surface is made of. Order matters: a face steep enough to shed
	 * everything is rock however cold or wet it is, and only ground that snow can
	 * settle on stays under it year round.
	 */
	EMassifGroundType ClassifyGround(double SlopeDegrees, double TemperatureCelsius, double Humidity)
	{
		if (SlopeDegrees >= Climate::RockSlopeDegrees)
		{
			return EMassifGroundType::Rock;
		}
		if (TemperatureCelsius <= Climate::PermanentSnowTemperatureCelsius)
		{
			return EMassifGroundType::PermanentSnow;
		}
		if (SlopeDegrees >= Climate::ScreeSlopeDegrees && TemperatureCelsius <= Climate::ScreeTemperatureCelsius)
		{
			return EMassifGroundType::Scree;
		}
		if (Humidity >= Climate::WetlandHumidity && SlopeDegrees <= Climate::WetlandSlopeDegrees)
		{
			return EMassifGroundType::Wetland;
		}
		return EMassifGroundType::Soil;
	}
}

FMassifHabitatField::FMassifHabitatField(TSharedRef<const FMassifTerrainField> InTerrainField, uint32 InMasterSeed)
	: TerrainField(MoveTemp(InTerrainField))
	, WeatherSeed(FMassifSeed(InMasterSeed).MakeChannelSeed(TEXT("Habitat.Weather")))
{
	const FRandomStream WindStream = FMassifSeed(InMasterSeed).MakeChannelStream(TEXT("Habitat.PrevailingWind"));
	const double WindBearingRadians = WindStream.FRand() * UE_DOUBLE_TWO_PI;
	PrevailingWindDirection = FVector2d(FMath::Sin(WindBearingRadians), FMath::Cos(WindBearingRadians));
}

FMassifHabitatSample FMassifHabitatField::Sample(FVector2d PositionMeters, double SampleSpacingMeters) const
{
	const FVector3d Normal = TerrainField->SampleNormal(PositionMeters, SampleSpacingMeters);

	FMassifHabitatSample Habitat;
	Habitat.AltitudeMeters = TerrainField->SampleHeightMeters(PositionMeters, SampleSpacingMeters);
	Habitat.SlopeDegrees = SlopeDegreesFromNormal(Normal);
	Habitat.AspectDegrees = AspectDegreesFromNormal(Normal);
	Habitat.TemperatureCelsius = TemperatureCelsius(Habitat.AltitudeMeters, Normal);

	const double Windwardness = -FVector2d(Normal.X, Normal.Y).Dot(PrevailingWindDirection);
	Habitat.Humidity = HumidityFraction(
		Habitat.AltitudeMeters,
		Habitat.SlopeDegrees,
		Habitat.TemperatureCelsius,
		SampleConvergence(PositionMeters),
		Windwardness,
		SampleWeatherVariation(PositionMeters));

	Habitat.GroundType = ClassifyGround(Habitat.SlopeDegrees, Habitat.TemperatureCelsius, Habitat.Humidity);
	return Habitat;
}

double FMassifHabitatField::SampleConvergence(FVector2d PositionMeters) const
{
	// Centre and ring are read at the ring's own spacing, so this measures the
	// shape of the hollow rather than the bumps inside it.
	const double Radius = Climate::ConvergenceRadiusMeters;
	const double Centre = TerrainField->SampleHeightMeters(PositionMeters, Radius);
	const double West = TerrainField->SampleHeightMeters(PositionMeters - FVector2d(Radius, 0.0), Radius);
	const double East = TerrainField->SampleHeightMeters(PositionMeters + FVector2d(Radius, 0.0), Radius);
	const double South = TerrainField->SampleHeightMeters(PositionMeters - FVector2d(0.0, Radius), Radius);
	const double North = TerrainField->SampleHeightMeters(PositionMeters + FVector2d(0.0, Radius), Radius);

	const double RingAverage = 0.25 * (West + East + South + North);
	return FMath::Clamp((RingAverage - Centre) / Climate::ConvergenceReliefMeters, -1.0, 1.0);
}

double FMassifHabitatField::SampleWeatherVariation(FVector2d PositionMeters) const
{
	const double Regional = ValueNoise(WeatherSeed, PositionMeters / Climate::RegionalWeatherCellMeters);
	const double Local = ValueNoise(WeatherSeed + 1, PositionMeters / Climate::LocalWeatherCellMeters);
	const double Combined = FMath::Lerp(Regional, Local, Climate::LocalWeatherWeight);
	return 2.0 * Combined - 1.0;
}
