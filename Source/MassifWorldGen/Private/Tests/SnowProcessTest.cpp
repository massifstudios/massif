#include "MassifElevationSource.h"
#include "MassifEnvironmentGraph.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifPrecipitationProcesses.h"
#include "MassifSnowProcesses.h"
#include "MassifSunProcesses.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr uint32 SnowTestSeed = 4711;
	constexpr double SnowTestWorldMeters = 2048.0;

	/** Slices the pack is written at, which is what every reduction here is counted against. */
	constexpr int32 SnowTestSliceCount = 24;

	constexpr double SnowTestQuantisationSteps = 65535.0;

	/** The wind bearing is pinned in every test: a seed-drawn one makes exposed and sheltered unreadable. */
	constexpr double SnowTestBearingDegrees = 0.0;

	FMassifEnvironmentParams MakeSnowEnvironmentParams()
	{
		FMassifEnvironmentParams Params;
		Params.Water.CellSizeMeters = 32.0;

		// Soil has nothing to say about the snowpack, and the droplets are the
		// expensive part of the water solve.
		Params.Water.DropletsPerCell = 0.05;
		Params.Water.MaxDropletSteps = 8;
		Params.Wind.SynopticBearingDegrees = SnowTestBearingDegrees;
		return Params;
	}

	FMassifAnalyticElevationSource::FParams MakeSnowTerrain(
		EMassifAnalyticTerrain Shape, double BearingDegrees, double ReliefMeters)
	{
		FMassifAnalyticElevationSource::FParams Params;
		Params.Shape = Shape;
		Params.WorldSizeMeters = SnowTestWorldMeters;
		Params.BaseAltitudeMeters = 1600.0;
		Params.ReliefMeters = ReliefMeters;
		Params.BearingDegrees = BearingDegrees;
		return Params;
	}

	/** Everything one solve is asked for: the world, the datum it sits in, and how the pack is tuned. */
	struct FSnowTestCase
	{
		FMassifAnalyticElevationSource::FParams Terrain =
			MakeSnowTerrain(EMassifAnalyticTerrain::Ridge, 90.0, 800.0);
		FMassifEnvironmentParams Environment = MakeSnowEnvironmentParams();
		FMassifSnowParams Snow;
		EMassifFidelity SnowFidelity = EMassifFidelity::Standard;
		EMassifFidelity PrecipitationFidelity = EMassifFidelity::Standard;
		double SeaLevelMeanTemperatureCelsius = FMassifGeoreference().SeaLevelMeanTemperatureCelsius;
	};

	/**
	 * The whole standard set, which the snowpack is part of.
	 *
	 * Not thrift: the pack reads the precipitation, the seasonal temperature, the
	 * sun and the wind, and the scheduler refuses a graph whose inputs no
	 * registered process produces.
	 */
	TUniquePtr<FMassifEnvironmentScheduler> SolveSnow(const FSnowTestCase& TestCase)
	{
		FMassifGeoreference Georeference;
		Georeference.SeaLevelMeanTemperatureCelsius = TestCase.SeaLevelMeanTemperatureCelsius;

		TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MakeUnique<FMassifEnvironmentScheduler>(
			MakeShared<FMassifAnalyticElevationSource>(TestCase.Terrain), Georeference, SnowTestSeed);

		FMassifEnvironmentParams Params = TestCase.Environment;
		Params.Snow = TestCase.Snow;
		MassifRegisterEnvironmentProcesses(*Scheduler, Params);

		FString Error;
		checkf(Scheduler->BuildSchedule(Error), TEXT("%s"), *Error);
		Scheduler->SetFidelity(MassifEnvironmentProcesses::Precipitation(), TestCase.PrecipitationFidelity);
		Scheduler->SetFidelity(MassifEnvironmentProcesses::Snowpack(), TestCase.SnowFidelity);
		Scheduler->Execute();
		return Scheduler;
	}

	FMassifChannelHandle SnowChannel(const FMassifEnvironmentScheduler& Scheduler, const FName& ChannelName)
	{
		return Scheduler.GetState().FindChannel(ChannelName);
	}

	const FMassifChannelGrid& SnowGrid(const FMassifEnvironmentScheduler& Scheduler)
	{
		return Scheduler.GetState().GetGrid(SnowChannel(Scheduler, MassifEnvironmentChannels::SnowDepth()));
	}

	/** One cell of a solved channel, unfiltered, so no interpolation enters a comparison. */
	double ReadSnowCell(
		const FMassifEnvironmentScheduler& Scheduler, const FName& ChannelName, int32 Column, int32 Row,
		int32 TimeSlice = 0)
	{
		return Scheduler.GetState().ReadCell(SnowChannel(Scheduler, ChannelName), Column, Row, 0, TimeSlice);
	}

	double SampleSnowChannel(
		const FMassifEnvironmentScheduler& Scheduler, const FName& ChannelName, FVector2d PositionMeters)
	{
		return Scheduler.GetState().SampleScalar(SnowChannel(Scheduler, ChannelName), PositionMeters);
	}

	/** All a uint16 channel can be asked to hold, so nothing here guesses a tolerance. */
	double SnowQuantisationStep(const FMassifEnvironmentScheduler& Scheduler, const FName& ChannelName)
	{
		const FMassifEnvironmentState& State = Scheduler.GetState();
		const FMassifChannelHandle Channel = SnowChannel(Scheduler, ChannelName);
		const double Span = State.GetChannelMaximum(Channel) - State.GetChannelMinimum(Channel);
		return FMath::Max(Span / SnowTestQuantisationSteps, UE_DOUBLE_KINDA_SMALL_NUMBER);
	}

	double TotalDepthMetres(const FMassifEnvironmentScheduler& Scheduler, int32 TimeSlice)
	{
		const FMassifChannelGrid& Grid = SnowGrid(Scheduler);
		double TotalMetres = 0.0;
		for (int32 CellIndex = 0; CellIndex < Grid.CellCount(); ++CellIndex)
		{
			TotalMetres += ReadSnowCell(
				Scheduler, MassifEnvironmentChannels::SnowDepth(), CellIndex % Grid.Width,
				CellIndex / Grid.Width, TimeSlice);
		}
		return TotalMetres;
	}

	/** Deepest cell minus shallowest, over one slice: how far apart the pack has been driven. */
	double DepthSpreadMetres(const FMassifEnvironmentScheduler& Scheduler, int32 TimeSlice)
	{
		const FMassifChannelGrid& Grid = SnowGrid(Scheduler);
		double DeepestMetres = 0.0;
		double ShallowestMetres = TNumericLimits<double>::Max();
		for (int32 CellIndex = 0; CellIndex < Grid.CellCount(); ++CellIndex)
		{
			const double DepthMetres = ReadSnowCell(
				Scheduler, MassifEnvironmentChannels::SnowDepth(), CellIndex % Grid.Width,
				CellIndex / Grid.Width, TimeSlice);
			DeepestMetres = FMath::Max(DeepestMetres, DepthMetres);
			ShallowestMetres = FMath::Min(ShallowestMetres, DepthMetres);
		}
		return DeepestMetres - ShallowestMetres;
	}

	/**
	 * A pack that only ever gains: melt and sublimation taken out, so whatever the
	 * wind does to the snow is the only thing left that can change the total.
	 */
	FSnowTestCase MakeDriftOnlyCase(double RedistributionStrength)
	{
		FSnowTestCase TestCase;
		TestCase.Snow.DegreeDayFactorMetresPerDegreeDay = 0.0;
		TestCase.Snow.SublimationMetresPerDay = 0.0;
		TestCase.Snow.WindRedistributionStrength = RedistributionStrength;

		// Flat weather, so the snowfall entering the redistribution is the same
		// everywhere and any spread across the grid afterwards is the wind's doing.
		TestCase.PrecipitationFidelity = EMassifFidelity::Off;

		// Well below freezing at every altitude in the world and in every slice of
		// the year, so all of it falls as snow and none of it is at risk of melting.
		TestCase.SeaLevelMeanTemperatureCelsius = -20.0;
		return TestCase;
	}

	/**
	 * A ridge under uniform snowfall with the wind transport switched off, so the
	 * only thing separating its two flanks is how much sun they take.
	 *
	 * A warm datum and a dry year on purpose: the pack has to clear at some point
	 * for a cover duration to say anything, and under a whole default winter at two
	 * thousand metres neither flank ever melts out.
	 */
	FSnowTestCase MakeAspectCase()
	{
		FSnowTestCase TestCase;
		TestCase.Terrain = MakeSnowTerrain(EMassifAnalyticTerrain::Ridge, 90.0, 800.0);
		TestCase.Snow.WindRedistributionStrength = 0.0;
		TestCase.SeaLevelMeanTemperatureCelsius = 18.0;
		TestCase.PrecipitationFidelity = EMassifFidelity::Cheap;
		TestCase.Environment.Precipitation.BaseAnnualMillimetres = 250.0;
		TestCase.Environment.Precipitation.AltitudeGainMillimetresPerKilometre = 0.0;
		return TestCase;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSnowMassConservationTest,
	"Massif.WorldGen.Snow.MassConservation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnowMassConservationTest::RunTest(const FString& Parameters)
{
	// Wind moves snow; it does not make or destroy it. With nothing able to remove
	// snow, everything that fell is still lying at the end of the year, so the total
	// over the grid may not move when the redistribution is turned up.
	const TUniquePtr<FMassifEnvironmentScheduler> Still = SolveSnow(MakeDriftOnlyCase(0.0));
	const TUniquePtr<FMassifEnvironmentScheduler> Drifted = SolveSnow(MakeDriftOnlyCase(0.8));

	const int32 LastSlice = SnowTestSliceCount - 1;
	const double StillMetres = TotalDepthMetres(*Still, LastSlice);
	const double DriftedMetres = TotalDepthMetres(*Drifted, LastSlice);

	TestTrue(
		FString::Printf(TEXT("There is snow on the ground to conserve (%.1f m summed over the grid)"),
			StillMetres),
		StillMetres > 0.0);

	// Every cell of both totals carries half a step of the channel's own rounding.
	const int32 CellCount = SnowGrid(*Still).CellCount();
	const double ToleranceMetres = 0.5 * CellCount
		* (SnowQuantisationStep(*Still, MassifEnvironmentChannels::SnowDepth())
			+ SnowQuantisationStep(*Drifted, MassifEnvironmentChannels::SnowDepth()));

	TestTrue(
		FString::Printf(TEXT("Redistribution leaves the total alone (%.4f m against %.4f m, tolerance %.4f m)"),
			DriftedMetres, StillMetres, ToleranceMetres),
		FMath::Abs(DriftedMetres - StillMetres) <= ToleranceMetres);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSnowRedistributionTest,
	"Massif.WorldGen.Snow.Redistribution",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnowRedistributionTest::RunTest(const FString& Parameters)
{
	// The same two solves the mass balance is asserted on, read the other way round:
	// conserving the total is only interesting if the snow moved at all. The
	// snowfall is uniform, so without the wind every cell holds the same depth and
	// the spread across the grid is zero by construction.
	const TUniquePtr<FMassifEnvironmentScheduler> Still = SolveSnow(MakeDriftOnlyCase(0.0));
	const TUniquePtr<FMassifEnvironmentScheduler> Drifted = SolveSnow(MakeDriftOnlyCase(0.8));

	const int32 LastSlice = SnowTestSliceCount - 1;
	const double StillSpreadMetres = DepthSpreadMetres(*Still, LastSlice);
	const double DriftedSpreadMetres = DepthSpreadMetres(*Drifted, LastSlice);

	TestTrue(
		FString::Printf(TEXT("Uniform snowfall lies uniformly deep (%.6f m between deepest and shallowest)"),
			StillSpreadMetres),
		StillSpreadMetres <= SnowQuantisationStep(*Still, MassifEnvironmentChannels::SnowDepth()));
	TestTrue(
		FString::Printf(TEXT("And the wind drives it apart (%.4f m against %.6f m)"),
			DriftedSpreadMetres, StillSpreadMetres),
		DriftedSpreadMetres > StillSpreadMetres
			+ SnowQuantisationStep(*Drifted, MassifEnvironmentChannels::SnowDepth()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSnowColdAndWarmWorldTest,
	"Massif.WorldGen.Snow.ColdAndWarmWorld",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnowColdAndWarmWorldTest::RunTest(const FString& Parameters)
{
	// The two ends of the model. A world that never rises above freezing is white
	// all year everywhere; one that never falls below it never sees snow at all,
	// whatever the terrain does.
	FSnowTestCase Frozen;
	Frozen.SeaLevelMeanTemperatureCelsius = -20.0;

	FSnowTestCase Warm;
	Warm.SeaLevelMeanTemperatureCelsius = 40.0;

	const TUniquePtr<FMassifEnvironmentScheduler> FrozenWorld = SolveSnow(Frozen);
	const TUniquePtr<FMassifEnvironmentScheduler> WarmWorld = SolveSnow(Warm);

	const FMassifEnvironmentState& FrozenState = FrozenWorld->GetState();
	const FMassifChannelHandle FrozenDuration =
		SnowChannel(*FrozenWorld, MassifEnvironmentChannels::SnowCoverDuration());
	TestEqual(
		TEXT("A world below freezing keeps its snow through every slice of the year"),
		FrozenState.GetChannelMinimum(FrozenDuration), 1.0);

	const FMassifEnvironmentState& WarmState = WarmWorld->GetState();
	const FMassifChannelHandle WarmDuration =
		SnowChannel(*WarmWorld, MassifEnvironmentChannels::SnowCoverDuration());
	TestEqual(
		TEXT("And a world above it never lies under any"),
		WarmState.GetChannelMaximum(WarmDuration), 0.0);
	TestEqual(
		TEXT("Nor has any depth to show for the year"),
		WarmState.GetChannelMaximum(SnowChannel(*WarmWorld, MassifEnvironmentChannels::PeakSnowDepth())), 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSnowAspectTest,
	"Massif.WorldGen.Snow.Aspect",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnowAspectTest::RunTest(const FString& Parameters)
{
	// A ridge running east-west stands its two flanks at the same altitude under the
	// same snowfall, so what is left between them is the sun: it warms the south
	// flank and it melts what lies there faster than the air temperature alone
	// would. This is why one side of a valley is skiable weeks after the other.
	const TUniquePtr<FMassifEnvironmentScheduler> Ridge = SolveSnow(MakeAspectCase());

	const FMassifChannelGrid& Grid = SnowGrid(*Ridge);
	const int32 Column = Grid.Width / 2;
	const int32 NorthRow = Grid.Height / 4;
	const int32 SouthRow = Grid.Height - 1 - NorthRow;

	const FVector2d NorthPosition = Grid.CellCentreMeters(Column, NorthRow);
	const FVector2d SouthPosition = Grid.CellCentreMeters(Column, SouthRow);
	TestEqual(
		TEXT("The two flanks stand at the same altitude"),
		SampleSnowChannel(*Ridge, MassifEnvironmentChannels::Altitude(), SouthPosition),
		SampleSnowChannel(*Ridge, MassifEnvironmentChannels::Altitude(), NorthPosition));

	// Read out of the sun channel rather than assumed: a shading claim is only worth
	// making if the two flanks really are lit differently.
	const double SouthInsolation =
		SampleSnowChannel(*Ridge, MassifEnvironmentChannels::DirectInsolation(), SouthPosition);
	const double NorthInsolation =
		SampleSnowChannel(*Ridge, MassifEnvironmentChannels::DirectInsolation(), NorthPosition);
	TestTrue(
		FString::Printf(TEXT("The south flank takes the more sun of the two (%.3f against %.3f)"),
			SouthInsolation, NorthInsolation),
		SouthInsolation > NorthInsolation);

	const double NorthDuration =
		ReadSnowCell(*Ridge, MassifEnvironmentChannels::SnowCoverDuration(), Column, NorthRow);
	const double SouthDuration =
		ReadSnowCell(*Ridge, MassifEnvironmentChannels::SnowCoverDuration(), Column, SouthRow);

	TestTrue(
		FString::Printf(TEXT("Both flanks clear at some point in the year (%.3f and %.3f of it under snow)"),
			NorthDuration, SouthDuration),
		NorthDuration < 1.0 && SouthDuration > 0.0);
	TestTrue(
		FString::Printf(TEXT("And the shaded one holds its snow the longer (%.3f against %.3f)"),
			NorthDuration, SouthDuration),
		NorthDuration > SouthDuration);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSnowReductionsTest,
	"Massif.WorldGen.Snow.Reductions",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnowReductionsTest::RunTest(const FString& Parameters)
{
	// The two reductions are the whole point of holding a time axis at all, and they
	// have to be the slices they claim to reduce rather than a second model of them.
	const TUniquePtr<FMassifEnvironmentScheduler> Ridge = SolveSnow(MakeAspectCase());

	const FMassifChannelGrid& Grid = SnowGrid(*Ridge);
	const int32 Column = Grid.Width / 2;
	const int32 Row = Grid.Height / 4;

	const FMassifSnowParams Defaults;
	double DeepestMetres = 0.0;
	int32 LyingSliceCount = 0;
	for (int32 TimeSlice = 0; TimeSlice < SnowTestSliceCount; ++TimeSlice)
	{
		const double DepthMetres =
			ReadSnowCell(*Ridge, MassifEnvironmentChannels::SnowDepth(), Column, Row, TimeSlice);
		DeepestMetres = FMath::Max(DeepestMetres, DepthMetres);
		if (DepthMetres > Defaults.LyingSnowThresholdMetres)
		{
			++LyingSliceCount;
		}
	}

	const double PeakMetres = ReadSnowCell(*Ridge, MassifEnvironmentChannels::PeakSnowDepth(), Column, Row);
	const double PeakToleranceMetres = SnowQuantisationStep(*Ridge, MassifEnvironmentChannels::SnowDepth())
		+ SnowQuantisationStep(*Ridge, MassifEnvironmentChannels::PeakSnowDepth());
	TestTrue(
		FString::Printf(TEXT("The peak depth is the deepest of the slices (%.4f m against %.4f m)"),
			PeakMetres, DeepestMetres),
		FMath::Abs(PeakMetres - DeepestMetres) <= PeakToleranceMetres);

	const double CountedDuration = LyingSliceCount / static_cast<double>(SnowTestSliceCount);
	const double StoredDuration =
		ReadSnowCell(*Ridge, MassifEnvironmentChannels::SnowCoverDuration(), Column, Row);
	TestTrue(
		FString::Printf(TEXT("The cover duration is the share of them with snow lying (%.4f against %.4f)"),
			StoredDuration, CountedDuration),
		FMath::Abs(StoredDuration - CountedDuration)
			<= SnowQuantisationStep(*Ridge, MassifEnvironmentChannels::SnowCoverDuration()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSnowOffTest,
	"Massif.WorldGen.Snow.Off",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnowOffTest::RunTest(const FString& Parameters)
{
	// Off is bare ground all year, not an absent channel: nothing downstream has to
	// ask whether the pack was solved before reading it.
	FSnowTestCase Bare;
	Bare.SnowFidelity = EMassifFidelity::Off;

	// Cold enough that a solved pack would bury the world, so an empty channel here
	// can only be the fidelity and not the weather.
	Bare.SeaLevelMeanTemperatureCelsius = -20.0;

	const TUniquePtr<FMassifEnvironmentScheduler> Unsolved = SolveSnow(Bare);
	const FMassifEnvironmentState& State = Unsolved->GetState();

	const FName Names[] = {
		MassifEnvironmentChannels::SnowDepth(), MassifEnvironmentChannels::PeakSnowDepth(),
		MassifEnvironmentChannels::SnowCoverDuration()};
	for (const FName& Name : Names)
	{
		const FMassifChannelHandle Channel = State.FindChannel(Name);
		TestTrue(FString::Printf(TEXT("%s exists at Off"), *Name.ToString()), Channel.IsValid());
		TestTrue(
			FString::Printf(TEXT("%s is written at Off"), *Name.ToString()), State.IsChannelWritten(Channel));
		TestEqual(
			FString::Printf(TEXT("%s reads as bare ground at Off"), *Name.ToString()),
			State.GetChannelMaximum(Channel), 0.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSnowDeterminismTest,
	"Massif.WorldGen.Snow.Determinism",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnowDeterminismTest::RunTest(const FString& Parameters)
{
	// A sequential process carries state from slice to slice and walks the year a
	// fixed number of times, which is exactly where a solve that stopped when it had
	// converged would start differing between runs (ARCHITECTURE §4).
	const uint64 FirstHash = SolveSnow(MakeAspectCase())->GetState().ComputeContentHash();
	const uint64 SecondHash = SolveSnow(MakeAspectCase())->GetState().ComputeContentHash();

	TestEqual(TEXT("The same seed solves the same year twice"), FirstHash, SecondHash);
	return true;
}
