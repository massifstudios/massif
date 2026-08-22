#include "MassifElevationSource.h"
#include "MassifEnvironmentGraph.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifClimateProcesses.h"
#include "MassifPrecipitationProcesses.h"
#include "MassifWindProcesses.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr uint32 PrecipTestSeed = 4711;
	constexpr double PrecipTestWorldMeters = 4096.0;

	/** The wind bearing is pinned in every test: a seed-drawn one makes windward unreadable. */
	FMassifEnvironmentParams MakePrecipParams(double SynopticBearingDegrees)
	{
		FMassifEnvironmentParams Params;
		Params.Water.CellSizeMeters = 32.0;

		// Soil is irrelevant here, and the droplets are the expensive part.
		Params.Water.DropletsPerCell = 0.05;
		Params.Water.MaxDropletSteps = 8;
		Params.Wind.SynopticBearingDegrees = SynopticBearingDegrees;
		return Params;
	}

	FMassifAnalyticElevationSource::FParams MakePrecipTerrain(
		EMassifAnalyticTerrain Shape, double BearingDegrees, double ReliefMeters)
	{
		FMassifAnalyticElevationSource::FParams Params;
		Params.Shape = Shape;
		Params.WorldSizeMeters = PrecipTestWorldMeters;
		Params.BaseAltitudeMeters = 1200.0;
		Params.ReliefMeters = ReliefMeters;
		Params.BearingDegrees = BearingDegrees;
		return Params;
	}

	/**
	 * The whole standard set, which precipitation is part of.
	 *
	 * It reads the wind and the temperature, and the scheduler refuses a graph
	 * whose inputs no registered process produces.
	 */
	TUniquePtr<FMassifEnvironmentScheduler> SolvePrecipitation(
		const FMassifAnalyticElevationSource::FParams& TerrainParams, EMassifFidelity PrecipFidelity,
		double SynopticBearingDegrees)
	{
		TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MakeUnique<FMassifEnvironmentScheduler>(
			MakeShared<FMassifAnalyticElevationSource>(TerrainParams), FMassifGeoreference(), PrecipTestSeed);

		MassifRegisterEnvironmentProcesses(*Scheduler, MakePrecipParams(SynopticBearingDegrees));

		FString Error;
		checkf(Scheduler->BuildSchedule(Error), TEXT("%s"), *Error);
		Scheduler->SetFidelity(MassifEnvironmentProcesses::Precipitation(), PrecipFidelity);
		Scheduler->Execute();
		return Scheduler;
	}

	double SamplePrecipChannel(
		const FMassifEnvironmentScheduler& Scheduler, const FName& ChannelName, FVector2d PositionMeters)
	{
		const FMassifEnvironmentState& State = Scheduler.GetState();
		return State.SampleScalar(State.FindChannel(ChannelName), PositionMeters);
	}

	/** All a uint16 channel can be asked to hold, so nothing here guesses a tolerance. */
	double QuantisationStep(const FMassifEnvironmentScheduler& Scheduler, const FName& ChannelName)
	{
		const FMassifEnvironmentState& State = Scheduler.GetState();
		const FMassifChannelHandle Channel = State.FindChannel(ChannelName);
		const double Span = State.GetChannelMaximum(Channel) - State.GetChannelMinimum(Channel);
		return FMath::Max(Span / 65535.0, UE_DOUBLE_KINDA_SMALL_NUMBER);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPrecipitationAltitudeTest,
	"Massif.WorldGen.Precipitation.Altitude",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPrecipitationAltitudeTest::RunTest(const FString& Parameters)
{
	// Cheap is base plus the altitude gradient and nothing else, so the closed form
	// is exact and worth asserting against rather than a threshold.
	const TUniquePtr<FMassifEnvironmentScheduler> Cone =
		SolvePrecipitation(MakePrecipTerrain(EMassifAnalyticTerrain::Cone, 0.0, 1200.0),
			EMassifFidelity::Cheap, 90.0);

	const FMassifEnvironmentState& State = Cone->GetState();
	const FMassifChannelHandle Altitude = State.FindChannel(MassifEnvironmentChannels::Altitude());
	const FMassifChannelHandle Annual = State.FindChannel(MassifEnvironmentChannels::AnnualPrecipitation());

	// Both readings are taken on the precipitation grid's own cells: it is coarser
	// than the altitude it reads, so sampling the two at an arbitrary point
	// compares a cell value against an interpolation of a finer one.
	const FMassifChannelGrid& Grid = State.GetGrid(Annual);
	const FVector2d Summit = Grid.CellCentreMeters(Grid.Width / 2, Grid.Height / 2);
	const FVector2d Flank = Grid.CellCentreMeters(Grid.Width - 4, Grid.Height / 2);

	const FMassifPrecipitationParams Defaults;
	const double AltitudeDifference =
		State.SampleScalar(Altitude, Summit) - State.SampleScalar(Altitude, Flank);
	const double Expected = Defaults.AltitudeGainMillimetresPerKilometre * AltitudeDifference / 1000.0;

	const double Measured = State.ReadCell(Annual, Grid.Width / 2, Grid.Height / 2)
		- State.ReadCell(Annual, Grid.Width - 4, Grid.Height / 2);

	TestTrue(
		FString::Printf(TEXT("Rainfall follows the altitude gradient (%.1f against %.1f mm)"), Measured, Expected),
		FMath::Abs(Measured - Expected)
			<= 2.0 * QuantisationStep(*Cone, MassifEnvironmentChannels::AnnualPrecipitation()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPrecipitationRainShadowTest,
	"Massif.WorldGen.Precipitation.RainShadow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPrecipitationRainShadowTest::RunTest(const FString& Parameters)
{
	// A ridge across the wind: air climbs the windward flank and drops its water
	// there, and the lee is dry because of what happened on the other side rather
	// than because of anything about the lee itself. Both flanks are at the same
	// altitude, so the altitude term cancels exactly and only the orography is left.
	const TUniquePtr<FMassifEnvironmentScheduler> Ridge =
		SolvePrecipitation(MakePrecipTerrain(EMassifAnalyticTerrain::Ridge, 90.0, 1200.0),
			EMassifFidelity::Standard, 0.0);

	// The synoptic bearing is where the wind comes from, so a northerly blows
	// towards -Y: the north flank is windward and the south flank is in the lee.
	const FVector2d Windward(0.0, 1000.0);
	const FVector2d Lee(0.0, -1000.0);

	const FMassifEnvironmentState& State = Ridge->GetState();
	const FMassifChannelHandle Altitude = State.FindChannel(MassifEnvironmentChannels::Altitude());
	TestTrue(
		TEXT("The two flanks sit at the same altitude"),
		FMath::Abs(State.SampleScalar(Altitude, Windward) - State.SampleScalar(Altitude, Lee)) < 1.0);

	const double WindwardRain =
		SamplePrecipChannel(*Ridge, MassifEnvironmentChannels::AnnualPrecipitation(), Windward);
	const double LeeRain = SamplePrecipChannel(*Ridge, MassifEnvironmentChannels::AnnualPrecipitation(), Lee);

	TestTrue(
		FString::Printf(TEXT("The windward flank takes more than the lee (%.0f against %.0f mm)"),
			WindwardRain, LeeRain),
		WindwardRain > LeeRain);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPrecipitationSnowFractionTest,
	"Massif.WorldGen.Precipitation.SnowFraction",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPrecipitationSnowFractionTest::RunTest(const FString& Parameters)
{
	// The freezing level is what the split is about: everything well above it is
	// snow, everything well below it is rain, and the fraction never leaves 0-1.
	const TUniquePtr<FMassifEnvironmentScheduler> Cone =
		SolvePrecipitation(MakePrecipTerrain(EMassifAnalyticTerrain::Cone, 0.0, 3000.0),
			EMassifFidelity::Standard, 90.0);

	const FMassifEnvironmentState& State = Cone->GetState();
	const FMassifChannelHandle SnowFraction = State.FindChannel(MassifEnvironmentChannels::SnowFraction());
	const FMassifChannelHandle Temperature = State.FindChannel(MassifEnvironmentChannels::MeanTemperature());

	TestTrue(
		FString::Printf(TEXT("The snow fraction stays inside 0-1 (%.3f to %.3f)"),
			State.GetChannelMinimum(SnowFraction), State.GetChannelMaximum(SnowFraction)),
		State.GetChannelMinimum(SnowFraction) >= 0.0 && State.GetChannelMaximum(SnowFraction) <= 1.0);

	const FVector2d Summit = FVector2d::ZeroVector;
	const FVector2d Foot(1900.0, 0.0);
	TestTrue(
		TEXT("The summit is the colder of the two"),
		State.SampleScalar(Temperature, Summit) < State.SampleScalar(Temperature, Foot));
	TestTrue(
		FString::Printf(TEXT("And it takes the greater share as snow (%.3f against %.3f)"),
			State.SampleScalar(SnowFraction, Summit), State.SampleScalar(SnowFraction, Foot)),
		State.SampleScalar(SnowFraction, Summit) > State.SampleScalar(SnowFraction, Foot));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPrecipitationOffTest,
	"Massif.WorldGen.Precipitation.Off",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPrecipitationOffTest::RunTest(const FString& Parameters)
{
	// Off is flat weather, not an absent channel: the same rain everywhere whatever
	// the ground does, so nothing downstream asks whether it rained.
	const TUniquePtr<FMassifEnvironmentScheduler> Flat =
		SolvePrecipitation(MakePrecipTerrain(EMassifAnalyticTerrain::Cone, 0.0, 1200.0),
			EMassifFidelity::Off, 90.0);
	const FMassifEnvironmentState& State = Flat->GetState();

	const FMassifChannelHandle Annual = State.FindChannel(MassifEnvironmentChannels::AnnualPrecipitation());
	const FMassifChannelHandle Snow = State.FindChannel(MassifEnvironmentChannels::SnowFraction());
	TestTrue(TEXT("Annual precipitation is written at Off"), State.IsChannelWritten(Annual));
	TestTrue(TEXT("The snow fraction is written at Off"), State.IsChannelWritten(Snow));

	const FMassifPrecipitationParams Defaults;
	TestEqual(
		TEXT("It rains the base amount at the summit"),
		State.SampleScalar(Annual, FVector2d::ZeroVector), Defaults.BaseAnnualMillimetres);
	TestEqual(
		TEXT("And the same on the flank, whatever the ground does"),
		State.SampleScalar(Annual, FVector2d(1400.0, 0.0)), Defaults.BaseAnnualMillimetres);
	TestEqual(TEXT("None of it is frozen"), State.GetChannelMaximum(Snow), 0.0);
	return true;
}
