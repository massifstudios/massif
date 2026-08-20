#pragma once

#include "CoreMinimal.h"

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

	EMassifGroundType GroundType = EMassifGroundType::Soil;
};

/**
 * Climate and ground cover, as a function of world position:
 *
 *     Habitat(Position, SampleSpacing)
 *
 * Derived from the terrain field and the master seed alone, with no state of
 * its own — the same position under the same seed always answers the same,
 * whoever asks and in whatever order, so this can be sampled from the thread
 * pool and tested headless like the field it reads.
 *
 * Temperature is altitude by lapse rate, warmed or cooled by how the ground
 * faces the sun; humidity combines orographic lift, the windward side of the
 * massif, water gathering in hollows and draining off steep ground, and
 * evaporation where it is hot. Ground type follows from those and the slope.
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
	FMassifHabitatField(TSharedRef<const FMassifTerrainField> InTerrainField, uint32 InMasterSeed);

	FMassifHabitatField(const FMassifHabitatField&) = delete;
	FMassifHabitatField& operator=(const FMassifHabitatField&) = delete;

	/** SampleSpacingMeters is the spacing the caller samples at, as for the terrain field. */
	FMassifHabitatSample Sample(FVector2d PositionMeters, double SampleSpacingMeters) const;

	const FMassifTerrainField& GetTerrainField() const
	{
		return TerrainField.Get();
	}

	/** Direction the prevailing wind travels, drawn from the seed. Slopes facing into it collect the rain. */
	FVector2d GetPrevailingWindDirection() const
	{
		return PrevailingWindDirection;
	}

private:
	/** How far below the ring of neighbours the point sits, -1 (ridge) to 1 (hollow). */
	double SampleConvergence(FVector2d PositionMeters) const;

	/** Large-scale wet and dry regions, -1 to 1, so one massif is not uniformly damp. */
	double SampleWeatherVariation(FVector2d PositionMeters) const;

	TSharedRef<const FMassifTerrainField> TerrainField;
	uint32 WeatherSeed = 0;
	FVector2d PrevailingWindDirection = FVector2d(1.0, 0.0);
};
