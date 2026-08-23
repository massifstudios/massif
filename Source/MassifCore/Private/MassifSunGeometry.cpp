#include "MassifSunGeometry.h"

double MassifSolarDeclinationRadians(double DayOfYear)
{
	const double OrbitFraction = UE_DOUBLE_TWO_PI * (284.0 + DayOfYear) / 365.0;
	return FMath::DegreesToRadians(MassifAxialTiltDegrees) * FMath::Sin(OrbitFraction);
}

FMassifSunPosition MassifSunAt(double LatitudeRadians, double DeclinationRadians, double HourAngleRadians)
{
	const double SinAltitude = FMath::Sin(LatitudeRadians) * FMath::Sin(DeclinationRadians)
		+ FMath::Cos(LatitudeRadians) * FMath::Cos(DeclinationRadians) * FMath::Cos(HourAngleRadians);

	FMassifSunPosition Sun;
	Sun.AltitudeRadians = FMath::Asin(FMath::Clamp(SinAltitude, -1.0, 1.0));

	// Azimuth clockwise from north, so the horizontal part is (sin, cos) east-north.
	const double CosAltitude = FMath::Max(FMath::Cos(Sun.AltitudeRadians), UE_DOUBLE_KINDA_SMALL_NUMBER);
	const double CosAzimuth = (FMath::Sin(DeclinationRadians) * FMath::Cos(LatitudeRadians)
		- FMath::Cos(DeclinationRadians) * FMath::Sin(LatitudeRadians) * FMath::Cos(HourAngleRadians))
		/ CosAltitude;

	double AzimuthRadians = FMath::Acos(FMath::Clamp(CosAzimuth, -1.0, 1.0));
	if (HourAngleRadians > 0.0)
	{
		AzimuthRadians = UE_DOUBLE_TWO_PI - AzimuthRadians;
	}

	Sun.Direction = FVector3d(
		CosAltitude * FMath::Sin(AzimuthRadians), CosAltitude * FMath::Cos(AzimuthRadians), SinAltitude);
	return Sun;
}

double MassifSunsetHourAngleRadians(double LatitudeRadians, double DeclinationRadians)
{
	const double CosHourAngle = -FMath::Tan(LatitudeRadians) * FMath::Tan(DeclinationRadians);
	if (CosHourAngle <= -1.0)
	{
		// Midnight sun: the whole rotation is daylight.
		return UE_DOUBLE_PI;
	}
	if (CosHourAngle >= 1.0)
	{
		return 0.0;
	}
	return FMath::Acos(CosHourAngle);
}

FMassifSunPosition MassifSunAtTimeOfDay(double LatitudeDegrees, double DayOfYear, double DayFraction)
{
	// Solar noon sits halfway through the day, so the hour angle runs from minus pi
	// at midnight through zero at noon.
	const double HourAngleRadians = (DayFraction - 0.5) * UE_DOUBLE_TWO_PI;
	return MassifSunAt(
		FMath::DegreesToRadians(LatitudeDegrees), MassifSolarDeclinationRadians(DayOfYear),
		HourAngleRadians);
}
