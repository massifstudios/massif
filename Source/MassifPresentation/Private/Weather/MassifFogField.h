#pragma once

#include "CoreMinimal.h"
#include "MassifEnvironmentState.h"
#include "MassifSunGeometry.h"

/** What the fog solve is tuned by. All of it is a look decision. */
struct FMassifFogParams
{
	/** How far above the valley floor the inversion lid sits once the night has done its work. */
	double LidHeightMeters = 400.0;

	/** Depth of fog below the lid that reads as full density. */
	double FillDepthMeters = 150.0;

	/** Hours of darkness it takes for fog to build to its full depth. */
	double BuildHours = 7.0;

	/** Hours after sunrise the general warming has cleared it, wherever the sun did not reach first. */
	double ClearHours = 4.0;

	/**
	 * How much of the fog direct sun takes away where it lands.
	 *
	 * Not all of it: a valley does not clear the instant the first beam reaches
	 * the floor, and leaving a share behind is what gives the burn-off an edge that
	 * moves rather than a line that switches.
	 */
	double SunBurnFraction = 0.8;

	/**
	 * Wind exposure at which the fog is scoured away entirely.
	 *
	 * `Wind.Exposure` is 1 on open ground, falls towards 0 where terrain encloses a
	 * point, and rises past 1 over a crest the flow accelerates across. So this
	 * belongs above 1: a hollow at 0.6 holds its fog and a rim at 1.4 does not.
	 */
	double ScouringExposure = 1.4;
};

/** Where the fog lid stands and how much fog there is to hold under it, at one moment. */
struct FMassifFogConditions
{
	double LidAltitudeMeters = 0.0;

	/** 0 for none at all, 1 for as much as this night can make. */
	double Strength = 0.0;

	bool IsAnyFog() const
	{
		return Strength > UE_DOUBLE_KINDA_SMALL_NUMBER;
	}
};

/**
 * Fog over the whole world, at a moment (ARCHITECTURE §6.1).
 *
 * Radiation fog forms because the ground radiates its heat to a clear sky
 * overnight and chills the air past its dew point, and cold air is dense, so it
 * pools where it cannot drain. It therefore has a flat top, and the sun destroys
 * it rather than making it.
 *
 * That factors the way the rest of the weather does. What the night is doing is
 * two global numbers, the same over a massif twenty kilometres across. What this
 * particular ground does with them is a property of the ground, and the bake
 * already holds all of it: how wet it is, how much sky it can radiate to, and how
 * exposed it is to the wind that would mix the fog away.
 *
 * The one thing that is neither is the burn-off, and it is the interesting one.
 * Whether the sun has reached a point is not a property of the ground or of the
 * hour on its own — it is the sun's current bearing tested against the skyline
 * stored at that bearing (`Sun.Horizon`). A shaded valley floor under a high
 * ridge keeps its fog until midday while an open shoulder a kilometre away
 * cleared at nine.
 *
 * Presentation-only. Nothing gameplay-relevant reads it, which is what allows it
 * to read bake channels directly (ARCHITECTURE §4).
 */
class FMassifFogField
{
public:
	FMassifFogField(TSharedRef<const FMassifEnvironmentState> InEnvironment, const FMassifFogParams& InParams);

	/**
	 * How much fog this ground would hold if the night were perfect, 0 to 1.
	 *
	 * A property of the terrain alone and constant for a given world: wet, sheltered
	 * ground open to the sky holds fog, and a dry exposed shoulder under a ridge
	 * does not. This is the part that would move into the response table (§5.6.6)
	 * once the sim needs it.
	 */
	double PotentialAt(FVector2d PositionMeters) const;

	/** Fog density at a point right now, 0 to 1. */
	double DensityAt(
		FVector2d PositionMeters, const FMassifFogConditions& Conditions,
		const FMassifSunPosition& Sun) const;

	/** Whether direct sun reaches this point, from the skyline stored at the sun's bearing. */
	bool IsSunlitAt(FVector2d PositionMeters, const FMassifSunPosition& Sun) const;

	double GetAltitudeAt(FVector2d PositionMeters) const;

private:
	TSharedRef<const FMassifEnvironmentState> Environment;
	FMassifFogParams Params;

	FMassifChannelHandle AltitudeChannel;
	FMassifChannelHandle WetnessChannel;
	FMassifChannelHandle SkyViewChannel;
	FMassifChannelHandle HorizonChannel;
	FMassifChannelHandle WindExposureChannel;
};

/**
 * Where the lid stands and how strong the fog is, from the clock and the sun.
 *
 * Stateless, and derived rather than integrated, because presentation holds no
 * sim state (ARCHITECTURE §3). It is a climatological night: every morning is
 * the same morning until the weather state (W3) exists to vary it, at which
 * point this becomes the term the weather scales rather than the whole answer.
 */
FMassifFogConditions MassifSolveFogConditions(
	const FMassifFogParams& Params, double FloorAltitudeMeters, double LatitudeDegrees, double DayOfYear,
	double DayFraction);
