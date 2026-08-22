#include "MassifElevationSource.h"
#include "MassifEnvironmentGraph.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifWindProcesses.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr uint32 WindTestSeed = 8123;

	/**
	 * A whole number of cells of both grids, so a quarter turn maps every cell
	 * centre onto another cell centre and the rotation test compares like with
	 * like rather than two different discretisations.
	 */
	constexpr double WindTestWorldMeters = 4096.0;
	constexpr double WindTestAltitudeCellMeters = 32.0;
	constexpr double WindTestCellMeters = 64.0;
	constexpr double WindTestReliefMeters = 900.0;

	/** Only the wind runs here: the water solve has nothing to say about it and costs time. */
	TUniquePtr<FMassifEnvironmentScheduler> SolveWind(
		const FMassifAnalyticElevationSource::FParams& TerrainParams, const FMassifWindParams& WindParams,
		EMassifFidelity Fidelity, uint32 Seed = WindTestSeed)
	{
		TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MakeUnique<FMassifEnvironmentScheduler>(
			MakeShared<FMassifAnalyticElevationSource>(TerrainParams), FMassifGeoreference(), Seed);

		FMassifEnvironmentBakeParams AltitudeParams;
		AltitudeParams.CellSizeMeters = WindTestAltitudeCellMeters;
		MassifRegisterAltitudeProcess(*Scheduler, AltitudeParams);
		MassifRegisterWindProcesses(*Scheduler, WindParams);

		FString Error;
		checkf(Scheduler->BuildSchedule(Error), TEXT("%s"), *Error);
		Scheduler->SetFidelity(MassifEnvironmentProcesses::Wind(), Fidelity);
		Scheduler->Execute();
		return Scheduler;
	}

	FMassifWindParams MakeWindParams(double SynopticBearingDegrees)
	{
		FMassifWindParams Params;
		Params.CellSizeMeters = WindTestCellMeters;
		Params.SynopticBearingDegrees = SynopticBearingDegrees;
		return Params;
	}

	FMassifAnalyticElevationSource::FParams MakeWindTerrain(EMassifAnalyticTerrain Shape, double BearingDegrees)
	{
		FMassifAnalyticElevationSource::FParams Params;
		Params.Shape = Shape;
		Params.WorldSizeMeters = WindTestWorldMeters;
		Params.BaseAltitudeMeters = 1500.0;
		Params.ReliefMeters = WindTestReliefMeters;
		Params.BearingDegrees = BearingDegrees;
		return Params;
	}

	FVector2d CompassDirection(double BearingDegrees)
	{
		const double BearingRadians = FMath::DegreesToRadians(BearingDegrees);
		return FVector2d(FMath::Sin(BearingRadians), FMath::Cos(BearingRadians));
	}

	/** Where the air goes: the bearing names where it comes from. */
	FVector2d FlowDirection(double SynopticBearingDegrees)
	{
		return -CompassDirection(SynopticBearingDegrees);
	}

	/** Across the shape's own axis, the direction a ridge falls away in. */
	FVector2d AcrossDirection(double TerrainBearingDegrees)
	{
		const FVector2d Along = CompassDirection(TerrainBearingDegrees);
		return FVector2d(Along.Y, -Along.X);
	}

	/**
	 * A point on the flank the flow climbs, so a reading there is about how the
	 * terrain steers the wind and not about the pocket in the lee.
	 */
	FVector2d WindwardFlankPosition(
		double TerrainBearingDegrees, double SynopticBearingDegrees, double DistanceMeters)
	{
		const FVector2d Across = AcrossDirection(TerrainBearingDegrees);
		if (FVector2d::DotProduct(Across, FlowDirection(SynopticBearingDegrees)) > 0.0)
		{
			return -Across * DistanceMeters;
		}
		return Across * DistanceMeters;
	}

	/** The turn a 90 degree change of compass bearing is, applied to a position. */
	FVector2d QuarterTurn(FVector2d PositionMeters)
	{
		return FVector2d(PositionMeters.Y, -PositionMeters.X);
	}

	/** Angle between a direction and a terrain axis. An axis is a line, so its sign carries nothing. */
	double AngleToAxisDegrees(FVector2d Direction, FVector2d Axis)
	{
		const double Cosine = FVector2d::DotProduct(Direction.GetSafeNormal(), Axis.GetSafeNormal());
		return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FMath::Abs(Cosine), 0.0, 1.0)));
	}

	double SampleExposure(const FMassifEnvironmentScheduler& Scheduler, FVector2d PositionMeters)
	{
		const FMassifEnvironmentState& State = Scheduler.GetState();
		return State.SampleScalar(State.FindChannel(MassifEnvironmentChannels::WindExposure()), PositionMeters);
	}

	FVector2d SampleField(const FMassifEnvironmentScheduler& Scheduler, FVector2d PositionMeters)
	{
		const FMassifEnvironmentState& State = Scheduler.GetState();
		return State.SampleVector(State.FindChannel(MassifEnvironmentChannels::WindField()), PositionMeters);
	}

	/** How much of the field runs the way the synoptic wind drives it. Negative is reversed flow. */
	double AlongWindSpeed(FVector2d FieldMetersPerSecond, double SynopticBearingDegrees)
	{
		return FVector2d::DotProduct(FieldMetersPerSecond, FlowDirection(SynopticBearingDegrees));
	}

	/** What one step of the exposure channel's uint16 encoding is worth, over the range it committed. */
	double ExposureQuantisationStep(const FMassifEnvironmentScheduler& Scheduler)
	{
		const FMassifEnvironmentState& State = Scheduler.GetState();
		const FMassifChannelHandle Exposure = State.FindChannel(MassifEnvironmentChannels::WindExposure());
		return (State.GetChannelMaximum(Exposure) - State.GetChannelMinimum(Exposure)) / 65535.0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWindCrestTest,
	"Massif.WorldGen.Wind.Crest",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWindCrestTest::RunTest(const FString& Parameters)
{
	// A crest faces into an open fetch that only ever descends, so the flow is
	// compressed over it and it stands above what level ground takes. The analytic
	// ridge falls all the way to the world edge, so the level ground it is measured
	// against is a flat world under the same wind.
	const FMassifWindParams WindParams = MakeWindParams(0.0);
	const TUniquePtr<FMassifEnvironmentScheduler> Ridge = SolveWind(
		MakeWindTerrain(EMassifAnalyticTerrain::Ridge, 90.0), WindParams, EMassifFidelity::Standard);
	const TUniquePtr<FMassifEnvironmentScheduler> Flat = SolveWind(
		MakeWindTerrain(EMassifAnalyticTerrain::Flat, 90.0), WindParams, EMassifFidelity::Standard);

	// The crest line runs between two rows of cells, so the crest cell centre sits
	// half a cell to the windward side of it.
	const FVector2d CrestPosition(0.0, 0.5 * WindTestCellMeters);
	const double CrestExposure = SampleExposure(*Ridge, CrestPosition);
	const double LevelExposure = SampleExposure(*Flat, CrestPosition);
	const double CrestSpeed = SampleField(*Ridge, CrestPosition).Length();
	const double LevelSpeed = SampleField(*Flat, CrestPosition).Length();

	TestTrue(
		FString::Printf(TEXT("The crest is more exposed than level ground (%.3f against %.3f)"),
			CrestExposure, LevelExposure),
		CrestExposure > LevelExposure * 1.2);
	TestTrue(
		FString::Printf(TEXT("And the wind over it is stronger (%.3f against %.3f m/s)"), CrestSpeed, LevelSpeed),
		CrestSpeed > LevelSpeed * 1.2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWindLeePocketTest,
	"Massif.WorldGen.Wind.LeePocket",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWindLeePocketTest::RunTest(const FString& Parameters)
{
	// With the wind across the ridge, one flank faces the fetch and the other lies
	// behind the crest. The lee flank is not merely weaker: the along-wind flow
	// there collapses as the air separates from ground that falls away under it,
	// which is what fills that side with drift instead of scouring it.
	constexpr double SynopticBearing = 0.0;
	const FMassifWindParams WindParams = MakeWindParams(SynopticBearing);
	const TUniquePtr<FMassifEnvironmentScheduler> Ridge = SolveWind(
		MakeWindTerrain(EMassifAnalyticTerrain::Ridge, 90.0), WindParams, EMassifFidelity::Standard);
	const TUniquePtr<FMassifEnvironmentScheduler> Unsteered = SolveWind(
		MakeWindTerrain(EMassifAnalyticTerrain::Ridge, 90.0), WindParams, EMassifFidelity::Cheap);

	const FVector2d WindwardPosition(0.0, 800.0);
	const FVector2d LeePosition(0.0, -800.0);

	const double WindwardSpeed = SampleField(*Ridge, WindwardPosition).Length();
	const double LeeSpeed = SampleField(*Ridge, LeePosition).Length();
	TestTrue(
		FString::Printf(TEXT("The lee pocket is slower than the windward flank (%.3f against %.3f m/s)"),
			LeeSpeed, WindwardSpeed),
		LeeSpeed < WindwardSpeed * 0.5);

	const double WindwardAlong = AlongWindSpeed(SampleField(*Ridge, WindwardPosition), SynopticBearing);
	const double LeeAlong = AlongWindSpeed(SampleField(*Ridge, LeePosition), SynopticBearing);
	TestTrue(
		FString::Printf(TEXT("And carries far less of the wind's own direction (%.3f against %.3f m/s)"),
			LeeAlong, WindwardAlong),
		LeeAlong < WindwardAlong * 0.25);

	// Shelter alone is already in the Cheap field. Anything beyond it in the lee is
	// the separation term reversing part of the flow.
	const double ShelteredAlong = AlongWindSpeed(SampleField(*Unsteered, LeePosition), SynopticBearing);
	TestTrue(
		FString::Printf(TEXT("Separation takes more along-wind flow than shelter alone (%.3f against %.3f m/s)"),
			LeeAlong, ShelteredAlong),
		LeeAlong < ShelteredAlong * 0.5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWindChannelingTest,
	"Massif.WorldGen.Wind.Channeling",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWindChannelingTest::RunTest(const FString& Parameters)
{
	// Terrain steers the flow towards the axis it is confined along. A ridge whose
	// axis lies near the synoptic bearing pulls the wind round onto that axis; one
	// lying across the bearing cannot, whatever it does to the speed.
	constexpr double SynopticBearing = 60.0;
	constexpr double AlignedTerrainBearing = 30.0;
	constexpr double CrossTerrainBearing = 150.0;
	constexpr double FlankDistanceMeters = 500.0;

	const FMassifWindParams WindParams = MakeWindParams(SynopticBearing);
	const TUniquePtr<FMassifEnvironmentScheduler> Aligned = SolveWind(
		MakeWindTerrain(EMassifAnalyticTerrain::Ridge, AlignedTerrainBearing), WindParams, EMassifFidelity::Standard);
	const TUniquePtr<FMassifEnvironmentScheduler> Unsteered = SolveWind(
		MakeWindTerrain(EMassifAnalyticTerrain::Ridge, AlignedTerrainBearing), WindParams, EMassifFidelity::Cheap);
	const TUniquePtr<FMassifEnvironmentScheduler> Crossing = SolveWind(
		MakeWindTerrain(EMassifAnalyticTerrain::Ridge, CrossTerrainBearing), WindParams, EMassifFidelity::Standard);

	const FVector2d AlignedPosition =
		WindwardFlankPosition(AlignedTerrainBearing, SynopticBearing, FlankDistanceMeters);
	const double SteeredAngle = AngleToAxisDegrees(
		SampleField(*Aligned, AlignedPosition), CompassDirection(AlignedTerrainBearing));
	const double SynopticAngle = AngleToAxisDegrees(
		SampleField(*Unsteered, AlignedPosition), CompassDirection(AlignedTerrainBearing));
	TestTrue(
		FString::Printf(TEXT("Channeling pulls the flow onto the valley axis (%.1f against %.1f degrees)"),
			SteeredAngle, SynopticAngle),
		SteeredAngle < SynopticAngle - 5.0);

	const FVector2d CrossPosition =
		WindwardFlankPosition(CrossTerrainBearing, SynopticBearing, FlankDistanceMeters);
	const double CrossAngle = AngleToAxisDegrees(
		SampleField(*Crossing, CrossPosition), CompassDirection(CrossTerrainBearing));
	TestTrue(
		FString::Printf(TEXT("An axis across the wind does not capture the flow (%.1f degrees)"), CrossAngle),
		CrossAngle > 45.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWindRotationTest,
	"Massif.WorldGen.Wind.Rotation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWindRotationTest::RunTest(const FString& Parameters)
{
	// Turning the terrain and the wind together by the same angle is not a physical
	// change, so nothing about the reading may move with it. A quarter turn keeps
	// every cell centre on a cell centre, which leaves the encoding as the only
	// thing between the two readings — and catches a frame error in the upwind
	// sweep, which is the easiest thing here to get wrong.
	constexpr double TerrainBearing = 20.0;
	constexpr double SynopticBearing = 55.0;
	constexpr double QuarterTurnDegrees = 90.0;
	const FVector2d SamplePosition(700.0, -300.0);

	const TUniquePtr<FMassifEnvironmentScheduler> Upright = SolveWind(
		MakeWindTerrain(EMassifAnalyticTerrain::Ridge, TerrainBearing), MakeWindParams(SynopticBearing),
		EMassifFidelity::Standard);
	const TUniquePtr<FMassifEnvironmentScheduler> Turned = SolveWind(
		MakeWindTerrain(EMassifAnalyticTerrain::Ridge, TerrainBearing + QuarterTurnDegrees),
		MakeWindParams(SynopticBearing + QuarterTurnDegrees), EMassifFidelity::Standard);

	const double UprightExposure = SampleExposure(*Upright, SamplePosition);
	const double TurnedExposure = SampleExposure(*Turned, QuarterTurn(SamplePosition));

	// One step for the quantisation, one for a value sitting on a step boundary.
	const double Tolerance = 2.0 * ExposureQuantisationStep(*Upright);
	TestTrue(
		FString::Printf(TEXT("Exposure survives the turn (%.6f against %.6f, tolerance %.6f)"),
			UprightExposure, TurnedExposure, Tolerance),
		FMath::Abs(UprightExposure - TurnedExposure) <= Tolerance);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWindOffTest,
	"Massif.WorldGen.Wind.Off",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWindOffTest::RunTest(const FString& Parameters)
{
	// Off still writes both channels, so nothing downstream has to ask whether the
	// wind was solved before reading it: the synoptic wind everywhere and neutral
	// exposure, with the terrain ignored.
	constexpr double SynopticBearing = 0.0;
	const FMassifWindParams WindParams = MakeWindParams(SynopticBearing);
	const TUniquePtr<FMassifEnvironmentScheduler> Uniform = SolveWind(
		MakeWindTerrain(EMassifAnalyticTerrain::Ridge, 90.0), WindParams, EMassifFidelity::Off);
	const FMassifEnvironmentState& State = Uniform->GetState();

	const FName Names[] = {MassifEnvironmentChannels::WindExposure(), MassifEnvironmentChannels::WindField()};
	for (const FName& Name : Names)
	{
		const FMassifChannelHandle Channel = State.FindChannel(Name);
		TestTrue(FString::Printf(TEXT("%s exists at Off"), *Name.ToString()), Channel.IsValid());
		TestTrue(FString::Printf(TEXT("%s is written at Off"), *Name.ToString()), State.IsChannelWritten(Channel));
	}

	const FVector2d CrestPosition(0.0, 0.5 * WindTestCellMeters);
	TestEqual(TEXT("Exposure is neutral at Off"), SampleExposure(*Uniform, CrestPosition), 1.0);

	const FVector2d Expected = FlowDirection(SynopticBearing) * WindParams.SynopticSpeedMetersPerSecond;
	const FVector2d Field = SampleField(*Uniform, CrestPosition);
	TestEqual(
		TEXT("The field runs east as the synoptic wind does at Off"), Field.X, Expected.X,
		UE_DOUBLE_KINDA_SMALL_NUMBER);
	TestEqual(
		TEXT("The field runs north as the synoptic wind does at Off"), Field.Y, Expected.Y,
		UE_DOUBLE_KINDA_SMALL_NUMBER);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWindDeterminismTest,
	"Massif.WorldGen.Wind.Determinism",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWindDeterminismTest::RunTest(const FString& Parameters)
{
	// Left unpinned, the prevailing wind comes out of the seeded stream: the same
	// seed has to give the same world, and a different seed a different one, or the
	// bearing is not coming from the seed at all.
	const FMassifAnalyticElevationSource::FParams Terrain = MakeWindTerrain(EMassifAnalyticTerrain::Cone, 0.0);

	FMassifWindParams WindParams;
	WindParams.CellSizeMeters = WindTestCellMeters;

	const uint64 FirstHash =
		SolveWind(Terrain, WindParams, EMassifFidelity::Standard)->GetState().ComputeContentHash();
	const uint64 SecondHash =
		SolveWind(Terrain, WindParams, EMassifFidelity::Standard)->GetState().ComputeContentHash();
	const uint64 OtherSeedHash =
		SolveWind(Terrain, WindParams, EMassifFidelity::Standard, WindTestSeed + 1)
			->GetState().ComputeContentHash();

	TestEqual(TEXT("The same seed blows the same wind twice"), FirstHash, SecondHash);
	TestNotEqual(TEXT("A different seed turns the prevailing wind"), FirstHash, OtherSeedHash);
	return true;
}
