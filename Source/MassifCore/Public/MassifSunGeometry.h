#pragma once

#include "CoreMinimal.h"

/**
 * Where the sun stands, in the frame the terrain is in: X east, Y north, Z up.
 *
 * Shared rather than owned by whoever needed it first. The bake integrates a
 * day of it to decide where trees grow; presentation asks where it is right now
 * to point a light and to work out whether a valley floor is still in shadow.
 * Two copies of the same spherical trigonometry would eventually disagree, and
 * the disagreement would show up as the bake and the runtime shading a slope
 * differently.
 */
struct MASSIFCORE_API FMassifSunPosition
{
	double AltitudeRadians = 0.0;

	/** Unit vector towards the sun. Meaningless where the altitude is negative. */
	FVector3d Direction = FVector3d::ZAxisVector;

	bool IsAboveHorizon() const
	{
		return AltitudeRadians > 0.0;
	}

	/** Clockwise from north, which is the convention the horizon sweep is stored in. */
	double GetAzimuthRadians() const
	{
		const double Azimuth = FMath::Atan2(Direction.X, Direction.Y);
		if (Azimuth < 0.0)
		{
			return Azimuth + UE_DOUBLE_TWO_PI;
		}
		return Azimuth;
	}
};

/** Tilt of the earth's axis against its orbit, which is what gives the year a summer. */
constexpr double MassifAxialTiltDegrees = 23.44;

MASSIFCORE_API double MassifSolarDeclinationRadians(double DayOfYear);

MASSIFCORE_API FMassifSunPosition MassifSunAt(
	double LatitudeRadians, double DeclinationRadians, double HourAngleRadians);

/** Hour angle the sun crosses the horizon at, which is what bounds a day being integrated. */
MASSIFCORE_API double MassifSunsetHourAngleRadians(double LatitudeRadians, double DeclinationRadians);

/**
 * Where the sun stands at a point in a day, from the wall clock's own terms.
 *
 * `DayFraction` is 0 at midnight and 0.5 at solar noon, which is what the hour
 * angle is measured from. No equation of time and no longitude correction: both
 * are worth under a quarter of an hour, and nothing in the game is told the
 * difference between local solar time and the clock on the wall.
 */
MASSIFCORE_API FMassifSunPosition MassifSunAtTimeOfDay(
	double LatitudeDegrees, double DayOfYear, double DayFraction);
