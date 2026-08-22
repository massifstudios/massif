#pragma once

#include "CoreMinimal.h"
#include "MassifEnvironmentChannel.h"

class FMassifEnvironmentState;
class FMassifTerrainField;

/** Ground cover class of a point on the surface, as far as anything growing on it cares. */
enum class EMassifGroundType : uint8
{
	Rock,
	Scree,
	Soil,
	Wetland,
	PermanentSnow,

	/** The wetted bed of a channel. Nothing roots in a river, whatever else is true of the ground. */
	Water,

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
	inline constexpr uint32 Water = MassifGroundTypeBit(EMassifGroundType::Water);
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

	/**
	 * Metres out from the wetted edge of the nearest river, negative in the water
	 * and enormous where there is no channel within reach.
	 *
	 * Standing water above does not cover this: the depression fill only finds
	 * hollows water cannot leave, and a river is water that is leaving. Without it
	 * the whole channel network reads as ordinary damp ground, and trees are
	 * planted in it.
	 */
	double ChannelDistanceMeters = TNumericLimits<double>::Max();

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
 * Climate is read, not invented. Temperature comes from the temperature channel,
 * which already carries the lapse rate, the sun the ground actually took and the
 * cold air pooled in its hollows; rainfall comes from the precipitation channel,
 * which already carries the orographic gain and the rain shadow behind it. What
 * is left here is the part that is genuinely about the ground: the catchment
 * water draining through a point, what evaporation takes back out of it, the wet
 * margin beside a channel, and a ground type decided by soil depth and standing
 * water first and slope second — so bare rock is ground the water stripped
 * rather than ground over an angle.
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
		TSharedRef<const FMassifEnvironmentState> InEnvironment,
		uint32 InMasterSeed);

	FMassifHabitatField(const FMassifHabitatField&) = delete;
	FMassifHabitatField& operator=(const FMassifHabitatField&) = delete;

	/** SampleSpacingMeters is the spacing the caller samples at, as for the terrain field. */
	FMassifHabitatSample Sample(FVector2d PositionMeters, double SampleSpacingMeters) const;

	const FMassifTerrainField& GetTerrainField() const
	{
		return TerrainField.Get();
	}

	const FMassifEnvironmentState& GetEnvironment() const
	{
		return Environment.Get();
	}

private:
	/** Distance to the nearest channel, from the river field the terrain was carved with. */
	double SampleChannelDistanceMeters(FVector2d PositionMeters) const;

	TSharedRef<const FMassifTerrainField> TerrainField;
	TSharedRef<const FMassifEnvironmentState> Environment;

	/** Resolved once: sampling a channel by name on every query would hash a string per plant. */
	FMassifChannelHandle SoilDepthChannel;
	FMassifChannelHandle StandingWaterChannel;
	FMassifChannelHandle WetnessChannel;
	FMassifChannelHandle TemperatureChannel;
	FMassifChannelHandle PrecipitationChannel;
};
