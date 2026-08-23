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

	/**
	 * How far the wettest and the driest half-month stand from an even year, as a
	 * share of the mean half-month.
	 *
	 * A third is a temperate massif with a convective summer: thunderstorms over
	 * warm ground put more water on a slope in August than any frontal winter
	 * month manages. It moves nothing but the timing — the year still totals what
	 * the annual channel says it does.
	 */
	double SeasonalAmplitude = 0.35;

	/**
	 * Half-month the wet season peaks in, counting from the first half of January.
	 * Slice 14 is the first half of August: an Alpine massif takes its water in
	 * high summer off convection, not off winter fronts, which is why its
	 * snowpack is built out of a small share of a large annual total.
	 */
	int32 WettestSlice = 14;

	/**
	 * What a year takes off ground with water to spare at the reference
	 * temperature. Temperature stands in for the energy available, Thornthwaite
	 * style: the humidity and wind speed a Penman form needs are not in the graph.
	 */
	double ReferenceEvapotranspirationMillimetres = 700.0;

	/** Below this nothing evaporates: frozen ground returns no water to the air. */
	double EvapotranspirationBaseCelsius = 0.0;

	/** Temperature the reference figure is quoted at. */
	double EvapotranspirationReferenceCelsius = 15.0;

	/**
	 * Share of the potential that bare ground manages. Rock and mineral soil
	 * evaporate what is on the surface and transpire nothing, so the water balance
	 * over scree closes very differently from the one over a forest.
	 */
	double BareGroundEvaporationFraction = 0.35;
};
