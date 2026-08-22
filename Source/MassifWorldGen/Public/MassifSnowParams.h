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
	 * give. The insolation channel is a ratio against level open ground, so at 0.6
	 * a shaded north face melts at roughly half the rate of a face taking a full
	 * day of sun — which is why the two sides of one valley clear weeks apart.
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
};
