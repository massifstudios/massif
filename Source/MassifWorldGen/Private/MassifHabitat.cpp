#include "MassifHabitat.h"

#include "MassifEnvironmentBake.h"
#include "MassifSeed.h"
#include "MassifTerrainField.h"
#include "MassifValueNoise.h"

namespace
{
	namespace Climate
	{
		/** Growing-season mean at sea level, falling with altitude: the treeline lands near 2000 m. */
		constexpr double SeaLevelTemperatureCelsius = 19.0;
		constexpr double LapseRateCelsiusPerMeter = 0.0065;

		/** Warmth gained on ground facing full south and lost facing full north, scaled by steepness. */
		constexpr double SolarAspectGainCelsius = 2.5;

		/** Damp a point has before any of the ground water the bake routed to it. */
		constexpr double BaseHumidity = 0.2;

		/** How much of the humidity range the bake's wetness accounts for. */
		constexpr double GroundWaterHumidityWeight = 0.7;

		/** Orographic lift: air pushed higher over the massif drops more of its water. */
		constexpr double HumidityPerMeterOfLift = 0.00014;
		constexpr double LiftReferenceAltitudeMeters = 1200.0;

		/** Ground facing into the prevailing wind takes the rain; the lee side stays dry. */
		constexpr double WindwardHumidityGain = 0.18;

		/** Water leaves the ground where it is hot, whatever drained into it. */
		constexpr double EvaporationHumidityPerCelsius = 0.02;
		constexpr double EvaporationTemperatureCelsius = 8.0;

		/** Wet and dry regions of the massif, as two octaves of seeded noise. */
		constexpr double RegionalHumidityAmplitude = 0.12;
		constexpr double RegionalWeatherCellMeters = 3000.0;
		constexpr double LocalWeatherCellMeters = 900.0;
		constexpr double LocalWeatherWeight = 0.3;

		/** Ground too steep to hold anything, and the looser debris slope below it. */
		constexpr double RockSlopeDegrees = 45.0;
		constexpr double ScreeSlopeDegrees = 34.0;

		/**
		 * Soil this thin is bare rock, and steep ground under the second figure is
		 * loose debris. Both sit against the regolith the bake leaves on ground
		 * nothing has worked at: a slope at the angle of repose keeps most of its
		 * cover but not all of it, and a face keeps none.
		 */
		constexpr double BareRockSoilDepthMeters = 0.08;
		constexpr double LooseDebrisSoilDepthMeters = 0.32;

		/** Water standing this deep makes the ground it covers wetland whatever else it is. */
		constexpr double PondedWaterMeters = 0.1;

		constexpr double PermanentSnowTemperatureCelsius = 0.0;
		constexpr double WetlandHumidity = 0.8;
		constexpr double WetlandSlopeDegrees = 6.0;
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

	/**
	 * The bake's wetness is the ground water: everything draining through the
	 * point from the catchment above it, which is what makes a valley floor damp
	 * and a shoulder at the same altitude dry. Climate moves that figure rather
	 * than replacing it — lift and windward exposure add rain, heat takes it back.
	 */
	double HumidityFraction(
		double GroundWetness,
		double AltitudeMeters,
		double TemperatureCelsius,
		double Windwardness,
		double WeatherVariation)
	{
		const double Evaporation = FMath::Max(TemperatureCelsius - Climate::EvaporationTemperatureCelsius, 0.0);
		const double Humidity = Climate::BaseHumidity
			+ Climate::GroundWaterHumidityWeight * GroundWetness
			+ Climate::HumidityPerMeterOfLift * (AltitudeMeters - Climate::LiftReferenceAltitudeMeters)
			+ Climate::WindwardHumidityGain * Windwardness
			- Climate::EvaporationHumidityPerCelsius * Evaporation
			+ Climate::RegionalHumidityAmplitude * WeatherVariation;
		return FMath::Clamp(Humidity, 0.0, 1.0);
	}

	/**
	 * What the surface is made of. Order matters: a face steep enough to shed
	 * everything is rock however deep the soil elsewhere, only ground that snow
	 * can settle on stays under it year round, and past those two it is the soil
	 * the water left that decides — bare rock, loose debris, or ground a root
	 * can hold in.
	 */
	EMassifGroundType ClassifyGround(const FMassifHabitatSample& Habitat)
	{
		if (Habitat.SlopeDegrees >= Climate::RockSlopeDegrees)
		{
			return EMassifGroundType::Rock;
		}
		if (Habitat.TemperatureCelsius <= Climate::PermanentSnowTemperatureCelsius)
		{
			return EMassifGroundType::PermanentSnow;
		}
		if (Habitat.StandingWaterDepthMeters >= Climate::PondedWaterMeters)
		{
			return EMassifGroundType::Wetland;
		}
		if (Habitat.SoilDepthMeters <= Climate::BareRockSoilDepthMeters)
		{
			return EMassifGroundType::Rock;
		}
		if (Habitat.SoilDepthMeters <= Climate::LooseDebrisSoilDepthMeters
			&& Habitat.SlopeDegrees >= Climate::ScreeSlopeDegrees)
		{
			return EMassifGroundType::Scree;
		}
		if (Habitat.Humidity >= Climate::WetlandHumidity && Habitat.SlopeDegrees <= Climate::WetlandSlopeDegrees)
		{
			return EMassifGroundType::Wetland;
		}
		return EMassifGroundType::Soil;
	}
}

FMassifHabitatField::FMassifHabitatField(
	TSharedRef<const FMassifTerrainField> InTerrainField,
	TSharedRef<const FMassifEnvironmentBake> InEnvironmentBake,
	uint32 InMasterSeed)
	: TerrainField(MoveTemp(InTerrainField))
	, EnvironmentBake(MoveTemp(InEnvironmentBake))
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
	Habitat.SoilDepthMeters = EnvironmentBake->Sample(EMassifBakeChannel::SoilDepth, PositionMeters);
	Habitat.StandingWaterDepthMeters =
		EnvironmentBake->Sample(EMassifBakeChannel::StandingWaterDepth, PositionMeters);

	const double Windwardness = -FVector2d(Normal.X, Normal.Y).Dot(PrevailingWindDirection);
	Habitat.Humidity = HumidityFraction(
		EnvironmentBake->Sample(EMassifBakeChannel::Wetness, PositionMeters),
		Habitat.AltitudeMeters,
		Habitat.TemperatureCelsius,
		Windwardness,
		SampleWeatherVariation(PositionMeters));

	Habitat.GroundType = ClassifyGround(Habitat);
	return Habitat;
}

double FMassifHabitatField::SampleWeatherVariation(FVector2d PositionMeters) const
{
	const double Regional =
		MassifNoise::ValueNoise(WeatherSeed, PositionMeters / Climate::RegionalWeatherCellMeters);
	const double Local =
		MassifNoise::ValueNoise(WeatherSeed + 1, PositionMeters / Climate::LocalWeatherCellMeters);
	const double Combined = FMath::Lerp(Regional, Local, Climate::LocalWeatherWeight);
	return 2.0 * Combined - 1.0;
}
