#pragma once

#include "CoreMinimal.h"

class FMassifEnvironmentBake;
class FMassifTerrainField;

/** Ground cover class of a point on the surface, as far as anything growing on it cares. */
enum class EMassifGroundType : uint8
{
	Rock,
	Scree,
	Soil,
	Wetland,
	PermanentSnow,
	Count
};

inline constexpr uint32 MassifGroundTypeBit(EMassifGroundType GroundType)
{
	return 1u << static_cast<uint32>(GroundType);
}

/** Ground type bits, for the masks that say which ground a species accepts. */
namespace MassifGround
{
	inline constexpr uint32 Rock = MassifGroundTypeBit(EMassifGroundType::Rock);
	inline constexpr uint32 Scree = MassifGroundTypeBit(EMassifGroundType::Scree);
	inline constexpr uint32 Soil = MassifGroundTypeBit(EMassifGroundType::Soil);
	inline constexpr uint32 Wetland = MassifGroundTypeBit(EMassifGroundType::Wetland);
	inline constexpr uint32 PermanentSnow = MassifGroundTypeBit(EMassifGroundType::PermanentSnow);
}

/** The growing conditions at one point on the surface. */
struct FMassifHabitatSample
{
	double AltitudeMeters = 0.0;
	double SlopeDegrees = 0.0;

	/** Compass bearing the ground faces: 0 north, 90 east. Meaningless on flat ground. */
	double AspectDegrees = 0.0;

	/** Mean growing-season air temperature at the surface. */
	double TemperatureCelsius = 0.0;

	/** Soil moisture, 0 parched to 1 saturated. */
	double Humidity = 0.0;

	/** Loose material over bedrock. Nothing roots where this runs out. */
	double SoilDepthMeters = 0.0;

	/** Water standing on the surface: a tarn, a pond, or a boggy hollow. */
	double StandingWaterDepthMeters = 0.0;

	EMassifGroundType GroundType = EMassifGroundType::Soil;
};

/**
 * Climate and ground cover, as a function of world position:
 *
 *     Habitat(Position, SampleSpacing)
 *
 * Reads the terrain field for shape, the environment bake (ARCHITECTURE §5.6)
 * for water and soil, and the master seed for the weather. It holds no state of
 * its own — the same position under the same seed always answers the same,
 * whoever asks and in whatever order, so this can be sampled from the thread
 * pool and tested headless like the fields it reads.
 *
 * Temperature is altitude by lapse rate, warmed or cooled by how the ground
 * faces the sun. Humidity starts from the bake's wetness — the catchment
 * draining through the point, which no local measurement can see — and climate
 * moves it: orographic lift, the windward side of the massif, evaporation where
 * it is hot, and the seed's own wet and dry regions. Ground type is soil depth
 * and standing water first and slope second, so bare rock is ground the water
 * stripped rather than ground over an angle.
 *
 * Presentation reads this for what to plant; it is not sim state. When forest
 * and scree become semantic masks (ARCHITECTURE §5.5) those masks are derived
 * from the same function, so the two agree.
 *
 * Thread-safe once constructed: sampling touches only immutable state.
 */
class MASSIFWORLDGEN_API FMassifHabitatField
{
public:
	FMassifHabitatField(
		TSharedRef<const FMassifTerrainField> InTerrainField,
		TSharedRef<const FMassifEnvironmentBake> InEnvironmentBake,
		uint32 InMasterSeed);

	FMassifHabitatField(const FMassifHabitatField&) = delete;
	FMassifHabitatField& operator=(const FMassifHabitatField&) = delete;

	/** SampleSpacingMeters is the spacing the caller samples at, as for the terrain field. */
	FMassifHabitatSample Sample(FVector2d PositionMeters, double SampleSpacingMeters) const;

	const FMassifTerrainField& GetTerrainField() const
	{
		return TerrainField.Get();
	}

	const FMassifEnvironmentBake& GetEnvironmentBake() const
	{
		return EnvironmentBake.Get();
	}

	/** Direction the prevailing wind travels, drawn from the seed. Slopes facing into it collect the rain. */
	FVector2d GetPrevailingWindDirection() const
	{
		return PrevailingWindDirection;
	}

private:
	/** Large-scale wet and dry regions, -1 to 1, so one massif is not uniformly damp. */
	double SampleWeatherVariation(FVector2d PositionMeters) const;

	TSharedRef<const FMassifTerrainField> TerrainField;
	TSharedRef<const FMassifEnvironmentBake> EnvironmentBake;
	uint32 WeatherSeed = 0;
	FVector2d PrevailingWindDirection = FVector2d(1.0, 0.0);
};
