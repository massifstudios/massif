#include "MassifClimateProcesses.h"
#include "MassifElevationSource.h"
#include "MassifEnvironmentGraph.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifSunProcesses.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr uint32 ClimateTestSeed = 4711;

	/** A whole number of both cell sizes, so both grids sit symmetrically about the world centre. */
	constexpr double ClimateTestWorldMeters = 2048.0;
	constexpr double ClimateTestBakeCellMeters = 16.0;

	/** Steps a uint16 channel is quantised into, which is what bounds every tolerance here. */
	constexpr double ClimateTestQuantisationSteps = 65535.0;

	FMassifEnvironmentBakeParams MakeBakeParams()
	{
		FMassifEnvironmentBakeParams Params;
		Params.CellSizeMeters = ClimateTestBakeCellMeters;

		// Soil has nothing to say about temperature; the droplets only run because
		// the water set registers them.
		Params.DropletsPerCell = 0.25;
		Params.MaxDropletSteps = 32;
		return Params;
	}

	FMassifAnalyticElevationSource::FParams MakeClimateTerrain(
		EMassifAnalyticTerrain Shape, double BearingDegrees, double ReliefMeters)
	{
		FMassifAnalyticElevationSource::FParams Params;
		Params.Shape = Shape;
		Params.WorldSizeMeters = ClimateTestWorldMeters;
		Params.BaseAltitudeMeters = 1500.0;
		Params.ReliefMeters = ReliefMeters;
		Params.BearingDegrees = BearingDegrees;
		return Params;
	}

	/**
	 * The whole standard set, which the temperature is part of.
	 *
	 * Not thrift: the temperature reads a filled surface and an insolation, and the
	 * scheduler refuses a graph whose inputs no registered process produces.
	 */
	TUniquePtr<FMassifEnvironmentScheduler> SolveClimate(
		const FMassifAnalyticElevationSource::FParams& TerrainParams, EMassifFidelity TemperatureFidelity,
		EMassifFidelity SunFidelity = EMassifFidelity::Standard)
	{
		TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MakeUnique<FMassifEnvironmentScheduler>(
			MakeShared<FMassifAnalyticElevationSource>(TerrainParams), FMassifGeoreference(), ClimateTestSeed);

		MassifRegisterEnvironmentProcesses(*Scheduler, MakeBakeParams());

		FString Error;
		checkf(Scheduler->BuildSchedule(Error), TEXT("%s"), *Error);
		Scheduler->SetFidelity(MassifEnvironmentProcesses::Insolation(), SunFidelity);
		Scheduler->SetFidelity(MassifEnvironmentProcesses::Temperature(), TemperatureFidelity);
		Scheduler->Execute();
		return Scheduler;
	}

	FMassifChannelHandle TemperatureChannel(const FMassifEnvironmentScheduler& Scheduler)
	{
		return Scheduler.GetState().FindChannel(MassifEnvironmentChannels::MeanTemperature());
	}

	FMassifChannelGrid TemperatureGrid(const FMassifEnvironmentScheduler& Scheduler)
	{
		return Scheduler.GetState().GetGrid(TemperatureChannel(Scheduler));
	}

	/** One cell of the solved channel, unfiltered, so no interpolation enters a comparison. */
	double TemperatureAtCell(const FMassifEnvironmentScheduler& Scheduler, int32 Column, int32 Row)
	{
		return Scheduler.GetState().ReadCell(TemperatureChannel(Scheduler), Column, Row);
	}

	/**
	 * What one step of the channel's own quantisation is worth. Two decoded cells
	 * carry half a step of rounding each, so a difference between them is good to
	 * one step and no tolerance has to be guessed.
	 */
	double TemperatureQuantisationStepCelsius(const FMassifEnvironmentScheduler& Scheduler)
	{
		const FMassifEnvironmentState& State = Scheduler.GetState();
		const FMassifChannelHandle Channel = TemperatureChannel(Scheduler);
		const double SpanCelsius = State.GetChannelMaximum(Channel) - State.GetChannelMinimum(Channel);
		return SpanCelsius / ClimateTestQuantisationSteps;
	}

	double SampleClimateChannel(
		const FMassifEnvironmentScheduler& Scheduler, const FName& ChannelName, FVector2d PositionMeters)
	{
		const FMassifEnvironmentState& State = Scheduler.GetState();
		return State.SampleScalar(State.FindChannel(ChannelName), PositionMeters);
	}

	/** Depth of the hollow the fill had to flood here: what the pooling term is keyed to. */
	double BasinDepthMeters(const FMassifEnvironmentScheduler& Scheduler, FVector2d PositionMeters)
	{
		return SampleClimateChannel(Scheduler, MassifEnvironmentChannels::FilledAltitude(), PositionMeters)
			- SampleClimateChannel(Scheduler, MassifEnvironmentChannels::Altitude(), PositionMeters);
	}

	/** The model's pooling term, written out here rather than called, so the test states it. */
	double ExpectedPoolingCelsius(double DepthMeters)
	{
		const FMassifClimateParams Params;
		return Params.PoolingDepressionCelsius
			* (1.0 - FMath::Exp(-DepthMeters / Params.PoolingDepthScaleMeters));
	}

	double LargestDeviationCelsius(
		const FMassifEnvironmentState& State, FMassifChannelHandle Channel, double ExpectedCelsius)
	{
		const FMassifChannelGrid& Grid = State.GetGrid(Channel);
		double LargestCelsius = 0.0;
		for (int32 Row = 0; Row < Grid.Height; ++Row)
		{
			for (int32 Column = 0; Column < Grid.Width; ++Column)
			{
				const double Celsius = State.ReadCell(Channel, Column, Row);
				LargestCelsius = FMath::Max(LargestCelsius, FMath::Abs(Celsius - ExpectedCelsius));
			}
		}
		return LargestCelsius;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClimateLapseRateTest,
	"Massif.WorldGen.Climate.LapseRate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FClimateLapseRateTest::RunTest(const FString& Parameters)
{
	// Cheap is the lapse rate and nothing else, so a plane of constant gradient has
	// to give the rate back: the drop between two cells times the rate, against the
	// altitude channel the solve itself read.
	const TUniquePtr<FMassifEnvironmentScheduler> Plane =
		SolveClimate(MakeClimateTerrain(EMassifAnalyticTerrain::TiltedPlane, 0.0, 900.0), EMassifFidelity::Cheap);

	// The plane descends towards its bearing, and row 0 is north.
	const FMassifChannelGrid Grid = TemperatureGrid(*Plane);
	const int32 Column = Grid.Width / 2;
	const int32 LowRow = Grid.Height / 4;
	const int32 HighRow = 3 * Grid.Height / 4;

	const double LowAltitude =
		SampleClimateChannel(*Plane, MassifEnvironmentChannels::Altitude(), Grid.CellCentreMeters(Column, LowRow));
	const double HighAltitude =
		SampleClimateChannel(*Plane, MassifEnvironmentChannels::Altitude(), Grid.CellCentreMeters(Column, HighRow));
	TestTrue(
		FString::Printf(TEXT("The two cells stand at different altitudes (%.1f m against %.1f m)"),
			HighAltitude, LowAltitude),
		HighAltitude > LowAltitude + 1.0);

	const double MeasuredCelsius =
		TemperatureAtCell(*Plane, Column, LowRow) - TemperatureAtCell(*Plane, Column, HighRow);
	const double ExpectedCelsius =
		FMassifClimateParams().LapseRateCelsiusPerMeter * (HighAltitude - LowAltitude);
	TestTrue(
		FString::Printf(TEXT("The lapse rate comes back off the plane (%.4f C against %.4f C)"),
			MeasuredCelsius, ExpectedCelsius),
		FMath::Abs(MeasuredCelsius - ExpectedCelsius) <= TemperatureQuantisationStepCelsius(*Plane));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClimateOffTest,
	"Massif.WorldGen.Climate.Off",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FClimateOffTest::RunTest(const FString& Parameters)
{
	// Off is the datum everywhere, over a kilometre of relief that would otherwise
	// move it by six and a half degrees.
	const TUniquePtr<FMassifEnvironmentScheduler> Uniform =
		SolveClimate(MakeClimateTerrain(EMassifAnalyticTerrain::Cone, 0.0, 1000.0), EMassifFidelity::Off);

	const FMassifEnvironmentState& State = Uniform->GetState();
	const FMassifChannelHandle Channel = TemperatureChannel(*Uniform);
	TestTrue(TEXT("The temperature channel exists at Off"), Channel.IsValid());
	TestTrue(TEXT("The temperature channel is written at Off"), State.IsChannelWritten(Channel));

	const double DatumCelsius = Uniform->GetGeoreference().SeaLevelMeanTemperatureCelsius;
	const double LargestCelsius = LargestDeviationCelsius(State, Channel, DatumCelsius);
	TestTrue(
		FString::Printf(TEXT("Every cell reads the datum at Off (worst cell off by %.6f C)"), LargestCelsius),
		LargestCelsius < UE_DOUBLE_KINDA_SMALL_NUMBER);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClimateRidgeAspectTest,
	"Massif.WorldGen.Climate.RidgeAspect",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FClimateRidgeAspectTest::RunTest(const FString& Parameters)
{
	// A ridge running east-west stands its two flanks at the same altitude, so the
	// lapse rate cancels between them and what is left is the solar anomaly alone —
	// measured against the insolation the sun channel actually holds rather than
	// against a guess at what it should hold.
	const TUniquePtr<FMassifEnvironmentScheduler> Ridge =
		SolveClimate(MakeClimateTerrain(EMassifAnalyticTerrain::Ridge, 90.0, 1200.0), EMassifFidelity::Standard);

	const FMassifChannelGrid Grid = TemperatureGrid(*Ridge);
	const int32 Column = Grid.Width / 2;
	const int32 NorthRow = Grid.Height / 4;
	const int32 SouthRow = Grid.Height - 1 - NorthRow;

	const FVector2d NorthPosition = Grid.CellCentreMeters(Column, NorthRow);
	const FVector2d SouthPosition = Grid.CellCentreMeters(Column, SouthRow);
	TestEqual(TEXT("The two flanks stand at the same altitude"),
		SampleClimateChannel(*Ridge, MassifEnvironmentChannels::Altitude(), SouthPosition),
		SampleClimateChannel(*Ridge, MassifEnvironmentChannels::Altitude(), NorthPosition));

	const double SouthCelsius = TemperatureAtCell(*Ridge, Column, SouthRow);
	const double NorthCelsius = TemperatureAtCell(*Ridge, Column, NorthRow);
	TestTrue(
		FString::Printf(TEXT("The sunny flank is the warmer one (%.3f C against %.3f C)"),
			SouthCelsius, NorthCelsius),
		SouthCelsius > NorthCelsius);

	const double InsolationGap =
		SampleClimateChannel(*Ridge, MassifEnvironmentChannels::DirectInsolation(), SouthPosition)
		- SampleClimateChannel(*Ridge, MassifEnvironmentChannels::DirectInsolation(), NorthPosition);
	const double ExpectedCelsius = FMassifClimateParams().InsolationGainCelsius * InsolationGap;
	TestTrue(
		FString::Printf(TEXT("And by what the insolation between them is worth (%.4f C against %.4f C)"),
			SouthCelsius - NorthCelsius, ExpectedCelsius),
		FMath::Abs((SouthCelsius - NorthCelsius) - ExpectedCelsius)
			<= TemperatureQuantisationStepCelsius(*Ridge));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClimateColdAirPoolingTest,
	"Massif.WorldGen.Climate.ColdAirPooling",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FClimateColdAirPoolingTest::RunTest(const FString& Parameters)
{
	// The contrast is the test: the same basin floor is warmer than its rim without
	// pooling, because it is lower, and colder than it with pooling. Shallow on
	// purpose — the pooling saturates over a few tens of metres while the lapse rate
	// does not, so a deep enough hollow is warm at the bottom again. The sun is off
	// so that nothing but the pooling term separates the two runs.
	const FMassifAnalyticElevationSource::FParams Terrain =
		MakeClimateTerrain(EMassifAnalyticTerrain::Pit, 0.0, 60.0);
	const TUniquePtr<FMassifEnvironmentScheduler> Unpooled =
		SolveClimate(Terrain, EMassifFidelity::Standard, EMassifFidelity::Off);
	const TUniquePtr<FMassifEnvironmentScheduler> Pooled =
		SolveClimate(Terrain, EMassifFidelity::Reference, EMassifFidelity::Off);

	const FMassifChannelGrid Grid = TemperatureGrid(*Pooled);
	const int32 FloorColumn = Grid.Width / 2;
	const int32 RimColumn = Grid.Width - 2;
	const int32 Row = Grid.Height / 2;

	const FVector2d FloorPosition = Grid.CellCentreMeters(FloorColumn, Row);
	const FVector2d RimPosition = Grid.CellCentreMeters(RimColumn, Row);
	const double FloorAltitude =
		SampleClimateChannel(*Pooled, MassifEnvironmentChannels::Altitude(), FloorPosition);
	const double RimAltitude = SampleClimateChannel(*Pooled, MassifEnvironmentChannels::Altitude(), RimPosition);

	const double LapseCelsius =
		FMassifClimateParams().LapseRateCelsiusPerMeter * (RimAltitude - FloorAltitude);
	const double UnpooledCelsius =
		TemperatureAtCell(*Unpooled, FloorColumn, Row) - TemperatureAtCell(*Unpooled, RimColumn, Row);
	TestTrue(
		FString::Printf(TEXT("Without pooling the floor sits the lapse rate above its rim (%.4f C against %.4f C)"),
			UnpooledCelsius, LapseCelsius),
		FMath::Abs(UnpooledCelsius - LapseCelsius) <= TemperatureQuantisationStepCelsius(*Unpooled));
	TestTrue(
		FString::Printf(TEXT("Which leaves it the warmer of the two (%.4f C)"), UnpooledCelsius),
		UnpooledCelsius > 0.0);

	const double PooledCelsius =
		TemperatureAtCell(*Pooled, FloorColumn, Row) - TemperatureAtCell(*Pooled, RimColumn, Row);
	const double ExpectedCelsius = LapseCelsius
		- ExpectedPoolingCelsius(BasinDepthMeters(*Pooled, FloorPosition))
		+ ExpectedPoolingCelsius(BasinDepthMeters(*Pooled, RimPosition));
	TestTrue(
		FString::Printf(TEXT("With pooling the drained cold air overturns it (%.4f C against %.4f C)"),
			PooledCelsius, ExpectedCelsius),
		FMath::Abs(PooledCelsius - ExpectedCelsius) <= TemperatureQuantisationStepCelsius(*Pooled));
	TestTrue(
		FString::Printf(TEXT("Which leaves the floor colder than its rim (%.4f C)"), PooledCelsius),
		PooledCelsius < 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClimateChannelAtEveryFidelityTest,
	"Massif.WorldGen.Climate.ChannelAtEveryFidelity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FClimateChannelAtEveryFidelityTest::RunTest(const FString& Parameters)
{
	// Nothing downstream may have to ask which level the climate ran at.
	const EMassifFidelity Fidelities[] = {
		EMassifFidelity::Off, EMassifFidelity::Cheap, EMassifFidelity::Standard, EMassifFidelity::Reference};

	for (EMassifFidelity Fidelity : Fidelities)
	{
		const TUniquePtr<FMassifEnvironmentScheduler> Solved =
			SolveClimate(MakeClimateTerrain(EMassifAnalyticTerrain::Pit, 0.0, 300.0), Fidelity);
		const FMassifChannelHandle Channel = TemperatureChannel(*Solved);
		TestTrue(
			FString::Printf(TEXT("The channel exists at %s"), MassifFidelityName(Fidelity)), Channel.IsValid());
		TestTrue(
			FString::Printf(TEXT("The channel is written at %s"), MassifFidelityName(Fidelity)),
			Solved->GetState().IsChannelWritten(Channel));
	}
	return true;
}
