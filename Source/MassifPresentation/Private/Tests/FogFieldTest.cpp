#include "MassifElevationSource.h"
#include "MassifEnvironmentBakeParams.h"
#include "MassifEnvironmentGraph.h"
#include "MassifEnvironmentParams.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifWindProcesses.h"
#include "MassifSunProcesses.h"
#include "Misc/AutomationTest.h"
#include "Weather/MassifFogField.h"

namespace
{
	constexpr uint32 FogTestSeed = 8821;
	constexpr double FogTestWorldMeters = 4000.0;
	constexpr double FogTestLatitudeDegrees = 47.2;

	/** Deep midwinter, when the nights are longest and the sun clears least. */
	constexpr double FogTestWinterDay = 355.0;

	/** A pit, because a closed hollow is the shape fog actually collects in. */
	TUniquePtr<FMassifEnvironmentScheduler> SolveFogWorld()
	{
		FMassifAnalyticElevationSource::FParams TerrainParams;
		TerrainParams.Shape = EMassifAnalyticTerrain::Pit;
		TerrainParams.WorldSizeMeters = FogTestWorldMeters;
		TerrainParams.BaseAltitudeMeters = 2000.0;
		TerrainParams.ReliefMeters = 600.0;

		FMassifGeoreference Georeference;
		Georeference.LatitudeDegrees = FogTestLatitudeDegrees;

		TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MakeUnique<FMassifEnvironmentScheduler>(
			MakeShared<FMassifAnalyticElevationSource>(TerrainParams), Georeference, FogTestSeed);

		FMassifEnvironmentParams Params;
		Params.Water.CellSizeMeters = 32.0;
		Params.Water.DropletsPerCell = 0.05;
		Params.Water.MaxDropletSteps = 8;
		Params.Sun.CellSizeMeters = 64.0;
		Params.Sun.HorizonStepCount = 12;
		Params.Sun.SunPathStepCount = 6;
		MassifRegisterEnvironmentProcesses(*Scheduler, Params);

		FString BuildError;
		checkf(Scheduler->BuildSchedule(BuildError), TEXT("%s"), *BuildError);
		Scheduler->Execute();
		return Scheduler;
	}

	/** The pit floor, which is the world origin, and a point up on the rim. */
	constexpr double FogTestRimOffsetMeters = 1900.0;

	FMassifSunPosition SunAtDayFraction(double DayFraction)
	{
		return MassifSunAtTimeOfDay(FogTestLatitudeDegrees, FogTestWinterDay, DayFraction);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFogFormsOvernightTest,
	"Massif.Presentation.Fog.FormsOvernight",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FFogFormsOvernightTest::RunTest(const FString& Parameters)
{
	// Radiation fog is made by the night, not by the morning: the ground radiates
	// its heat to a clear sky and chills the air past its dew point. So the fog is
	// deepest just before dawn and there is none of it in the afternoon.
	const FMassifFogParams FogParams;

	// A midwinter night at this latitude runs about sixteen hours, which is more
	// than the fog needs to reach its full depth, so the interesting comparison is
	// early in the night against the end of it rather than midnight against dawn.
	constexpr double Afternoon = 0.6;
	constexpr double JustAfterSunset = 0.70;
	constexpr double Midnight = 0.0;

	const FMassifFogConditions Cleared =
		MassifSolveFogConditions(FogParams, 1400.0, FogTestLatitudeDegrees, FogTestWinterDay, Afternoon);
	const FMassifFogConditions EarlyNight =
		MassifSolveFogConditions(FogParams, 1400.0, FogTestLatitudeDegrees, FogTestWinterDay, JustAfterSunset);
	const FMassifFogConditions Deep =
		MassifSolveFogConditions(FogParams, 1400.0, FogTestLatitudeDegrees, FogTestWinterDay, Midnight);

	TestTrue(
		FString::Printf(TEXT("The afternoon has burned it off (%.3f)"), Cleared.Strength),
		Cleared.Strength < 0.01);
	TestTrue(
		FString::Printf(TEXT("An hour after sunset there is a little (%.3f)"), EarlyNight.Strength),
		EarlyNight.Strength > 0.0 && EarlyNight.Strength < 0.5);
	TestTrue(
		FString::Printf(TEXT("And by the small hours the night has made all it can (%.3f)"), Deep.Strength),
		Deep.Strength > EarlyNight.Strength);

	// The lid rises with the fog under it, so a shallow layer early fills only the
	// deepest ground and by dawn it has reached the shoulders.
	TestTrue(
		TEXT("The lid rises through the night with the fog under it"),
		Deep.LidAltitudeMeters > EarlyNight.LidAltitudeMeters);
	TestEqual(
		TEXT("And sits on the floor when there is no fog at all"),
		Cleared.LidAltitudeMeters, 1400.0 + FogParams.LidHeightMeters * Cleared.Strength);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFogFillsTheHollowTest,
	"Massif.Presentation.Fog.FillsTheHollow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FFogFillsTheHollowTest::RunTest(const FString& Parameters)
{
	// Cold air is dense, so fog pools where it cannot drain and has a flat top.
	// The floor of a hollow must therefore hold more of it than the rim above,
	// and nothing above the lid may hold any.
	const TUniquePtr<FMassifEnvironmentScheduler> World = SolveFogWorld();
	const FMassifFogParams FogParams;
	const FMassifFogField Field(World->GetSharedState(), FogParams);

	const FVector2d Floor = FVector2d::ZeroVector;
	const FVector2d Rim(FogTestRimOffsetMeters, 0.0);

	const double FloorAltitude = Field.GetAltitudeAt(Floor);
	const double RimAltitude = Field.GetAltitudeAt(Rim);
	TestTrue(
		FString::Printf(TEXT("The rim stands above the floor (%.1f against %.1f m)"),
			RimAltitude, FloorAltitude),
		RimAltitude > FloorAltitude + 100.0);

	const FMassifFogConditions BeforeDawn =
		MassifSolveFogConditions(FogParams, FloorAltitude, FogTestLatitudeDegrees, FogTestWinterDay, 0.3);
	const FMassifSunPosition NightSun = SunAtDayFraction(0.3);
	TestFalse(TEXT("Before dawn the sun is still down"), NightSun.IsAboveHorizon());

	const double FloorDensity = Field.DensityAt(Floor, BeforeDawn, NightSun);
	const double RimDensity = Field.DensityAt(Rim, BeforeDawn, NightSun);

	TestTrue(
		FString::Printf(TEXT("The hollow holds fog before dawn (%.4f)"), FloorDensity),
		FloorDensity > 0.0);
	TestTrue(
		FString::Printf(TEXT("And more of it than the rim above (%.4f against %.4f)"),
			FloorDensity, RimDensity),
		FloorDensity > RimDensity);

	// Above the lid there is clear air, which is what makes it a lake rather than a
	// cloud: a peak stands out of it into sunshine.
	TestTrue(
		FString::Printf(TEXT("The lid stands below the rim, so the rim is clear (%.1f m)"),
			BeforeDawn.LidAltitudeMeters),
		BeforeDawn.LidAltitudeMeters < RimAltitude);
	TestEqual(TEXT("Nothing above the lid holds fog"), RimDensity, 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFogBurnsOffWhereTheSunReachesTest,
	"Massif.Presentation.Fog.BurnsOffWhereTheSunReaches",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FFogBurnsOffWhereTheSunReachesTest::RunTest(const FString& Parameters)
{
	// The point of keeping the horizon per bearing (Sun.Horizon, W0). Whether the
	// sun has reached a place is not the hour on its own: it is the sun's bearing
	// tested against the skyline standing in that direction. Inside a hollow the
	// skyline is high all round, so the floor stays shaded well after sunrise while
	// open ground at the same altitude is already lit.
	const TUniquePtr<FMassifEnvironmentScheduler> World = SolveFogWorld();
	const FMassifFogParams FogParams;
	const FMassifFogField Field(World->GetSharedState(), FogParams);

	const FVector2d Floor = FVector2d::ZeroVector;
	const double FloorAltitude = Field.GetAltitudeAt(Floor);

	// Just after sunrise in midwinter, when the sun is up but standing very low.
	constexpr double JustAfterSunrise = 0.34;
	const FMassifSunPosition LowSun = SunAtDayFraction(JustAfterSunrise);
	TestTrue(TEXT("The sun has risen"), LowSun.IsAboveHorizon());

	TestFalse(
		FString::Printf(TEXT("But it has not cleared the wall of the hollow yet (%.2f degrees up)"),
			FMath::RadiansToDegrees(LowSun.AltitudeRadians)),
		Field.IsSunlitAt(Floor, LowSun));

	// The same instant, with the sun forced high enough to clear anything.
	FMassifSunPosition HighSun = LowSun;
	HighSun.AltitudeRadians = FMath::DegreesToRadians(80.0);
	TestTrue(TEXT("A sun overhead does reach the floor"), Field.IsSunlitAt(Floor, HighSun));

	const FMassifFogConditions Morning = MassifSolveFogConditions(
		FogParams, FloorAltitude, FogTestLatitudeDegrees, FogTestWinterDay, JustAfterSunrise);

	const double ShadedDensity = Field.DensityAt(Floor, Morning, LowSun);
	const double SunlitDensity = Field.DensityAt(Floor, Morning, HighSun);

	TestTrue(
		FString::Printf(TEXT("Shaded ground keeps its fog after sunrise (%.4f)"), ShadedDensity),
		ShadedDensity > 0.0);
	TestTrue(
		FString::Printf(TEXT("And the sun takes most of it where it lands (%.4f against %.4f)"),
			SunlitDensity, ShadedDensity),
		SunlitDensity < ShadedDensity);

	// Most, not all: a valley does not clear the instant the first beam lands, and
	// what is left is what gives the burn-off an edge that moves.
	TestTrue(
		FString::Printf(TEXT("But not all of it at once (%.4f)"), SunlitDensity),
		SunlitDensity > 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFogPotentialFollowsTheGroundTest,
	"Massif.Presentation.Fog.PotentialFollowsTheGround",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FFogPotentialFollowsTheGroundTest::RunTest(const FString& Parameters)
{
	// The static half of the factorisation, and the part that would move into the
	// response table (§5.6.6). It is the product of three baked channels, and the
	// test states that product rather than trusting it: a fog that ignored the wind
	// would sit on an exposed crest and one that ignored wetness would sit on bare
	// rock.
	const TUniquePtr<FMassifEnvironmentScheduler> World = SolveFogWorld();
	const FMassifEnvironmentState& State = World->GetState();
	const FMassifFogParams FogParams;
	const FMassifFogField Field(World->GetSharedState(), FogParams);

	const FVector2d Floor = FVector2d::ZeroVector;
	const FVector2d Rim(FogTestRimOffsetMeters, 0.0);

	const double Wetness =
		State.SampleScalar(State.FindChannel(MassifEnvironmentChannels::Wetness()), Floor);
	const double SkyView =
		State.SampleScalar(State.FindChannel(MassifEnvironmentChannels::SkyViewFactor()), Floor);
	const double Exposure =
		State.SampleScalar(State.FindChannel(MassifEnvironmentChannels::WindExposure()), Floor);
	const double Expected =
		Wetness * SkyView * FMath::Clamp(1.0 - Exposure / FogParams.ScouringExposure, 0.0, 1.0);

	const double FloorPotential = Field.PotentialAt(Floor);
	TestTrue(
		FString::Printf(TEXT("The hollow floor is the product of its three channels (%.4f against %.4f)"),
			FloorPotential, Expected),
		FMath::Abs(FloorPotential - Expected) <= 1.0e-9);
	TestTrue(
		FString::Printf(TEXT("Which is some fog and not none (%.4f)"), FloorPotential),
		FloorPotential > 0.0);
	TestTrue(TEXT("And never more than all of it"), FloorPotential <= 1.0);

	// Exposure is what separates the two: the flow accelerates over the rim and
	// scours it, while the hollow below is sheltered enough to hold what forms.
	const double RimPotential = Field.PotentialAt(Rim);
	TestTrue(
		FString::Printf(TEXT("The wind-scoured rim holds far less than the hollow (%.4f against %.4f)"),
			RimPotential, FloorPotential),
		RimPotential < FloorPotential);

	// And tightening what counts as scoured has to take the hollow too, which is
	// what says the wind term is live rather than incidental.
	FMassifFogParams ScouredParams;
	ScouredParams.ScouringExposure = UE_DOUBLE_KINDA_SMALL_NUMBER;
	const FMassifFogField ScouredField(World->GetSharedState(), ScouredParams);
	TestEqual(
		TEXT("Ground the wind scours holds none at all"),
		ScouredField.PotentialAt(Floor), 0.0);
	return true;
}
