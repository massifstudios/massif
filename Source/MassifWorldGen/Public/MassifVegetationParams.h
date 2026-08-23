#pragma once

#include "CoreMinimal.h"

/**
 * What the vegetation solve is tuned by.
 *
 * The couplings back into soil and wind are deliberately shaped so that zero
 * means no effect: a process in a relaxation group reads channels no one has
 * written yet on the first iteration, and a channel that has not been written
 * reads as zero. Keeping "no vegetation" and "no reading yet" the same value is
 * what lets the loop start from bare ground without a special case.
 */
struct FMassifVegetationParams
{
	double CellSizeMeters = 64.0;

	/** Growing-season mean below which nothing grows, and above which warmth stops limiting. */
	double MinimumGrowingTemperatureCelsius = 0.0;
	double ComfortableTemperatureCelsius = 10.0;

	/** Damp that suits growth best, and how far either side of it is still tolerable. */
	double OptimalWetness = 0.55;
	double WetnessTolerance = 0.45;

	/** Soil this deep supports full cover; nothing roots where it runs out. */
	double FullCoverSoilDepthMeters = 0.4;

	/**
	 * How much exposure above neutral costs. Wind does not merely slow growth on a
	 * crest, it shears it: this is what produces the krummholz band rather than a
	 * forest that simply stops at a contour.
	 */
	double WindDamageSensitivity = 1.2;

	/** Share of the year under snow past which the ground has no season left to grow in. */
	double SmotheringCoverDuration = 0.7;

	/** Warmth a tree needs over what ground cover needs, and the band it comes in over. */
	double TreelineTemperatureCelsius = 5.0;
	double TreelineBandCelsius = 3.0;

	/** Soil a tree needs to stand up in, as against what a sedge needs. */
	double TreeRootingDepthMeters = 0.6;

	/** Height of a fully closed canopy. */
	double MaxCanopyHeightMeters = 28.0;

	/**
	 * Share of the rain a fully closed canopy catches and gives back to the air
	 * before any of it reaches the ground. A closed conifer stand really does hold
	 * back about a third of a year's rainfall, which is why forested ground yields
	 * less runoff than cleared ground under the same weather.
	 */
	double MaxInterceptionFraction = 0.3;

	/** Wetness past which the ground is waterlogged, and what stands on it is bog rather than sward. */
	double WetlandWetness = 0.85;

	/** Cover below which the ground reads as bare: rock, scree and gravel carry about this much. */
	double BareCoverThreshold = 0.08;

	/**
	 * Share of the year under snow past which the ground is a snowbed — a hollow
	 * that clears too late in the summer for anything but what lives in one, and
	 * the duration the scour below is measured from.
	 */
	double SnowbedCoverDuration = 0.6;

	/** Canopy closure at which a stand reads as forest rather than as scattered trees. */
	double ForestCanopyThreshold = 0.35;

	/** Growing-season mean above which the forest belt is broadleaf rather than conifer. */
	double BroadleafTemperatureCelsius = 8.0;

	/** Exposure past which what holds on is dwarf shrub and krummholz, whatever the warmth would allow. */
	double ShrubExposure = 1.15;

	/**
	 * Share of a stand the wind throws per unit of exposure above neutral.
	 *
	 * Windthrow scales with what is standing: an exposed forest edge loses trees
	 * where open ground has none to lose, which is why a clearing widens downwind
	 * of itself rather than staying the shape it was cut.
	 */
	double WindthrowSensitivity = 0.8;

	/** Share cleared per unit of snow lying past the snowbed duration: late lie and avalanche track. */
	double SnowScourSensitivity = 1.2;

	/**
	 * Extra soil held on ground that is vegetated, as a share of what is there.
	 *
	 * Roots hold the ground that roots need, which is the tighter of the two
	 * feedback loops: forest keeps its own substrate, and ground that loses its
	 * cover loses the soil that would let it grow back.
	 */
	double RootSoilRetention = 0.5;

	/**
	 * Share of the wind a closed canopy takes out of the flow.
	 *
	 * Canopy slows the wind that redistributes the snow that decides where canopy
	 * grows. Under trees the snow lies deep and undrifted for exactly this reason.
	 */
	double CanopyWindDrag = 0.55;
};
