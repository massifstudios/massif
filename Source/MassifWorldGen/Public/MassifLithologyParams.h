#pragma once

#include "CoreMinimal.h"

/**
 * What the rock under the world is made of.
 *
 * Almost nothing here is a weather term. A band of limestone is where it is
 * because of what happened to the crust long before anything grew on it, so the
 * pattern comes from the seed and from world position and from nothing else.
 * What the weather gets to do is break it, which is the one term below that
 * reads a climate at all.
 */
struct FMassifLithologyParams
{
	/**
	 * Cells the rock is solved at. Coarse: a formation is kilometres across and
	 * nothing in it turns over within a few metres.
	 */
	double CellSizeMeters = 64.0;

	/**
	 * Width of one band of rock, measured across the strike. Four kilometres is a
	 * major formation, so a massif is built of a handful of them — which is what a
	 * geological map of one looks like, rather than a smooth gradient.
	 */
	double BandWavelengthMeters = 4000.0;

	/**
	 * How many times further a band runs along the strike than it is wide. This is
	 * what makes the pattern read as bedding tilted up on edge rather than as
	 * blotches.
	 */
	double BandElongation = 6.0;

	/**
	 * Scale of the intrusions, lenses and facies changes inside a band. Without
	 * them the contacts between formations are ruler-straight, which no real
	 * mountainside is.
	 */
	double PatchWavelengthMeters = 900.0;

	/** Full spread that finer detail adds to a band's hardness, half of it either way. */
	double PatchAmplitude = 0.5;

	/**
	 * How much of the soil yield is the rock's own chemistry rather than the mirror
	 * of its hardness. Zero would lock soft rock to deep soil, and a crumbling
	 * quartzite that gives nothing but sterile sand would have nowhere to sit.
	 */
	double SoilIndependence = 0.35;

	/**
	 * How much harder the highest ground in the massif reads than the lowest.
	 *
	 * Small, and it is a consequence rather than a cause: what is still standing is
	 * mostly what resisted being taken away.
	 */
	double AltitudeHardening = 0.12;

	/**
	 * Temperature the shattering runs fastest at. The freezing point, because it is
	 * water changing phase inside a crack that levers the rock apart — ground that
	 * never thaws and ground that never freezes both shed very little.
	 */
	double PeakShatteringTemperatureCelsius = 0.0;

	/**
	 * How far either side of that peak the shattering falls to about a third of it.
	 * Wide, because a mean near zero hides a daily swing that crosses it twice.
	 */
	double ShatteringBandCelsius = 6.0;

	/** Share of the shattering that the hardest rock resists. */
	double HardnessResistance = 0.7;

	/** Debris the weakest rock at the freezing point sheds in a thousand years. */
	double MaxScreeMetresPerMillennium = 0.8;

	/**
	 * How much shattering is assumed where no climate is in the graph at all. A
	 * moderate rate everywhere, so a world solved without a temperature still gets
	 * scree slopes rather than none.
	 */
	double BaselineShatteringFraction = 0.3;
};
