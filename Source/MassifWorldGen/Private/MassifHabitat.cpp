#include "MassifHabitat.h"

#include "MassifClimateProcesses.h"
#include "MassifEnvironmentGraph.h"
#include "MassifPrecipitationProcesses.h"
#include "MassifEnvironmentState.h"
#include "MassifRiverField.h"
#include "MassifTerrainField.h"

namespace
{
	namespace Climate
	{
		/**
		 * Damp a point has before either the catchment above it or the rain on it.
		 *
		 * Small, because it is only what neither term accounts for. It used to stand
		 * in for the rainfall nothing modelled; now that rainfall is a term of its
		 * own, leaving the old figure in would put a floor under the dry end and
		 * there would be no parched ground anywhere.
		 */
		constexpr double BaseHumidity = 0.05;

		/**
		 * How much of the humidity range each source accounts for.
		 *
		 * They sum with the base to exactly 1, so saturated ground under a wet sky
		 * reaches the top of the range and parched ground under a dry one reaches the
		 * bottom. Anything less and the bog plants and the dry-scree layers both
		 * describe ground that never occurs.
		 */
		constexpr double GroundWaterHumidityWeight = 0.7;
		constexpr double RainfallHumidityWeight = 0.25;

		/** Annual rainfall that counts as parched, and as thoroughly wet. */
		constexpr double DryAnnualMillimetres = 600.0;
		constexpr double WetAnnualMillimetres = 2200.0;

		/** Water leaves the ground where it is hot, whatever drained into it. */
		constexpr double EvaporationHumidityPerCelsius = 0.02;
		constexpr double EvaporationTemperatureCelsius = 8.0;

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

		/**
		 * How far out from a channel the ground it drains keeps a water table near
		 * the surface: the gravel bars, the alder gallery, the willow scrub. Not
		 * scaled to the channel — a brook and a river both wet about a bank's width
		 * of ground, because what sets it is how far the water table spreads
		 * sideways rather than how much water is going past.
		 */
		constexpr double RiparianReachMeters = 4.0;

		/**
		 * Share of that reach the water table is at the surface across before it
		 * starts dropping away. A margin that peaked at the waterline and fell off
		 * immediately would be saturated nowhere a plant could stand, and the alder
		 * and willow that want it would never be placed at all.
		 */
		constexpr double RiparianCoreFraction = 0.45;

		/** How damp the ground immediately beside a channel is, whatever the catchment above says. */
		constexpr double RiparianHumidity = 0.95;

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

	/** 0 where a point takes a parched year's rain, 1 where it takes a thoroughly wet one. */
	double RainfallFraction(double AnnualMillimetres)
	{
		const double Span = Climate::WetAnnualMillimetres - Climate::DryAnnualMillimetres;
		return FMath::Clamp((AnnualMillimetres - Climate::DryAnnualMillimetres) / Span, 0.0, 1.0);
	}

	/**
	 * The bake's wetness is the ground water: everything draining through the
	 * point from the catchment above it, which is what makes a valley floor damp
	 * and a shoulder at the same altitude dry. What falls out of the sky is the
	 * other half of it, and heat takes some of both back.
	 */
	double HumidityFraction(
		double GroundWetness, double AnnualMillimetres, double TemperatureCelsius)
	{
		const double Evaporation = FMath::Max(TemperatureCelsius - Climate::EvaporationTemperatureCelsius, 0.0);
		const double Humidity = Climate::BaseHumidity
			+ Climate::GroundWaterHumidityWeight * GroundWetness
			+ Climate::RainfallHumidityWeight * RainfallFraction(AnnualMillimetres)
			- Climate::EvaporationHumidityPerCelsius * Evaporation;
		return FMath::Clamp(Humidity, 0.0, 1.0);
	}

	/** 1 across the wet margin beside a channel, falling to 0 at the far side of the bank. */
	double RiparianWeight(double ChannelDistanceMeters)
	{
		const double CoreMeters = Climate::RiparianCoreFraction * Climate::RiparianReachMeters;
		if (ChannelDistanceMeters <= CoreMeters)
		{
			return 1.0;
		}
		if (ChannelDistanceMeters >= Climate::RiparianReachMeters)
		{
			return 0.0;
		}

		const double Fraction =
			(ChannelDistanceMeters - CoreMeters) / (Climate::RiparianReachMeters - CoreMeters);
		return 1.0 - Fraction * Fraction * (3.0 - 2.0 * Fraction);
	}

	/**
	 * What the surface is made of. Order matters: a channel is a channel whatever
	 * the ground around it is doing, a face steep enough to shed everything is rock
	 * however deep the soil elsewhere, only ground that snow can settle on stays
	 * under it year round, and past those it is the soil the water left that
	 * decides — bare rock, loose debris, or ground a root can hold in.
	 */
	EMassifGroundType ClassifyGround(const FMassifHabitatSample& Habitat)
	{
		if (Habitat.ChannelDistanceMeters <= 0.0)
		{
			return EMassifGroundType::Water;
		}
		if (Habitat.SlopeDegrees >= Climate::RockSlopeDegrees)
		{
			return EMassifGroundType::Rock;
		}
		if (Habitat.TemperatureCelsius <= Climate::PermanentSnowTemperatureCelsius)
		{
			return EMassifGroundType::PermanentSnow;
		}
		// The bank the channel keeps wet: gravel the water reworks, and the alder
		// and willow that hold it.
		if (Habitat.ChannelDistanceMeters < Climate::RiparianReachMeters)
		{
			return EMassifGroundType::Wetland;
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
	TSharedRef<const FMassifEnvironmentState> InEnvironment,
	uint32 InMasterSeed)
	: TerrainField(MoveTemp(InTerrainField))
	, Environment(MoveTemp(InEnvironment))
{
	SoilDepthChannel = Environment->FindChannel(MassifEnvironmentChannels::SoilDepth());
	StandingWaterChannel = Environment->FindChannel(MassifEnvironmentChannels::StandingWaterDepth());
	WetnessChannel = Environment->FindChannel(MassifEnvironmentChannels::Wetness());
	TemperatureChannel = Environment->FindChannel(MassifEnvironmentChannels::MeanTemperature());
	PrecipitationChannel = Environment->FindChannel(MassifEnvironmentChannels::AnnualPrecipitation());
}

FMassifHabitatSample FMassifHabitatField::Sample(FVector2d PositionMeters, double SampleSpacingMeters) const
{
	const FVector3d Normal = TerrainField->SampleNormal(PositionMeters, SampleSpacingMeters);

	FMassifHabitatSample Habitat;
	Habitat.AltitudeMeters = TerrainField->SampleHeightMeters(PositionMeters, SampleSpacingMeters);
	Habitat.SlopeDegrees = SlopeDegreesFromNormal(Normal);
	Habitat.AspectDegrees = AspectDegreesFromNormal(Normal);
	Habitat.TemperatureCelsius = Environment->SampleScalar(TemperatureChannel, PositionMeters);
	Habitat.SoilDepthMeters = Environment->SampleScalar(SoilDepthChannel, PositionMeters);
	Habitat.StandingWaterDepthMeters = Environment->SampleScalar(StandingWaterChannel, PositionMeters);

	Habitat.Humidity = HumidityFraction(
		Environment->SampleScalar(WetnessChannel, PositionMeters),
		Environment->SampleScalar(PrecipitationChannel, PositionMeters),
		Habitat.TemperatureCelsius);

	Habitat.ChannelDistanceMeters = SampleChannelDistanceMeters(PositionMeters);
	// Ground beside running water is wet whatever the catchment above it says,
	// which is what puts the gallery of alder and willow along a channel rather
	// than leaving it to whatever the hillside behind is growing.
	Habitat.Humidity = FMath::Max(
		Habitat.Humidity,
		Climate::RiparianHumidity * RiparianWeight(Habitat.ChannelDistanceMeters));

	Habitat.GroundType = ClassifyGround(Habitat);
	return Habitat;
}

/**
 * The rivers the terrain was carved for, read back through the same field that
 * carved it. Habitat and ground therefore agree about where a channel is by
 * construction rather than by two thresholds being kept in step.
 */
double FMassifHabitatField::SampleChannelDistanceMeters(FVector2d PositionMeters) const
{
	const FMassifRiverField* RiverField = TerrainField->GetRiverField();
	if (RiverField == nullptr)
	{
		return TNumericLimits<double>::Max();
	}
	return RiverField->SampleSignedDistanceMeters(PositionMeters);
}
