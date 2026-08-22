#include "MassifElevationSource.h"
#include "MassifEnvironmentGraph.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifSunProcesses.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr uint32 SunTestSeed = 4711;
	constexpr double SunTestWorldMeters = 4000.0;
	constexpr double SunTestLatitudeDegrees = 47.2;

	/** Only the sun runs here: the water solve has nothing to say about shading and costs time. */
	TUniquePtr<FMassifEnvironmentScheduler> SolveSun(
		const FMassifAnalyticElevationSource::FParams& TerrainParams, EMassifFidelity Fidelity)
	{
		FMassifGeoreference Georeference;
		Georeference.LatitudeDegrees = SunTestLatitudeDegrees;

		TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MakeUnique<FMassifEnvironmentScheduler>(
			MakeShared<FMassifAnalyticElevationSource>(TerrainParams), Georeference, SunTestSeed);

		FMassifEnvironmentBakeParams WaterParams;
		WaterParams.CellSizeMeters = 32.0;
		MassifRegisterAltitudeProcess(*Scheduler, WaterParams);

		FMassifSunParams SunParams;
		SunParams.CellSizeMeters = 64.0;
		MassifRegisterSunProcesses(*Scheduler, SunParams);

		FString Error;
		checkf(Scheduler->BuildSchedule(Error), TEXT("%s"), *Error);
		Scheduler->SetFidelity(MassifEnvironmentProcesses::Insolation(), Fidelity);
		Scheduler->Execute();
		return Scheduler;
	}

	FMassifAnalyticElevationSource::FParams MakeTerrain(
		EMassifAnalyticTerrain Shape, double BearingDegrees, double ReliefMeters = 900.0)
	{
		FMassifAnalyticElevationSource::FParams Params;
		Params.Shape = Shape;
		Params.WorldSizeMeters = SunTestWorldMeters;
		Params.BaseAltitudeMeters = 1500.0;
		Params.ReliefMeters = ReliefMeters;
		Params.BearingDegrees = BearingDegrees;
		return Params;
	}

	/**
	 * Sky view a cone-shaped hollow leaves its own floor.
	 *
	 * The walls rise at the same angle in every direction, so the horizon is that
	 * one angle all the way round and the standard cos-squared form is exact.
	 */
	double ExpectedPitSkyViewFactor(double ReliefMeters)
	{
		const double WallAngleRadians = FMath::Atan(ReliefMeters / (0.5 * SunTestWorldMeters));
		return FMath::Square(FMath::Cos(WallAngleRadians));
	}

	double SampleChannel(
		const FMassifEnvironmentScheduler& Scheduler, const FName& ChannelName, FVector2d PositionMeters)
	{
		const FMassifEnvironmentState& State = Scheduler.GetState();
		return State.SampleScalar(State.FindChannel(ChannelName), PositionMeters);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSunFlatWorldTest,
	"Massif.WorldGen.Sun.FlatWorld",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSunFlatWorldTest::RunTest(const FString& Parameters)
{
	// Level open ground is what the channels are measured against, so it has to
	// come out as one on both counts or every reading elsewhere is scaled wrong.
	const TUniquePtr<FMassifEnvironmentScheduler> Flat =
		SolveSun(MakeTerrain(EMassifAnalyticTerrain::Flat, 0.0), EMassifFidelity::Standard);

	const double SkyView = SampleChannel(*Flat, MassifEnvironmentChannels::SkyViewFactor(), FVector2d::ZeroVector);
	const double Direct =
		SampleChannel(*Flat, MassifEnvironmentChannels::DirectInsolation(), FVector2d::ZeroVector);

	TestTrue(FString::Printf(TEXT("A flat world sees the whole sky (%.4f)"), SkyView), SkyView > 0.999);
	TestTrue(FString::Printf(TEXT("A flat world takes a full day of sun (%.4f)"), Direct), Direct > 0.999);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSunPitTest,
	"Massif.WorldGen.Sun.Pit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSunPitTest::RunTest(const FString& Parameters)
{
	// The floor of a hole has ground all around it above the horizontal, which is
	// exactly the case a local slope-and-aspect estimate cannot see at all.
	const TUniquePtr<FMassifEnvironmentScheduler> Pit =
		SolveSun(MakeTerrain(EMassifAnalyticTerrain::Pit, 0.0), EMassifFidelity::Standard);

	const double SkyView = SampleChannel(*Pit, MassifEnvironmentChannels::SkyViewFactor(), FVector2d::ZeroVector);
	const double Direct = SampleChannel(*Pit, MassifEnvironmentChannels::DirectInsolation(), FVector2d::ZeroVector);

	const double Expected = ExpectedPitSkyViewFactor(900.0);
	TestTrue(
		FString::Printf(TEXT("The pit floor's sky view matches the closed form (%.4f against %.4f)"),
			SkyView, Expected),
		FMath::Abs(SkyView - Expected) < 0.02);
	TestTrue(FString::Printf(TEXT("And it takes less sun than open ground (%.4f)"), Direct), Direct < 0.95);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSunRidgeAspectTest,
	"Massif.WorldGen.Sun.RidgeAspect",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSunRidgeAspectTest::RunTest(const FString& Parameters)
{
	// A ridge running east-west has a south flank and a north flank, and in the
	// northern hemisphere the south flank takes far more sun — which is what puts
	// the treeline higher on one side of a valley than the other. Steep, because at
	// 47 degrees north in July the noon sun is high and a gentle flank barely
	// separates from its opposite number.
	const TUniquePtr<FMassifEnvironmentScheduler> Ridge =
		SolveSun(MakeTerrain(EMassifAnalyticTerrain::Ridge, 90.0, 2400.0), EMassifFidelity::Standard);

	const double SouthFlank =
		SampleChannel(*Ridge, MassifEnvironmentChannels::DirectInsolation(), FVector2d(0.0, -900.0));
	const double NorthFlank =
		SampleChannel(*Ridge, MassifEnvironmentChannels::DirectInsolation(), FVector2d(0.0, 900.0));

	TestTrue(
		FString::Printf(TEXT("The south flank takes more sun than the north (%.3f against %.3f)"),
			SouthFlank, NorthFlank),
		SouthFlank > NorthFlank * 1.2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSunShadowingTest,
	"Massif.WorldGen.Sun.Shadowing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSunShadowingTest::RunTest(const FString& Parameters)
{
	// Cheap is the same cosine law without a horizon. Turning the sweep on may only
	// ever take light away, and in a pit it must take a visible amount.
	const FMassifAnalyticElevationSource::FParams Terrain =
		MakeTerrain(EMassifAnalyticTerrain::Pit, 0.0, 2400.0);
	const TUniquePtr<FMassifEnvironmentScheduler> Unshadowed = SolveSun(Terrain, EMassifFidelity::Cheap);
	const TUniquePtr<FMassifEnvironmentScheduler> Shadowed = SolveSun(Terrain, EMassifFidelity::Standard);

	const double CheapSkyView =
		SampleChannel(*Unshadowed, MassifEnvironmentChannels::SkyViewFactor(), FVector2d::ZeroVector);
	TestTrue(
		FString::Printf(TEXT("Without a sweep the sky is taken as open (%.4f)"), CheapSkyView),
		CheapSkyView > 0.999);

	const double CheapDirect =
		SampleChannel(*Unshadowed, MassifEnvironmentChannels::DirectInsolation(), FVector2d::ZeroVector);
	const double ShadowedDirect =
		SampleChannel(*Shadowed, MassifEnvironmentChannels::DirectInsolation(), FVector2d::ZeroVector);
	TestTrue(
		FString::Printf(TEXT("Shadowing only ever removes light (%.4f against %.4f)"),
			ShadowedDirect, CheapDirect),
		ShadowedDirect < CheapDirect);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSunOffTest,
	"Massif.WorldGen.Sun.Off",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSunOffTest::RunTest(const FString& Parameters)
{
	// Off still writes the channels, so nothing downstream has to ask whether the
	// sun was solved before reading it.
	const TUniquePtr<FMassifEnvironmentScheduler> Unlit =
		SolveSun(MakeTerrain(EMassifAnalyticTerrain::Pit, 0.0), EMassifFidelity::Off);
	const FMassifEnvironmentState& State = Unlit->GetState();

	const FName Names[] = {
		MassifEnvironmentChannels::SkyViewFactor(), MassifEnvironmentChannels::DirectInsolation(),
		MassifEnvironmentChannels::DiffuseInsolation()};
	for (const FName& Name : Names)
	{
		const FMassifChannelHandle Channel = State.FindChannel(Name);
		TestTrue(FString::Printf(TEXT("%s exists at Off"), *Name.ToString()), Channel.IsValid());
		TestTrue(
			FString::Printf(TEXT("%s is written at Off"), *Name.ToString()), State.IsChannelWritten(Channel));
		TestEqual(
			FString::Printf(TEXT("%s reads as open sky at Off"), *Name.ToString()),
			State.SampleScalar(Channel, FVector2d::ZeroVector), 1.0);
	}
	return true;
}
