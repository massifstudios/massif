#include "MassifElevationSource.h"
#include "MassifEnvironmentGraph.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifHeightmap.h"
#include "MassifWindProcesses.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr uint32 DischargeTestSeed = 4711;
	constexpr int32 DischargeTestSide = 128;
	constexpr double DischargeTestSpacingMeters = 32.0;
	constexpr double DischargeTestWorldMeters = DischargeTestSide * DischargeTestSpacingMeters;

	FMassifEnvironmentParams MakeDischargeParams()
	{
		FMassifEnvironmentParams Params;
		Params.Water.CellSizeMeters = DischargeTestSpacingMeters;

		// Soil plays no part in any of this and the droplets are the expensive stage.
		Params.Water.DropletsPerCell = 0.05;
		Params.Water.MaxDropletSteps = 8;
		return Params;
	}

	/**
	 * A plane falling due south with no cross-slope, so every cell's water walks
	 * south and leaves through the southern border and nowhere else. That is what
	 * makes the total crossing that row the world's whole yield.
	 */
	TSharedRef<const FMassifHeightmap> MakeSouthwardPlane()
	{
		TArray<double> Altitudes;
		Altitudes.Reserve(DischargeTestSide * DischargeTestSide);
		for (int32 Row = 0; Row < DischargeTestSide; ++Row)
		{
			for (int32 Column = 0; Column < DischargeTestSide; ++Column)
			{
				// Row 0 is north, so altitude falls as the row index rises.
				Altitudes.Add(2000.0 - Row * DischargeTestSpacingMeters * 0.2);
			}
		}
		return FMassifHeightmap::FromAltitudes(
			Altitudes, DischargeTestSide, DischargeTestSide, DischargeTestSpacingMeters);
	}

	TUniquePtr<FMassifEnvironmentScheduler> SolveDischarge(
		TSharedRef<const IMassifElevationSource> Elevation, EMassifFidelity Fidelity,
		const FMassifEnvironmentParams& Params)
	{
		TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MakeUnique<FMassifEnvironmentScheduler>(
			MoveTemp(Elevation), FMassifGeoreference(), DischargeTestSeed);
		MassifRegisterEnvironmentProcesses(*Scheduler, Params);

		FString Error;
		checkf(Scheduler->BuildSchedule(Error), TEXT("%s"), *Error);
		Scheduler->SetFidelity(MassifEnvironmentProcesses::Discharge(), Fidelity);
		Scheduler->Execute();
		return Scheduler;
	}

	double SampleDischarge(const FMassifEnvironmentState& State, FVector2d PositionMeters)
	{
		return State.SampleScalar(State.FindChannel(MassifEnvironmentChannels::Discharge()), PositionMeters);
	}

	double SampleAccumulation(const FMassifEnvironmentState& State, FVector2d PositionMeters)
	{
		return State.SampleScalar(
			State.FindChannel(MassifEnvironmentChannels::FlowAccumulation()), PositionMeters);
	}

	/**
	 * What crosses one edge of the world, in cubic metres a second.
	 *
	 * Everything upslope of a border row leaves through it, so the total over the
	 * row is the yield of the whole flank behind it — the figure a point probe
	 * cannot give, because a probe's own catchment is only the ground between it
	 * and the divide.
	 */
	double SumBorderRow(const FMassifEnvironmentState& State, int32 Row)
	{
		const FMassifChannelHandle Discharge = State.FindChannel(MassifEnvironmentChannels::Discharge());
		const FMassifChannelGrid& Grid = State.GetGrid(Discharge);

		double TotalCubicMetersPerSecond = 0.0;
		for (int32 Column = 0; Column < Grid.Width; ++Column)
		{
			TotalCubicMetersPerSecond += State.ReadCell(Discharge, Column, Row);
		}
		return TotalCubicMetersPerSecond;
	}

	int32 SouthBorderRow(const FMassifEnvironmentState& State)
	{
		return State.GetGrid(State.FindChannel(MassifEnvironmentChannels::Discharge())).Height - 1;
	}

	/** The ridge both flank tests stand on: crest east to west through the world centre. */
	FMassifAnalyticElevationSource::FParams MakeRidgeTerrain()
	{
		FMassifAnalyticElevationSource::FParams TerrainParams;
		TerrainParams.Shape = EMassifAnalyticTerrain::Ridge;
		TerrainParams.WorldSizeMeters = DischargeTestWorldMeters;
		TerrainParams.BaseAltitudeMeters = 1200.0;
		TerrainParams.ReliefMeters = 1200.0;
		TerrainParams.BearingDegrees = 90.0;
		return TerrainParams;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDischargeConservationTest,
	"Massif.WorldGen.Discharge.Conservation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FDischargeConservationTest::RunTest(const FString& Parameters)
{
	const FMassifEnvironmentParams Params = MakeDischargeParams();
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler =
		SolveDischarge(MakeShared<FMassifHeightmapElevationSource>(MakeSouthwardPlane()),
			EMassifFidelity::Off, Params);

	const FMassifEnvironmentState& State = Scheduler->GetState();
	const double LeavingCubicMetersPerSecond = SumBorderRow(State, SouthBorderRow(State));

	const double WorldAreaSquareMeters = DischargeTestWorldMeters * DischargeTestWorldMeters;
	const double YieldCubicMetersPerSecond = 0.001 * Params.Water.FallbackRunoffMillimetersPerYear
		* WorldAreaSquareMeters / MassifSecondsPerYear;

	// Float32 over a sum of four thousand cells, so a part in ten thousand is the
	// accumulated rounding rather than water going missing.
	TestTrue(
		TEXT("Everything the world yields crosses the southern border"),
		FMath::Abs(LeavingCubicMetersPerSecond - YieldCubicMetersPerSecond)
			<= 1.0e-4 * YieldCubicMetersPerSecond);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDischargeEvenYearTest,
	"Massif.WorldGen.Discharge.EvenYear",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FDischargeEvenYearTest::RunTest(const FString& Parameters)
{
	// Off sheds one depth everywhere, so discharge and catchment differ by exactly
	// that depth at every point in the world. Nothing the weather does reaches it.
	const FMassifEnvironmentParams Params = MakeDischargeParams();
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = SolveDischarge(
		MakeShared<FMassifAnalyticElevationSource>(MakeRidgeTerrain()), EMassifFidelity::Off, Params);
	const FMassifEnvironmentState& State = Scheduler->GetState();

	const double ExpectedMetersPerSecond =
		0.001 * Params.Water.FallbackRunoffMillimetersPerYear / MassifSecondsPerYear;

	const FVector2d Probes[] = {
		FVector2d(-600.0, 300.0), FVector2d(600.0, 300.0),
		FVector2d(-200.0, -700.0), FVector2d(900.0, -100.0)};

	int32 ProbesOffTheEvenDepth = 0;
	for (const FVector2d& PositionMeters : Probes)
	{
		const double CatchmentSquareMeters = SampleAccumulation(State, PositionMeters);
		const double Expected = CatchmentSquareMeters * ExpectedMetersPerSecond;
		const double Measured = SampleDischarge(State, PositionMeters);
		ProbesOffTheEvenDepth += FMath::Abs(Measured - Expected) > 1.0e-3 * Expected ? 1 : 0;
	}

	TestEqual(TEXT("Discharge is the catchment times one depth everywhere"), ProbesOffTheEvenDepth, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDischargeWeatherTest,
	"Massif.WorldGen.Discharge.Weather",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FDischargeWeatherTest::RunTest(const FString& Parameters)
{
	// A ridge across the wind, which the precipitation solve wets on the windward
	// flank and dries in the lee. The two flanks are mirror images, so they shed
	// the same ground and any difference in what leaves them is water.
	//
	// Measured as whole flanks rather than at two probes. Rain falls hardest where
	// the air first starts climbing and thins as its moisture goes, so within the
	// windward flank the wettest ground is at the bottom and a probe partway up
	// drains only the drier ground above it. The flank is the unit the rain shadow
	// is a statement about.
	FMassifEnvironmentParams Params = MakeDischargeParams();
	// Pinned rather than seed-drawn: with a bearing nobody chose, windward is
	// whichever flank the seed happened to point at. The synoptic bearing is where
	// the wind comes from, so a northerly blows towards -Y and the crest, which
	// runs east to west, stands across it.
	Params.Wind.SynopticBearingDegrees = 0.0;

	const TSharedRef<const IMassifElevationSource> Elevation =
		MakeShared<FMassifAnalyticElevationSource>(MakeRidgeTerrain());
	const TUniquePtr<FMassifEnvironmentScheduler> Solved =
		SolveDischarge(Elevation, EMassifFidelity::Standard, Params);
	const TUniquePtr<FMassifEnvironmentScheduler> Even =
		SolveDischarge(Elevation, EMassifFidelity::Off, Params);

	// The even year is the control: it says the two flanks shed the same ground,
	// which is what makes the solved comparison a comparison of rainfall.
	const double EvenWindward = SumBorderRow(Even->GetState(), 0);
	const double EvenLee = SumBorderRow(Even->GetState(), SouthBorderRow(Even->GetState()));
	if (!TestTrue(
			TEXT("The two flanks shed the same ground"),
			FMath::Abs(EvenWindward - EvenLee) <= 0.01 * FMath::Max(EvenWindward, EvenLee)))
	{
		return false;
	}

	const double SolvedWindward = SumBorderRow(Solved->GetState(), 0);
	const double SolvedLee = SumBorderRow(Solved->GetState(), SouthBorderRow(Solved->GetState()));
	TestTrue(
		FString::Printf(
			TEXT("The windward flank yields more than the lee flank it mirrors (%.3f against %.3f m3/s)"),
			SolvedWindward, SolvedLee),
		SolvedWindward > SolvedLee);
	return true;
}
