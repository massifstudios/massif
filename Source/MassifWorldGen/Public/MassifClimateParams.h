#pragma once

#include "CoreMinimal.h"

/**
 * What the temperature field is tuned by.
 *
 * The zero point is not here: the mean temperature reduced to the datum comes
 * from the georeference, which is the one thing about a world that a shape
 * cannot say (ARCHITECTURE §5.6.1). Everything below is what the terrain does
 * to that number.
 */
struct FMassifClimateParams
{
	/**
	 * Cells the temperature is solved at. Coarse: air temperature is set by
	 * altitude and by a day of sun over a whole hillside, neither of which turns
	 * over within a few metres.
	 */
	double CellSizeMeters = 64.0;

	/**
	 * Degrees lost per metre climbed. 6.5 C per kilometre is the standard
	 * atmosphere's environmental lapse rate, and it is what puts the freezing
	 * level at a definite altitude rather than at a guessed one.
	 */
	double LapseRateCelsiusPerMeter = 0.0065;

	/**
	 * Degrees between ground that takes a full day of sun and ground that takes
	 * none at all, at the same altitude. This is why a north cirque holds snow
	 * into July while the flank opposite it is bare.
	 */
	double InsolationGainCelsius = 4.0;

	/**
	 * Degrees a deep closed basin sits below the slope above it once a night of
	 * cold air has drained into it. Enough to invert the lapse rate locally,
	 * which is what a frost hollow is.
	 */
	double PoolingDepressionCelsius = 5.0;

	/**
	 * Basin depth at which the pooling has reached about two thirds of its full
	 * depression. A hollow a few metres deep traps almost nothing; one deeper
	 * than the cold layer is as cold as it is going to get.
	 */
	double PoolingDepthScaleMeters = 30.0;
};
