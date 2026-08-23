#include "MassifClimateProcesses.h"
#include "MassifElevationSource.h"
#include "MassifEnvironmentGraph.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifLithologyProcesses.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr uint32 LithologyTestSeed = 4711;

	/** Steps a uint16 channel is quantised into, which is what bounds every tolerance here. */
	constexpr double LithologyQuantisationSteps = 65535.0;

	/** Wide enough for several bands of the default four-kilometre wavelength to fit across it. */
	constexpr double LithologyTestWorldMeters = 32768.0;

	/** No terrain is read in the graph that has no altitude channel, so it can be small. */
	constexpr double LithologyBareWorldMeters = 8192.0;

	/** Small, because the whole standard set runs over it to get a temperature. */
	constexpr double LithologyClimateWorldMeters = 2048.0;

	FMassifAnalyticElevationSource::FParams MakeLithologyTerrain(
		EMassifAnalyticTerrain Shape, double WorldSizeMeters, double ReliefMeters)
	{
		FMassifAnalyticElevationSource::FParams Params;
		Params.Shape = Shape;
		Params.WorldSizeMeters = WorldSizeMeters;
		Params.BaseAltitudeMeters = 1500.0;
		Params.ReliefMeters = ReliefMeters;
		return Params;
	}

	/**
	 * The rock over bare terrain.
	 *
	 * No climate, so the shattering falls back to its baseline and nothing but the
	 * seed and the ground itself shapes what comes out.
	 */
	TUniquePtr<FMassifEnvironmentScheduler> SolveLithology(
		const FMassifAnalyticElevationSource::FParams& TerrainParams, EMassifFidelity Fidelity,
		uint32 MasterSeed = LithologyTestSeed)
	{
		TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MakeUnique<FMassifEnvironmentScheduler>(
			MakeShared<FMassifAnalyticElevationSource>(TerrainParams), FMassifGeoreference(), MasterSeed);

		FMassifEnvironmentBakeParams BakeParams;
		BakeParams.CellSizeMeters = 64.0;
		MassifRegisterAltitudeProcess(*Scheduler, BakeParams);
		MassifRegisterLithologyProcesses(*Scheduler, FMassifLithologyParams());

		FString Error;
		checkf(Scheduler->BuildSchedule(Error), TEXT("%s"), *Error);
		Scheduler->SetFidelity(MassifEnvironmentProcesses::Lithology(), Fidelity);
		Scheduler->Execute();
		return Scheduler;
	}

	/** The rock on its own: nothing in this graph produces either of its optional inputs. */
	TUniquePtr<FMassifEnvironmentScheduler> SolveLithologyAlone()
	{
		TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MakeUnique<FMassifEnvironmentScheduler>(
			MakeShared<FMassifAnalyticElevationSource>(
				MakeLithologyTerrain(EMassifAnalyticTerrain::Flat, LithologyBareWorldMeters, 0.0)),
			FMassifGeoreference(), LithologyTestSeed);

		MassifRegisterLithologyProcesses(*Scheduler, FMassifLithologyParams());

		FString Error;
		checkf(Scheduler->BuildSchedule(Error), TEXT("%s"), *Error);
		Scheduler->Execute();
		return Scheduler;
	}

	/**
	 * Adds the rock only where the standard set does not already carry it.
	 *
	 * Registering a process twice gives its channels two producers and the schedule
	 * is refused, so this test does not have to move when the lithology joins that
	 * set.
	 */
	void RegisterLithologyOnce(FMassifEnvironmentScheduler& Scheduler)
	{
		if (Scheduler.GetSupportedFidelities(MassifEnvironmentProcesses::Lithology()).Num() > 0)
		{
			return;
		}
		MassifRegisterLithologyProcesses(Scheduler, FMassifLithologyParams());
	}

	/**
	 * The whole standard set with the rock in it.
	 *
	 * Not thrift: the frost shattering reads a temperature, and the temperature
	 * reads a filled surface and an insolation. Three kilometres of relief so the
	 * ground runs from well above freezing at the foot to well below it at the top.
	 */
	TUniquePtr<FMassifEnvironmentScheduler> SolveLithologyWithClimate()
	{
		TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MakeUnique<FMassifEnvironmentScheduler>(
			MakeShared<FMassifAnalyticElevationSource>(
				MakeLithologyTerrain(EMassifAnalyticTerrain::Cone, LithologyClimateWorldMeters, 3000.0)),
			FMassifGeoreference(), LithologyTestSeed);

		FMassifEnvironmentBakeParams BakeParams;
		BakeParams.CellSizeMeters = 32.0;
		BakeParams.DropletsPerCell = 0.25;
		BakeParams.MaxDropletSteps = 32;
		MassifRegisterEnvironmentProcesses(*Scheduler, BakeParams);
		RegisterLithologyOnce(*Scheduler);

		FString Error;
		checkf(Scheduler->BuildSchedule(Error), TEXT("%s"), *Error);
		Scheduler->Execute();
		return Scheduler;
	}

	double SampleLithologyChannel(
		const FMassifEnvironmentScheduler& Scheduler, const FName& ChannelName, FVector2d PositionMeters)
	{
		const FMassifEnvironmentState& State = Scheduler.GetState();
		return State.SampleScalar(State.FindChannel(ChannelName), PositionMeters);
	}

	/** One step of a channel's own quantisation, which is what any reading off it is good to. */
	double LithologyQuantisationStep(const FMassifEnvironmentScheduler& Scheduler, const FName& ChannelName)
	{
		const FMassifEnvironmentState& State = Scheduler.GetState();
		const FMassifChannelHandle Channel = State.FindChannel(ChannelName);
		return (State.GetChannelMaximum(Channel) - State.GetChannelMinimum(Channel)) / LithologyQuantisationSteps;
	}

	double LargestDeviationFrom(
		const FMassifEnvironmentState& State, const FName& ChannelName, double ExpectedValue)
	{
		const FMassifChannelHandle Channel = State.FindChannel(ChannelName);
		const FMassifChannelGrid& Grid = State.GetGrid(Channel);

		double Largest = 0.0;
		for (int32 Row = 0; Row < Grid.Height; ++Row)
		{
			for (int32 Column = 0; Column < Grid.Width; ++Column)
			{
				Largest = FMath::Max(Largest, FMath::Abs(State.ReadCell(Channel, Column, Row) - ExpectedValue));
			}
		}
		return Largest;
	}

	double MeanOf(TArrayView<const double> Values)
	{
		if (Values.IsEmpty())
		{
			return 0.0;
		}

		double Total = 0.0;
		for (double Value : Values)
		{
			Total += Value;
		}
		return Total / Values.Num();
	}

	/** Pearson correlation of two equal-length series, 0 where either of them is constant. */
	double CorrelationOf(TArrayView<const double> First, TArrayView<const double> Second)
	{
		const double FirstMean = MeanOf(First);
		const double SecondMean = MeanOf(Second);

		double Covariance = 0.0;
		double FirstVariance = 0.0;
		double SecondVariance = 0.0;
		for (int32 Index = 0; Index < First.Num(); ++Index)
		{
			const double FirstOffset = First[Index] - FirstMean;
			const double SecondOffset = Second[Index] - SecondMean;
			Covariance += FirstOffset * SecondOffset;
			FirstVariance += FirstOffset * FirstOffset;
			SecondVariance += SecondOffset * SecondOffset;
		}

		const double Denominator = FMath::Sqrt(FirstVariance * SecondVariance);
		if (Denominator <= UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			return 0.0;
		}
		return Covariance / Denominator;
	}

	/** Direction of a compass bearing in the world frame: 0 north, 90 east. */
	FVector2d LithologyTestBearingDirection(double BearingDegrees)
	{
		const double BearingRadians = FMath::DegreesToRadians(BearingDegrees);
		return FVector2d(FMath::Sin(BearingRadians), FMath::Cos(BearingRadians));
	}

	/** One band width, so a step across the strike lands in the next formation. */
	constexpr double StrikeLagMeters = 4000.0;

	/** Half-width of the box the pairs are drawn from, leaving the lag inside the world. */
	constexpr double StrikeSampleExtentMeters = 12000.0;

	constexpr int32 StrikeSampleCount = 48;

	double StrikeSampleCoordinate(int32 Index)
	{
		return FMath::Lerp(
			-StrikeSampleExtentMeters, StrikeSampleExtentMeters, Index / double(StrikeSampleCount - 1));
	}

	void AppendStrikeSampleRow(
		const FMassifEnvironmentScheduler& Scheduler, FVector2d Step, int32 Row, TArray<double>& OutAtPoint,
		TArray<double>& OutAtLag)
	{
		for (int32 Column = 0; Column < StrikeSampleCount; ++Column)
		{
			const FVector2d Position(StrikeSampleCoordinate(Column), StrikeSampleCoordinate(Row));
			OutAtPoint.Add(
				SampleLithologyChannel(Scheduler, MassifEnvironmentChannels::RockHardness(), Position));
			OutAtLag.Add(
				SampleLithologyChannel(Scheduler, MassifEnvironmentChannels::RockHardness(), Position + Step));
		}
	}

	/** How well the hardness at a point predicts the hardness one band width along a bearing. */
	double HardnessCorrelationAlong(const FMassifEnvironmentScheduler& Scheduler, double BearingDegrees)
	{
		const FVector2d Step = LithologyTestBearingDirection(BearingDegrees) * StrikeLagMeters;

		TArray<double> AtPoint;
		TArray<double> AtLag;
		for (int32 Row = 0; Row < StrikeSampleCount; ++Row)
		{
			AppendStrikeSampleRow(Scheduler, Step, Row, AtPoint, AtLag);
		}
		return CorrelationOf(AtPoint, AtLag);
	}

	/** One cell of the solved world, as the three channels the shattering tests compare. */
	struct FGroundSample
	{
		double TemperatureCelsius = 0.0;
		double RockHardness = 0.0;
		double ScreeMetresPerMillennium = 0.0;
	};

	void AppendGroundRow(
		const FMassifEnvironmentState& State, const FMassifChannelGrid& Grid, int32 Row,
		TArray<FGroundSample>& OutSamples)
	{
		const FMassifChannelHandle Temperature = State.FindChannel(MassifEnvironmentChannels::MeanTemperature());
		const FMassifChannelHandle Hardness = State.FindChannel(MassifEnvironmentChannels::RockHardness());
		const FMassifChannelHandle Scree = State.FindChannel(MassifEnvironmentChannels::ScreeProduction());

		for (int32 Column = 0; Column < Grid.Width; ++Column)
		{
			FGroundSample Sample;
			Sample.TemperatureCelsius = State.ReadCell(Temperature, Column, Row);
			Sample.RockHardness = State.ReadCell(Hardness, Column, Row);
			Sample.ScreeMetresPerMillennium = State.ReadCell(Scree, Column, Row);
			OutSamples.Add(Sample);
		}
	}

	/** Every cell of the rock grid, which the climate is solved on too. */
	TArray<FGroundSample> ReadGround(const FMassifEnvironmentScheduler& Scheduler)
	{
		const FMassifEnvironmentState& State = Scheduler.GetState();
		const FMassifChannelGrid& Grid =
			State.GetGrid(State.FindChannel(MassifEnvironmentChannels::ScreeProduction()));

		TArray<FGroundSample> Samples;
		Samples.Reserve(Grid.CellCount());
		for (int32 Row = 0; Row < Grid.Height; ++Row)
		{
			AppendGroundRow(State, Grid, Row, Samples);
		}
		return Samples;
	}

	/** Mean scree over the cells of one temperature band, and how many cells that was. */
	struct FScreeBand
	{
		double MeanMetresPerMillennium = 0.0;
		int32 CellCount = 0;
	};

	FScreeBand MeanScreeBetween(
		const TArray<FGroundSample>& Ground, double LowCelsius, double HighCelsius)
	{
		FScreeBand Band;
		for (const FGroundSample& Sample : Ground)
		{
			const bool bInBand =
				Sample.TemperatureCelsius >= LowCelsius && Sample.TemperatureCelsius <= HighCelsius;
			if (bInBand)
			{
				Band.MeanMetresPerMillennium += Sample.ScreeMetresPerMillennium;
				++Band.CellCount;
			}
		}

		if (Band.CellCount > 0)
		{
			Band.MeanMetresPerMillennium /= Band.CellCount;
		}
		return Band;
	}

	double SpanOf(const TArray<double>& Values)
	{
		if (Values.IsEmpty())
		{
			return 0.0;
		}
		return FMath::Max(Values) - FMath::Min(Values);
	}

	/** What the scree comes to where nothing produces a temperature, from the hardness alone. */
	double ExpectedBaselineScree(double RockHardness)
	{
		const FMassifLithologyParams Params;
		return Params.MaxScreeMetresPerMillennium * Params.BaselineShatteringFraction
			* (1.0 - Params.HardnessResistance * RockHardness);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLithologyVariesTest,
	"Massif.WorldGen.Lithology.Varies",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLithologyVariesTest::RunTest(const FString& Parameters)
{
	// Every slope in a world of one rock is the same slope. Flat ground, so nothing
	// but the bands themselves is in the field, and the range they cover has to be a
	// real share of nought to one rather than a wobble about the middle of it.
	const TUniquePtr<FMassifEnvironmentScheduler> Solved = SolveLithology(
		MakeLithologyTerrain(EMassifAnalyticTerrain::Flat, LithologyTestWorldMeters, 0.0),
		EMassifFidelity::Standard);

	const FMassifEnvironmentState& State = Solved->GetState();
	const FMassifChannelHandle Hardness = State.FindChannel(MassifEnvironmentChannels::RockHardness());
	const double WeakestRock = State.GetChannelMinimum(Hardness);
	const double HardestRock = State.GetChannelMaximum(Hardness);

	TestTrue(
		FString::Printf(TEXT("The weakest rock in the world sits well below the middle (%.3f)"), WeakestRock),
		WeakestRock < 0.35);
	TestTrue(
		FString::Printf(TEXT("And the hardest well above it (%.3f)"), HardestRock), HardestRock > 0.65);

	const FMassifChannelHandle SoilYield = State.FindChannel(MassifEnvironmentChannels::SoilYield());
	const double SoilSpan = State.GetChannelMaximum(SoilYield) - State.GetChannelMinimum(SoilYield);
	TestTrue(
		FString::Printf(TEXT("And the soil the rock gives varies with it (%.3f)"), SoilSpan), SoilSpan > 0.3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLithologyDeterminismTest,
	"Massif.WorldGen.Lithology.Determinism",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLithologyDeterminismTest::RunTest(const FString& Parameters)
{
	// The bake is a pure function of the source, the georeference and the seed, and
	// is rebuilt rather than saved, so the same seed must lay down the same rock
	// every time and a different one must lay down different rock.
	const FMassifAnalyticElevationSource::FParams Terrain =
		MakeLithologyTerrain(EMassifAnalyticTerrain::Cone, LithologyTestWorldMeters, 1200.0);

	const uint64 FirstHash =
		SolveLithology(Terrain, EMassifFidelity::Standard, LithologyTestSeed)->GetState().ComputeContentHash();
	const uint64 RepeatHash =
		SolveLithology(Terrain, EMassifFidelity::Standard, LithologyTestSeed)->GetState().ComputeContentHash();
	const uint64 OtherSeedHash =
		SolveLithology(Terrain, EMassifFidelity::Standard, LithologyTestSeed + 1)
			->GetState().ComputeContentHash();

	TestTrue(
		FString::Printf(TEXT("The same seed lays down the same rock (%llu against %llu)"), RepeatHash, FirstHash),
		RepeatHash == FirstHash);
	TestTrue(
		FString::Printf(TEXT("A different seed lays down different rock (%llu against %llu)"),
			OtherSeedHash, FirstHash),
		OtherSeedHash != FirstHash);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLithologyStrikeTest,
	"Massif.WorldGen.Lithology.Strike",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLithologyStrikeTest::RunTest(const FString& Parameters)
{
	// Rock comes in bands, and a band has a direction. Along the strike the same
	// formation carries on for kilometres; a band width across it puts you in the
	// next one. This is the property that makes the field read as geology rather
	// than as noise, and flat ground leaves nothing else in it.
	const TUniquePtr<FMassifEnvironmentScheduler> Solved = SolveLithology(
		MakeLithologyTerrain(EMassifAnalyticTerrain::Flat, LithologyTestWorldMeters, 0.0),
		EMassifFidelity::Standard);

	const double StrikeBearingDegrees = MassifLithologyStrikeBearingDegrees(LithologyTestSeed);
	const double AlongStrike = HardnessCorrelationAlong(*Solved, StrikeBearingDegrees);
	const double AcrossStrike = HardnessCorrelationAlong(*Solved, StrikeBearingDegrees + 90.0);

	TestTrue(
		FString::Printf(TEXT("The same rock carries on along the strike (%.3f at %.1f degrees)"),
			AlongStrike, StrikeBearingDegrees),
		AlongStrike > 0.5);
	TestTrue(
		FString::Printf(TEXT("And gives way to another across it (%.3f against %.3f)"),
			AcrossStrike, AlongStrike),
		AcrossStrike < AlongStrike - 0.2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLithologyFrostShatteringTest,
	"Massif.WorldGen.Lithology.FrostShattering",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLithologyFrostShatteringTest::RunTest(const FString& Parameters)
{
	// Frost shattering is water changing phase inside a crack, so it runs fastest
	// where the ground crosses zero rather than where it is coldest. Averaged over
	// every cell of each band, because what rock a cell happens to be made of is
	// exactly what the average is there to take out of the comparison.
	const TUniquePtr<FMassifEnvironmentScheduler> Solved = SolveLithologyWithClimate();

	const FMassifEnvironmentState& State = Solved->GetState();
	TestEqual(TEXT("The rock and the climate are solved on the same grid"),
		State.GetGrid(State.FindChannel(MassifEnvironmentChannels::ScreeProduction())).Width,
		State.GetGrid(State.FindChannel(MassifEnvironmentChannels::MeanTemperature())).Width);

	const TArray<FGroundSample> Ground = ReadGround(*Solved);
	const FScreeBand NearFreezing = MeanScreeBetween(Ground, -1.5, 1.5);
	const FScreeBand WellAbove = MeanScreeBetween(Ground, 5.0, TNumericLimits<double>::Max());
	const FScreeBand WellBelow = MeanScreeBetween(Ground, -TNumericLimits<double>::Max(), -5.0);

	TestTrue(
		FString::Printf(TEXT("The world holds ground at, above and below freezing (%d, %d, %d cells)"),
			NearFreezing.CellCount, WellAbove.CellCount, WellBelow.CellCount),
		NearFreezing.CellCount > 0 && WellAbove.CellCount > 0 && WellBelow.CellCount > 0);

	TestTrue(
		FString::Printf(TEXT("Ground near freezing sheds more than ground well above it (%.4f against %.4f)"),
			NearFreezing.MeanMetresPerMillennium, WellAbove.MeanMetresPerMillennium),
		NearFreezing.MeanMetresPerMillennium > WellAbove.MeanMetresPerMillennium);
	TestTrue(
		FString::Printf(TEXT("And more than ground well below it (%.4f against %.4f)"),
			NearFreezing.MeanMetresPerMillennium, WellBelow.MeanMetresPerMillennium),
		NearFreezing.MeanMetresPerMillennium > WellBelow.MeanMetresPerMillennium);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLithologySoftRockShedsMoreTest,
	"Massif.WorldGen.Lithology.SoftRockShedsMore",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLithologySoftRockShedsMoreTest::RunTest(const FString& Parameters)
{
	// Held to half a degree either side of the peak, where the shattering term varies
	// by under one per cent, so what is left between these cells is the rock alone.
	const TUniquePtr<FMassifEnvironmentScheduler> Solved = SolveLithologyWithClimate();
	const TArray<FGroundSample> Ground = ReadGround(*Solved);

	TArray<double> Hardness;
	TArray<double> Scree;
	for (const FGroundSample& Sample : Ground)
	{
		const bool bNearFreezing = FMath::Abs(Sample.TemperatureCelsius) <= 0.5;
		if (bNearFreezing)
		{
			Hardness.Add(Sample.RockHardness);
			Scree.Add(Sample.ScreeMetresPerMillennium);
		}
	}

	const double HardnessSpan = SpanOf(Hardness);
	TestTrue(
		FString::Printf(TEXT("The band holds rock of very different hardness (%d cells spanning %.3f)"),
			Hardness.Num(), HardnessSpan),
		Hardness.Num() > 16 && HardnessSpan > 0.15);

	const double Correlation = CorrelationOf(Hardness, Scree);
	TestTrue(
		FString::Printf(TEXT("The softer the rock at that temperature, the more it sheds (%.3f)"), Correlation),
		Correlation < -0.9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLithologyWithoutOptionalInputsTest,
	"Massif.WorldGen.Lithology.WithoutOptionalInputs",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLithologyWithoutOptionalInputsTest::RunTest(const FString& Parameters)
{
	// Rock is a property of the place, not of the weather, so the process has to run
	// in a graph that produces neither a temperature nor a sampled altitude. That is
	// what the optional inputs are for.
	const TUniquePtr<FMassifEnvironmentScheduler> Solved = SolveLithologyAlone();
	const FMassifEnvironmentState& State = Solved->GetState();

	TestEqual(TEXT("Nothing but the rock is in the graph"), State.GetChannelCount(), 3);

	const FName Names[] = {
		MassifEnvironmentChannels::RockHardness(), MassifEnvironmentChannels::SoilYield(),
		MassifEnvironmentChannels::ScreeProduction()};
	for (const FName& Name : Names)
	{
		const FMassifChannelHandle Channel = State.FindChannel(Name);
		TestTrue(FString::Printf(TEXT("%s exists"), *Name.ToString()), Channel.IsValid());
		TestTrue(FString::Printf(TEXT("%s is written"), *Name.ToString()), State.IsChannelWritten(Channel));
	}

	// With no temperature to read, every cell shatters at the baseline rate, which
	// leaves the scree a closed form of the hardness beside it.
	const FMassifLithologyParams Params;
	const FMassifChannelHandle Hardness = State.FindChannel(MassifEnvironmentChannels::RockHardness());
	const FMassifChannelHandle Scree = State.FindChannel(MassifEnvironmentChannels::ScreeProduction());
	const FMassifChannelGrid& Grid = State.GetGrid(Scree);

	double LargestDeviation = 0.0;
	for (int32 Row = 0; Row < Grid.Height; ++Row)
	{
		for (int32 Column = 0; Column < Grid.Width; ++Column)
		{
			const double Expected = ExpectedBaselineScree(State.ReadCell(Hardness, Column, Row));
			LargestDeviation =
				FMath::Max(LargestDeviation, FMath::Abs(State.ReadCell(Scree, Column, Row) - Expected));
		}
	}

	// A cell carries a step of the scree channel's own rounding and a step of the
	// hardness channel's, the second through the slope of the model against it.
	const double Tolerance = LithologyQuantisationStep(*Solved, MassifEnvironmentChannels::ScreeProduction())
		+ Params.MaxScreeMetresPerMillennium * Params.BaselineShatteringFraction * Params.HardnessResistance
			* LithologyQuantisationStep(*Solved, MassifEnvironmentChannels::RockHardness());
	TestTrue(
		FString::Printf(TEXT("And the scree is the baseline rate over that rock (worst cell off by %.8f)"),
			LargestDeviation),
		LargestDeviation <= Tolerance);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLithologyOffTest,
	"Massif.WorldGen.Lithology.Off",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLithologyOffTest::RunTest(const FString& Parameters)
{
	// Off is one rock over the whole world, and the channels still exist: nothing
	// downstream has to ask whether the ground was solved before reading it.
	const TUniquePtr<FMassifEnvironmentScheduler> Uniform = SolveLithology(
		MakeLithologyTerrain(EMassifAnalyticTerrain::Cone, LithologyBareWorldMeters, 1200.0),
		EMassifFidelity::Off);
	const FMassifEnvironmentState& State = Uniform->GetState();

	const FName Names[] = {
		MassifEnvironmentChannels::RockHardness(), MassifEnvironmentChannels::SoilYield(),
		MassifEnvironmentChannels::ScreeProduction()};
	const double Expected[] = {0.5, 0.5, 0.0};

	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Names); ++Index)
	{
		const FMassifChannelHandle Channel = State.FindChannel(Names[Index]);
		TestTrue(FString::Printf(TEXT("%s exists at Off"), *Names[Index].ToString()), Channel.IsValid());
		TestTrue(
			FString::Printf(TEXT("%s is written at Off"), *Names[Index].ToString()),
			State.IsChannelWritten(Channel));

		const double LargestDeviation = LargestDeviationFrom(State, Names[Index], Expected[Index]);
		TestTrue(
			FString::Printf(TEXT("%s reads %.1f everywhere at Off (worst cell off by %.8f)"),
				*Names[Index].ToString(), Expected[Index], LargestDeviation),
			LargestDeviation < UE_DOUBLE_KINDA_SMALL_NUMBER);
	}
	return true;
}
