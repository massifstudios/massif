#include "MassifSunGeometry.h"
#include "Misc/AutomationTest.h"

namespace
{
	/** Sölden, which is the window the shipped elevation model covers. */
	constexpr double SunGeometryLatitudeDegrees = 47.2;

	/** Days the solstices and an equinox fall on, near enough for the angles they pin. */
	constexpr double SummerSolsticeDay = 172.0;
	constexpr double WinterSolsticeDay = 355.0;
	constexpr double EquinoxDay = 80.0;

	double NoonAltitudeDegrees(double DayOfYear)
	{
		return FMath::RadiansToDegrees(
			MassifSunAtTimeOfDay(SunGeometryLatitudeDegrees, DayOfYear, 0.5).AltitudeRadians);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifSunNoonAltitudeTest,
	"Massif.Core.Sun.NoonAltitude",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifSunNoonAltitudeTest::RunTest(const FString& Parameters)
{
	// At noon the sun stands (90 - latitude + declination) above the horizon, which
	// is the one closed form this whole file has to agree with. Stated against
	// trigonometry rather than against remembered numbers, so the test says what the
	// sun does and not what the code last produced.
	const double Colatitude = 90.0 - SunGeometryLatitudeDegrees;

	const double Summer = NoonAltitudeDegrees(SummerSolsticeDay);
	const double Winter = NoonAltitudeDegrees(WinterSolsticeDay);
	const double Equinox = NoonAltitudeDegrees(EquinoxDay);

	TestTrue(
		FString::Printf(TEXT("Midsummer noon stands a full tilt above the equinox (%.2f degrees)"), Summer),
		FMath::Abs(Summer - (Colatitude + MassifAxialTiltDegrees)) <= 1.0);
	TestTrue(
		FString::Printf(TEXT("Midwinter noon a full tilt below it (%.2f degrees)"), Winter),
		FMath::Abs(Winter - (Colatitude - MassifAxialTiltDegrees)) <= 1.0);
	TestTrue(
		FString::Printf(TEXT("And the equinox at the colatitude itself (%.2f degrees)"), Equinox),
		FMath::Abs(Equinox - Colatitude) <= 1.5);

	// Thirty degrees between the two solstices is why a north face holds its snow:
	// the same slope takes a different sun in January than in July.
	TestTrue(
		FString::Printf(TEXT("Which is a forty-seven degree swing across the year (%.2f)"), Summer - Winter),
		FMath::Abs((Summer - Winter) - 2.0 * MassifAxialTiltDegrees) <= 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifSunAzimuthTest,
	"Massif.Core.Sun.Azimuth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifSunAzimuthTest::RunTest(const FString& Parameters)
{
	// The bearing is what indexes Sun.Horizon, so getting it turned round would put
	// every shadow on the wrong side of the mountain at the right time of day.
	// North of the tropics the noon sun is due south, and the morning sun east of it.
	const FMassifSunPosition Noon =
		MassifSunAtTimeOfDay(SunGeometryLatitudeDegrees, SummerSolsticeDay, 0.5);
	const FMassifSunPosition Morning =
		MassifSunAtTimeOfDay(SunGeometryLatitudeDegrees, SummerSolsticeDay, 0.3);
	const FMassifSunPosition Evening =
		MassifSunAtTimeOfDay(SunGeometryLatitudeDegrees, SummerSolsticeDay, 0.7);

	const double NoonDegrees = FMath::RadiansToDegrees(Noon.GetAzimuthRadians());
	const double MorningDegrees = FMath::RadiansToDegrees(Morning.GetAzimuthRadians());
	const double EveningDegrees = FMath::RadiansToDegrees(Evening.GetAzimuthRadians());

	TestTrue(
		FString::Printf(TEXT("The noon sun bears due south (%.2f degrees)"), NoonDegrees),
		FMath::Abs(NoonDegrees - 180.0) <= 1.0);
	TestTrue(
		FString::Printf(TEXT("The morning sun east of that (%.2f degrees)"), MorningDegrees),
		MorningDegrees < 180.0 && MorningDegrees > 45.0);
	TestTrue(
		FString::Printf(TEXT("And the evening sun west of it (%.2f degrees)"), EveningDegrees),
		EveningDegrees > 180.0 && EveningDegrees < 315.0);

	// The direction vector and the bearing must be the same statement, since the
	// horizon lookup takes one and the light takes the other.
	TestTrue(TEXT("The noon sun points south in the vector too"), Noon.Direction.Y < 0.0);
	TestTrue(TEXT("The morning sun points east"), Morning.Direction.X > 0.0);
	TestTrue(TEXT("The evening sun west"), Evening.Direction.X < 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifSunNightTest,
	"Massif.Core.Sun.Night",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifSunNightTest::RunTest(const FString& Parameters)
{
	// Fog forms overnight and burns off when the sun clears the skyline, so "is it
	// up" has to be answerable and has to be false at midnight.
	const FMassifSunPosition Midnight =
		MassifSunAtTimeOfDay(SunGeometryLatitudeDegrees, WinterSolsticeDay, 0.0);
	const FMassifSunPosition Noon =
		MassifSunAtTimeOfDay(SunGeometryLatitudeDegrees, WinterSolsticeDay, 0.5);

	TestFalse(TEXT("The sun is down at midnight"), Midnight.IsAboveHorizon());
	TestTrue(TEXT("And up at noon, even in midwinter at this latitude"), Noon.IsAboveHorizon());

	// The sunset hour angle is what bounds a day, and a midwinter day here is short
	// but not absent.
	const double SunsetRadians = MassifSunsetHourAngleRadians(
		FMath::DegreesToRadians(SunGeometryLatitudeDegrees),
		MassifSolarDeclinationRadians(WinterSolsticeDay));
	const double DaylightHours = 2.0 * SunsetRadians / UE_DOUBLE_TWO_PI * 24.0;

	TestTrue(
		FString::Printf(TEXT("A midwinter day runs eight or nine hours (%.2f)"), DaylightHours),
		DaylightHours > 7.5 && DaylightHours < 9.5);
	return true;
}
