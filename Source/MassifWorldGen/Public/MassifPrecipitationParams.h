#pragma once

#include "CoreMinimal.h"

/**
 * What the precipitation solve is tuned by.
 *
 * The numbers are a temperate mid-latitude massif: a valley floor around a
 * metre of rain a year, a summit ridge several times that. Fetches and step
 * counts are fixed, never convergence tests (ARCHITECTURE §4).
 */
struct FMassifPrecipitationParams
{
	/** Cells the precipitation channels are solved at. As coarse as the wind it rides on. */
	double CellSizeMeters = 64.0;

	/** What falls on level ground at the datum, before the massif does anything to it. */
	double BaseAnnualMillimetres = 900.0;

	/** Added per kilometre of altitude: colder air holds less, so more of it comes out. */
	double AltitudeGainMillimetresPerKilometre = 500.0;

	/**
	 * Added per metre per second of air forced upwards.
	 *
	 * Orographic lift is the whole reason one side of a range is green and the
	 * other is brown: air pushed up a windward slope cools, and drops what it can
	 * no longer hold before it reaches the crest.
	 */
	double OrographicGainMillimetresPerLift = 400.0;

	/** Share of the remaining rainfall lost where air is descending instead of climbing. */
	double LeeLossFraction = 0.5;

	/** How far upwind the moisture already wrung out of the air is tracked. */
	double UpwindFetchMeters = 3000.0;

	int32 UpwindStepCount = 16;

	/**
	 * How sharply upwind lift dries the air arriving at a point. This is what makes
	 * a rain shadow a shadow: the far side of a barrier is dry because the near
	 * side already took the water, not because of anything about the far side.
	 */
	double DepletionCoefficient = 0.35;

	/** Nothing plausible falls beyond this, whatever the terrain conspires to do. */
	double MaxAnnualMillimetres = 4000.0;

	/** Air temperature the falling phase turns over at, and the width of the turn. */
	double FreezingCelsius = 1.0;
	double SnowTransitionCelsius = 2.0;
};
