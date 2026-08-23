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

	/** Gentle enough that ground the whole flank drains across still holds some of it. */
	constexpr double WindTestDrainageReliefMeters = 300.0;

	/** Steep flanks, but short of the slope at which drained air stops settling at all. */
	constexpr double WindTestGullyReliefMeters = 800.0;

	/** Steeper than drainage tolerates, so within the hollow only its floor holds anything. */
	constexpr double WindTestHollowReliefMeters = 1100.0;

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

	FMassifAnalyticElevationSource::FParams MakeWindTerrain(
		EMassifAnalyticTerrain Shape, double BearingDegrees, double ReliefMeters = WindTestReliefMeters)
	{
		FMassifAnalyticElevationSource::FParams Params;
		Params.Shape = Shape;
		Params.WorldSizeMeters = WindTestWorldMeters;
		Params.BaseAltitudeMeters = 1500.0;
		Params.ReliefMeters = ReliefMeters;
		Params.BearingDegrees = BearingDegrees;
		return Params;
	}

	/**
	 * The whole standard set, so the wind reads the catchment the water solve
	 * actually routed rather than one this test made up.
	 */
	TUniquePtr<FMassifEnvironmentScheduler> SolveWindWithHydrology(
		const FMassifAnalyticElevationSource::FParams& TerrainParams, double SynopticBearingDegrees)
	{
		TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MakeUnique<FMassifEnvironmentScheduler>(
			MakeShared<FMassifAnalyticElevationSource>(TerrainParams), FMassifGeoreference(), WindTestSeed);

		FMassifEnvironmentParams Params;
		Params.Water.CellSizeMeters = WindTestAltitudeCellMeters;

		// Soil has nothing to say about the wind, and the droplets are the expensive part.
		Params.Water.DropletsPerCell = 0.05;
		Params.Water.MaxDropletSteps = 8;
		Params.Wind = MakeWindParams(SynopticBearingDegrees);
		MassifRegisterEnvironmentProcesses(*Scheduler, Params);

		FString Error;
		checkf(Scheduler->BuildSchedule(Error), TEXT("%s"), *Error);
		Scheduler->Execute();
		return Scheduler;
	}

	/**
	 * A flow accumulation channel reading the same everywhere.
	 *
	 * Standing in for the water solve, so a comparison between two points is a
	 * comparison of the ground under them and of nothing else: the real routing
	 * concentrates flow wherever the terrain sends it, which is exactly the
	 * variable that has to be held still here.
	 */
	class FUniformFlowAccumulationProcess final : public IMassifEnvironmentProcess
	{
	public:
		explicit FUniformFlowAccumulationProcess(double InSquareMeters)
			: SquareMeters(InSquareMeters)
		{
		}

		FMassifProcessDeclaration Declare() const override
		{
			FMassifChannelDescriptor Descriptor;
			Descriptor.Name = MassifEnvironmentChannels::FlowAccumulation();
			Descriptor.Type = EMassifChannelType::Scalar;
			Descriptor.Encoding = EMassifChannelEncoding::Float32;
			Descriptor.CellSizeMeters = WindTestAltitudeCellMeters;
			Descriptor.Unit = TEXT("m2");

			FMassifProcessDeclaration Declaration;
			Declaration.Name = TEXT("Test.UniformFlowAccumulation");
			Declaration.SupportedFidelities = {EMassifFidelity::Standard};
			Declaration.OutputChannels.Add(Descriptor);
			return Declaration;
		}

		void Execute(FMassifEnvironmentContext& Context, EMassifFidelity) const override
		{
			const TUniquePtr<FMassifChannelWriteScope> Scope =
				Context.OpenOutput(MassifEnvironmentChannels::FlowAccumulation());
			TArrayView<double> Values = Scope->GetComponent(0);
			for (int32 CellIndex = 0; CellIndex < Values.Num(); ++CellIndex)
			{
				Values[CellIndex] = SquareMeters;
			}
			Scope->Commit();
		}

	private:
		double SquareMeters = 0.0;
	};

	TUniquePtr<FMassifEnvironmentScheduler> SolveWindWithUniformDrainage(
		const FMassifAnalyticElevationSource::FParams& TerrainParams, const FMassifWindParams& WindParams,
		double AccumulationSquareMeters)
	{
		TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MakeUnique<FMassifEnvironmentScheduler>(
			MakeShared<FMassifAnalyticElevationSource>(TerrainParams), FMassifGeoreference(), WindTestSeed);

		FMassifEnvironmentBakeParams AltitudeParams;
		AltitudeParams.CellSizeMeters = WindTestAltitudeCellMeters;
		MassifRegisterAltitudeProcess(*Scheduler, AltitudeParams);
		Scheduler->RegisterProcess(MakeShared<FUniformFlowAccumulationProcess>(AccumulationSquareMeters));
		MassifRegisterWindProcesses(*Scheduler, WindParams);

		FString Error;
		checkf(Scheduler->BuildSchedule(Error), TEXT("%s"), *Error);
		Scheduler->Execute();
		return Scheduler;
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

	double SampleScalarChannel(
		const FMassifEnvironmentScheduler& Scheduler, const FName& ChannelName, FVector2d PositionMeters)
	{
		const FMassifEnvironmentState& State = Scheduler.GetState();
		return State.SampleScalar(State.FindChannel(ChannelName), PositionMeters);
	}

	double SampleExposure(const FMassifEnvironmentScheduler& Scheduler, FVector2d PositionMeters)
	{
		return SampleScalarChannel(Scheduler, MassifEnvironmentChannels::WindExposure(), PositionMeters);
	}

	double SampleKatabatic(const FMassifEnvironmentScheduler& Scheduler, FVector2d PositionMeters)
	{
		return SampleScalarChannel(Scheduler, MassifEnvironmentChannels::KatabaticStrength(), PositionMeters);
	}

	double SampleFoehn(const FMassifEnvironmentScheduler& Scheduler, FVector2d PositionMeters)
	{
		return SampleScalarChannel(Scheduler, MassifEnvironmentChannels::FoehnWarming(), PositionMeters);
	}

	/** The slope the solve reads, differenced across one wind cell of the altitude channel. */
	double SampleSlopeTangent(const FMassifEnvironmentScheduler& Scheduler, FVector2d PositionMeters)
	{
		const FVector2d East(WindTestCellMeters, 0.0);
		const FVector2d North(0.0, WindTestCellMeters);
		const FName& Altitude = MassifEnvironmentChannels::Altitude();

		const double Eastward = SampleScalarChannel(Scheduler, Altitude, PositionMeters + East)
			- SampleScalarChannel(Scheduler, Altitude, PositionMeters - East);
		const double Northward = SampleScalarChannel(Scheduler, Altitude, PositionMeters + North)
			- SampleScalarChannel(Scheduler, Altitude, PositionMeters - North);
		return FVector2d(Eastward, Northward).Length() / (2.0 * WindTestCellMeters);
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

	/** What one step of a channel's uint16 encoding is worth, over the range it committed. */
	double QuantisationStep(const FMassifEnvironmentScheduler& Scheduler, const FName& ChannelName)
	{
		const FMassifEnvironmentState& State = Scheduler.GetState();
		const FMassifChannelHandle Channel = State.FindChannel(ChannelName);
		return (State.GetChannelMaximum(Channel) - State.GetChannelMinimum(Channel)) / 65535.0;
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
	const double Tolerance = 2.0 * QuantisationStep(*Upright, MassifEnvironmentChannels::WindExposure());
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
	// Off still writes every channel, so nothing downstream has to ask whether the
	// wind was solved before reading it: the synoptic wind everywhere and neutral
	// exposure, with the terrain ignored. Terrain is the whole of what drives the
	// drainage and the foehn, so both of those read nothing at all.
	constexpr double SynopticBearing = 0.0;
	const FMassifWindParams WindParams = MakeWindParams(SynopticBearing);
	const TUniquePtr<FMassifEnvironmentScheduler> Uniform = SolveWind(
		MakeWindTerrain(EMassifAnalyticTerrain::Ridge, 90.0), WindParams, EMassifFidelity::Off);
	const FMassifEnvironmentState& State = Uniform->GetState();

	const FName Names[] = {
		MassifEnvironmentChannels::WindExposure(), MassifEnvironmentChannels::WindField(),
		MassifEnvironmentChannels::KatabaticStrength(), MassifEnvironmentChannels::FoehnWarming()};
	for (const FName& Name : Names)
	{
		const FMassifChannelHandle Channel = State.FindChannel(Name);
		TestTrue(FString::Printf(TEXT("%s exists at Off"), *Name.ToString()), Channel.IsValid());
		TestTrue(FString::Printf(TEXT("%s is written at Off"), *Name.ToString()), State.IsChannelWritten(Channel));
	}

	const FName ZeroedNames[] = {
		MassifEnvironmentChannels::KatabaticStrength(), MassifEnvironmentChannels::FoehnWarming()};
	for (const FName& Name : ZeroedNames)
	{
		const FMassifChannelHandle Channel = State.FindChannel(Name);
		TestEqual(FString::Printf(TEXT("%s bottoms out at zero at Off"), *Name.ToString()),
			State.GetChannelMinimum(Channel), 0.0, UE_DOUBLE_KINDA_SMALL_NUMBER);
		TestEqual(FString::Printf(TEXT("%s tops out at zero at Off"), *Name.ToString()),
			State.GetChannelMaximum(Channel), 0.0, UE_DOUBLE_KINDA_SMALL_NUMBER);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWindKatabaticDrainageTest,
	"Massif.WorldGen.Wind.KatabaticDrainage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWindKatabaticDrainageTest::RunTest(const FString& Parameters)
{
	// Cold air runs downhill and collects. The foot of a peak has the whole flank
	// above it draining through; the summit has nothing above it at all, which no
	// local reading of the ground could tell apart from the foot on its own.
	//
	// The wind grid has no cell on the world centre, so the summit reading is taken
	// at the cell centre nearest to it.
	constexpr double SynopticBearing = 0.0;
	const FVector2d SummitPosition(32.0, 32.0);
	const FVector2d FootPosition(1824.0, 32.0);

	const TUniquePtr<FMassifEnvironmentScheduler> Peak = SolveWindWithHydrology(
		MakeWindTerrain(EMassifAnalyticTerrain::Cone, 0.0, WindTestDrainageReliefMeters), SynopticBearing);

	const double SummitStrength = SampleKatabatic(*Peak, SummitPosition);
	const double FootStrength = SampleKatabatic(*Peak, FootPosition);
	TestTrue(
		FString::Printf(TEXT("Drainage collects at the foot, not on the summit (%.4f against %.4f)"),
			FootStrength, SummitStrength),
		FootStrength > 5.0 * SummitStrength);

	// A closed hollow is the frost hollow case: its flanks are steep enough that the
	// air they collect is still moving, and only the floor is flat enough to hold
	// what arrives. Read along a transect from the floor out to the rim rather than
	// at two chosen points, so nothing is being hand-picked.
	const TUniquePtr<FMassifEnvironmentScheduler> Hollow = SolveWindWithHydrology(
		MakeWindTerrain(EMassifAnalyticTerrain::Pit, 0.0, WindTestHollowReliefMeters), SynopticBearing);

	const FVector2d FloorPosition(32.0, 32.0);
	const double FloorStrength = SampleKatabatic(*Hollow, FloorPosition);
	TestTrue(
		FString::Printf(TEXT("The hollow's floor holds cold air (%.4f)"), FloorStrength), FloorStrength > 0.0);

	double HighestOnTheFlank = 0.0;
	for (int32 CellStep = 1; CellStep <= 30; ++CellStep)
	{
		const FVector2d FlankPosition(FloorPosition.X + CellStep * WindTestCellMeters, FloorPosition.Y);
		HighestOnTheFlank = FMath::Max(HighestOnTheFlank, SampleKatabatic(*Hollow, FlankPosition));
	}

	TestTrue(
		FString::Printf(TEXT("And holds more than anywhere on the flank above it (%.4f against %.4f)"),
			FloorStrength, HighestOnTheFlank),
		FloorStrength > HighestOnTheFlank);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWindKatabaticSlopeTest,
	"Massif.WorldGen.Wind.KatabaticSlope",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWindKatabaticSlopeTest::RunTest(const FString& Parameters)
{
	// The same ground draining through two points and only the slope under them
	// different: a gully has the air moving through it, and the flat below has it
	// sitting. The accumulation is uniform by construction, so the reading cannot
	// be coming from where the water solve happened to concentrate.
	constexpr double SynopticBearing = 0.0;
	constexpr double UniformAccumulationSquareMeters = 125000.0;

	// The cone's flank, and the corner of the world box the shape has already
	// levelled off in.
	const FVector2d FlankPosition(1824.0, 32.0);
	const FVector2d BasinPosition(1824.0, 1824.0);

	const TUniquePtr<FMassifEnvironmentScheduler> Peak = SolveWindWithUniformDrainage(
		MakeWindTerrain(EMassifAnalyticTerrain::Cone, 0.0, WindTestGullyReliefMeters),
		MakeWindParams(SynopticBearing), UniformAccumulationSquareMeters);

	const double FlankAccumulation =
		SampleScalarChannel(*Peak, MassifEnvironmentChannels::FlowAccumulation(), FlankPosition);
	const double BasinAccumulation =
		SampleScalarChannel(*Peak, MassifEnvironmentChannels::FlowAccumulation(), BasinPosition);
	TestEqual(
		TEXT("The two points have the same catchment above them"), FlankAccumulation, BasinAccumulation,
		UniformAccumulationSquareMeters * UE_DOUBLE_KINDA_SMALL_NUMBER);

	// Which of the two is the steeper comes out of the terrain channel, so the
	// assertion does not rest on the names given above.
	FVector2d SteeperPosition = BasinPosition;
	FVector2d GentlerPosition = FlankPosition;
	if (SampleSlopeTangent(*Peak, FlankPosition) > SampleSlopeTangent(*Peak, BasinPosition))
	{
		SteeperPosition = FlankPosition;
		GentlerPosition = BasinPosition;
	}

	const double SteeperStrength = SampleKatabatic(*Peak, SteeperPosition);
	const double GentlerStrength = SampleKatabatic(*Peak, GentlerPosition);
	TestTrue(
		FString::Printf(TEXT("Steeper ground pools less than flat ground does (%.4f against %.4f)"),
			SteeperStrength, GentlerStrength),
		SteeperStrength < GentlerStrength);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWindWithoutHydrologyTest,
	"Massif.WorldGen.Wind.WithoutHydrology",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWindWithoutHydrologyTest::RunTest(const FString& Parameters)
{
	// The catchment is an optional input, so a graph with no water solve in it is a
	// valid graph and not a broken one: the wind still solves, every channel is
	// still written, and nothing drains anywhere because nothing has said what
	// drains where.
	const TUniquePtr<FMassifEnvironmentScheduler> Ridge = SolveWind(
		MakeWindTerrain(EMassifAnalyticTerrain::Ridge, 90.0), MakeWindParams(0.0), EMassifFidelity::Standard);
	const FMassifEnvironmentState& State = Ridge->GetState();

	TestFalse(
		TEXT("Nothing in this graph produces the flow accumulation"),
		State.FindChannel(MassifEnvironmentChannels::FlowAccumulation()).IsValid());

	const FName Names[] = {
		MassifEnvironmentChannels::KatabaticStrength(), MassifEnvironmentChannels::FoehnWarming()};
	for (const FName& Name : Names)
	{
		const FMassifChannelHandle Channel = State.FindChannel(Name);
		TestTrue(FString::Printf(TEXT("%s exists without hydrology"), *Name.ToString()), Channel.IsValid());
		TestTrue(
			FString::Printf(TEXT("%s is written without hydrology"), *Name.ToString()),
			State.IsChannelWritten(Channel));
	}

	const FMassifChannelHandle Katabatic = State.FindChannel(MassifEnvironmentChannels::KatabaticStrength());
	TestEqual(TEXT("Nothing drains anywhere, at the lowest"), State.GetChannelMinimum(Katabatic), 0.0);
	TestEqual(TEXT("Nothing drains anywhere, at the highest"), State.GetChannelMaximum(Katabatic), 0.0);

	// The foehn needs no water, so it is still solved over the same ridge.
	const FMassifChannelHandle Foehn = State.FindChannel(MassifEnvironmentChannels::FoehnWarming());
	TestTrue(TEXT("The foehn is solved regardless"), State.GetChannelMaximum(Foehn) > 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWindCheapWithoutTerrainTest,
	"Massif.WorldGen.Wind.CheapWithoutTerrain",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWindCheapWithoutTerrainTest::RunTest(const FString& Parameters)
{
	// Cheap is the level at which the terrain does not steer the flow. Drainage and
	// foehn are nothing but terrain, so at Cheap they are not weaker readings of the
	// same thing - there is no reading at all.
	const TUniquePtr<FMassifEnvironmentScheduler> Ridge = SolveWind(
		MakeWindTerrain(EMassifAnalyticTerrain::Ridge, 90.0), MakeWindParams(0.0), EMassifFidelity::Cheap);
	const FMassifEnvironmentState& State = Ridge->GetState();

	const FName Names[] = {
		MassifEnvironmentChannels::KatabaticStrength(), MassifEnvironmentChannels::FoehnWarming()};
	for (const FName& Name : Names)
	{
		const FMassifChannelHandle Channel = State.FindChannel(Name);
		TestTrue(FString::Printf(TEXT("%s is written at Cheap"), *Name.ToString()), State.IsChannelWritten(Channel));
		TestEqual(FString::Printf(TEXT("%s bottoms out at zero at Cheap"), *Name.ToString()),
			State.GetChannelMinimum(Channel), 0.0, UE_DOUBLE_KINDA_SMALL_NUMBER);
		TestEqual(FString::Printf(TEXT("%s tops out at zero at Cheap"), *Name.ToString()),
			State.GetChannelMaximum(Channel), 0.0, UE_DOUBLE_KINDA_SMALL_NUMBER);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWindFoehnLeeTest,
	"Massif.WorldGen.Wind.FoehnLee",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWindFoehnLeeTest::RunTest(const FString& Parameters)
{
	// Only air that has come down off something arrives warm. With the wind across
	// the ridge, one flank is what the flow climbs and the other is what it
	// descends onto, and the difference between them is the whole effect.
	constexpr double SynopticBearing = 0.0;
	const TUniquePtr<FMassifEnvironmentScheduler> Ridge = SolveWind(
		MakeWindTerrain(EMassifAnalyticTerrain::Ridge, 90.0), MakeWindParams(SynopticBearing),
		EMassifFidelity::Standard);

	const double WindwardWarming = SampleFoehn(*Ridge, FVector2d(0.0, 800.0));
	const double LeeWarming = SampleFoehn(*Ridge, FVector2d(0.0, -800.0));

	TestEqual(
		TEXT("Windward ground is not in a foehn at all"), WindwardWarming, 0.0, UE_DOUBLE_KINDA_SMALL_NUMBER);
	TestTrue(
		FString::Printf(TEXT("The lee flank warms (%.4f C)"), LeeWarming),
		LeeWarming > QuantisationStep(*Ridge, MassifEnvironmentChannels::FoehnWarming()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWindFoehnDescentTest,
	"Massif.WorldGen.Wind.FoehnDescent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWindFoehnDescentTest::RunTest(const FString& Parameters)
{
	// The warming is the descent from whatever the flow last crossed, at the dry
	// adiabatic rate, taken down by how far short of full shelter the point is.
	//
	// Reach and step count are set so one sweep sample lands exactly on the crest
	// line 992 m upwind: the altitude the solve topped out at is then an altitude
	// this test can read back out of the terrain, and the assertion is against the
	// model rather than against a threshold someone guessed.
	constexpr double SynopticBearing = 0.0;
	const FVector2d LeePosition(32.0, -992.0);
	const FVector2d CrestPosition(32.0, 0.0);

	FMassifWindParams WindParams = MakeWindParams(SynopticBearing);
	WindParams.ExposureReachMeters = 1024.0;
	WindParams.ExposureStepCount = 32;

	const TUniquePtr<FMassifEnvironmentScheduler> Ridge = SolveWind(
		MakeWindTerrain(EMassifAnalyticTerrain::Ridge, 90.0), WindParams, EMassifFidelity::Standard);

	const FName& Altitude = MassifEnvironmentChannels::Altitude();
	const double DescentMeters = SampleScalarChannel(*Ridge, Altitude, CrestPosition)
		- SampleScalarChannel(*Ridge, Altitude, LeePosition);
	const double DryWarmingCelsius = FMath::Min(
		WindParams.DryAdiabaticLapseCelsiusPerMeter * DescentMeters, WindParams.MaxFoehnWarmingCelsius);
	const double Shelter = FMath::Clamp(1.0 - SampleExposure(*Ridge, LeePosition), 0.0, 1.0);
	const double Expected = DryWarmingCelsius * Shelter;
	const double Measured = SampleFoehn(*Ridge, LeePosition);

	// One step for the warming's own encoding and one for a value sitting on a step
	// boundary, plus what a step of the exposure encoding is worth once the shelter
	// has carried it into the answer.
	const double Tolerance = 2.0 * QuantisationStep(*Ridge, MassifEnvironmentChannels::FoehnWarming())
		+ DryWarmingCelsius * QuantisationStep(*Ridge, MassifEnvironmentChannels::WindExposure());

	TestTrue(
		FString::Printf(TEXT("The warming is the descent at the dry lapse rate (%.5f against %.5f C over %.1f m, "
			"tolerance %.5f)"), Measured, Expected, DescentMeters, Tolerance),
		FMath::Abs(Measured - Expected) <= Tolerance);
	return true;
}
