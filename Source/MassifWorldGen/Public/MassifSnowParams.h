#pragma once

#include "CoreMinimal.h"

/**
 * What the snowpack solve is tuned by.
 *
 * A degree-day pack rather than a surface energy balance: what decides where
 * snow lies on a massif is how long the air stayed above freezing and how much
 * sun the ground took, and both of those are already channels. Everything here
 * is a rate, a ratio or a bound, never a convergence test — the year is walked a
 * fixed number of times (ARCHITECTURE §4).
 */
struct FMassifSnowParams
{
	/**
	 * Cells the snow channels are solved at. As coarse as the climate driving
	 * them, which is what makes twenty-four slices of a year affordable at all
	 * (ARCHITECTURE §5.6.4).
	 */
	double CellSizeMeters = 64.0;

	/**
	 * Depth of fresh snow per depth of the water in it. New snow is about a tenth
	 * water, so a millimetre of precipitation arrives as a centimetre of pack.
	 */
	double SnowToWaterRatio = 10.0;

	/** Air temperature the falling phase turns over at, and the width of the turn. */
	double FreezingCelsius = 1.0;
	double SnowTransitionCelsius = 2.0;

	/**
	 * Metres of pack lost per degree above the melt threshold per day. Measured
	 * degree-day factors for seasonal snow run around five millimetres of water a
	 * day per degree, which at a tenth density is the six millimetres of pack here.
	 */
	double DegreeDayFactorMetresPerDegreeDay = 0.05;

	/** Air temperature melt starts at. Snow at zero is already at its melting point. */
	double MeltThresholdCelsius = 0.0;

	/**
	 * How far the sun moves melt away from what the air temperature alone would
	 * give.
	 *
	 * The insolation this multiplies is a ratio against level open ground on an
	 * average growing-season day, so it carries the season as well as the aspect:
	 * level ground reads about a quarter in midwinter and a fifth more than one at
	 * midsummer, and the growing season averages to exactly one. A warm spell in
	 * January therefore takes less pack than the same spell in May does, and a
	 * shaded north face melts slower than the flank opposite it all year — which is
	 * why the two sides of one valley clear weeks apart.
	 *
	 * Held below one on purpose: at one the darkest midwinter ground would reach a
	 * melt factor of zero, and above it the term would want to go negative and turn
	 * a thaw into a snowfall.
	 */
	double InsolationMeltGain = 0.6;

	/**
	 * Metres of pack lost a day straight to vapour, without melting first. Small
	 * against melt, but it runs all winter on ground that never rises above
	 * freezing, and on a scoured crest it is the only thing that removes snow.
	 */
	double SublimationMetresPerDay = 0.0005;

	/**
	 * How hard the wind moves snow off exposed ground into shelter. Exposure is 1
	 * over open level ground, so at 0.8 a fully sheltered pocket takes nearly twice
	 * what falls on it while a swept crest keeps a fraction of it.
	 */
	double WindRedistributionStrength = 0.8;

	/** Bounds on that share: a crest is never swept quite bare, a hollow never loaded without limit. */
	double MinRedistributionWeight = 0.2;
	double MaxRedistributionWeight = 2.5;

	/**
	 * Depth below which the ground counts as bare. A few centimetres of old snow
	 * behind a rock is not a snow cover, and without a floor the cover duration
	 * would read a whole year of it.
	 */
	double LyingSnowThresholdMetres = 0.05;

	/**
	 * Density snow falls at, and the density it settles towards.
	 *
	 * A hundred is new snow that a ski sinks through; four hundred is a spring pack
	 * that has been through enough melt-freeze cycles to walk on. What separates the
	 * two is the whole difference between ground a groomer can work and ground it
	 * cannot, which is why the pack carries a density rather than a depth alone.
	 */
	double FreshSnowDensityKilogramsPerCubicMetre = 100.0;
	double SettledSnowDensityKilogramsPerCubicMetre = 400.0;

	/** Share of the way to the settled density a cold pack closes in one half-month. */
	double CompactionRatePerSlice = 0.15;

	/**
	 * How much faster a pack at its melting point settles than a cold one. Melt-
	 * freeze rounds the grains off and packs them in days; snow at twenty below
	 * keeps its structure for months.
	 */
	double WarmCompactionMultiplier = 3.0;

	/** Degrees below the melt threshold over which that speed-up comes in. */
	double WarmCompactionBandCelsius = 5.0;

	/**
	 * What the ground reflects, fresh, settled, and with nothing lying on it.
	 *
	 * Snow keeps its own ground cold by throwing the sunlight back off it, and a
	 * settled spring pack does far less of that than a fresh fall does — which is
	 * why an old pack goes in a rush once it starts going at all.
	 */
	double FreshSnowAlbedo = 0.85;
	double OldSnowAlbedo = 0.45;
	double BareGroundAlbedo = 0.15;

	/**
	 * Slope band a slab both forms on and lets go from. Below the lower angle snow
	 * simply sits where it fell; above the upper one it sloughs off in small loads
	 * as it arrives and never builds the slab that would release.
	 */
	double MinReleaseSlopeDegrees = 30.0;
	double MaxReleaseSlopeDegrees = 55.0;

	/** Peak depth at which a release area is as loaded as it is counted for. */
	double ReleaseFullLoadDepthMetres = 1.5;

	/** Slope a running slab comes to rest below, which is what ends a path on the valley floor. */
	double RunoutSlopeDegrees = 15.0;

	/** Cells one release is traced down before the trace gives up. A budget, never a convergence test. */
	int32 MaxRunoutStepCount = 128;

	/** Release passing through a cell at which its track reads as fully marked. */
	double PathSaturationLoad = 3.0;
};
