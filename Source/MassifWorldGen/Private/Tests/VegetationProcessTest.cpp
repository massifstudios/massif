#include "MassifClimateProcesses.h"
#include "MassifElevationSource.h"
#include "MassifEnvironmentGraph.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifSnowProcesses.h"
#include "MassifVegetationProcesses.h"
#include "MassifWindProcesses.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr uint32 VegetationTestSeed = 4711;
	constexpr double VegetationTestWorldMeters = 4096.0;

	constexpr double VegetationTestQuantisationSteps = 65535.0;

	/** The wind bearing is pinned in every test: a seed-drawn one makes exposed and sheltered unreadable. */
	constexpr double VegetationTestBearingDegrees = 0.0;

	/** Everything one solve is asked for: the world it runs over and how the systems in it are tuned. */
	struct FVegetationTestCase
	{
		FMassifAnalyticElevationSource::FParams Terrain;
		FMassifEnvironmentParams Environment;
		EMassifFidelity VegetationFidelity = EMassifFidelity::Standard;
		EMassifFidelity WindFidelity = EMassifFidelity::Standard;
		double SeaLevelMeanTemperatureCelsius = FMassifGeoreference().SeaLevelMeanTemperatureCelsius;
	};

	FMassifAnalyticElevationSource::FParams MakeVegetationTerrain(
		EMassifAnalyticTerrain Shape, double BaseAltitudeMeters, double ReliefMeters)
	{
		FMassifAnalyticElevationSource::FParams Params;
		Params.Shape = Shape;
		Params.WorldSizeMeters = VegetationTestWorldMeters;
		Params.BaseAltitudeMeters = BaseAltitudeMeters;
		Params.ReliefMeters = ReliefMeters;
		Params.BearingDegrees = VegetationTestBearingDegrees;
		return Params;
	}

	FMassifEnvironmentParams MakeVegetationEnvironmentParams()
	{
		FMassifEnvironmentParams Params;
		Params.Water.CellSizeMeters = 32.0;

		// The droplets are the expensive part of the water solve, and all the
		// vegetation asks the soil is how deep it is.
		Params.Water.DropletsPerCell = 0.05;
		Params.Water.MaxDropletSteps = 8;

		// Deep regolith, so what limits a stand here is the climate rather than
		// whatever ground a short droplet run happened to leave.
		Params.Water.BaseSoilDepthMeters = 0.8;
		Params.Wind.SynopticBearingDegrees = VegetationTestBearingDegrees;

		// The wetness index over these analytic worlds sits well off the optimum, and
		// a narrow tolerance would make every reading here about moisture.
		Params.Vegetation.WetnessTolerance = 1.0;
		return Params;
	}

	/**
	 * The whole standard set, which vegetation is part of.
	 *
	 * Not thrift: the solve reads the climate, the soil, the wind and the snowpack,
	 * and this call registers the vegetation as well — a second registration of it
	 * would be two processes under one name, which the scheduler refuses.
	 */
	TUniquePtr<FMassifEnvironmentScheduler> SolveVegetation(const FVegetationTestCase& TestCase)
	{
		FMassifGeoreference Georeference;
		Georeference.SeaLevelMeanTemperatureCelsius = TestCase.SeaLevelMeanTemperatureCelsius;

		TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MakeUnique<FMassifEnvironmentScheduler>(
			MakeShared<FMassifAnalyticElevationSource>(TestCase.Terrain), Georeference, VegetationTestSeed);
		MassifRegisterEnvironmentProcesses(*Scheduler, TestCase.Environment);

		FString Error;
		checkf(Scheduler->BuildSchedule(Error), TEXT("%s"), *Error);
		Scheduler->SetFidelity(MassifEnvironmentProcesses::Vegetation(), TestCase.VegetationFidelity);
		Scheduler->SetFidelity(MassifEnvironmentProcesses::Wind(), TestCase.WindFidelity);
		Scheduler->Execute();
		return Scheduler;
	}

	/**
	 * A mountain tall enough to run out of forest before it runs out of ground:
	 * warm damp ground at the foot, a treeline partway up it, and a summit that
	 * carries nothing at all.
	 */
	FVegetationTestCase MakeVariedCase()
	{
		FVegetationTestCase TestCase;
		TestCase.Terrain = MakeVegetationTerrain(EMassifAnalyticTerrain::Cone, 1200.0, 1200.0);
		TestCase.Environment = MakeVegetationEnvironmentParams();
		return TestCase;
	}

	/**
	 * A world where nothing but the canopy itself limits anything: warm, gently
	 * tilted, deeply soiled, and with the wind off so no exposure shears a stand
	 * back or throws it down.
	 */
	FVegetationTestCase MakeClosedCanopyCase()
	{
		FVegetationTestCase TestCase;
		TestCase.Terrain = MakeVegetationTerrain(EMassifAnalyticTerrain::TiltedPlane, 500.0, 100.0);
		TestCase.Environment = MakeVegetationEnvironmentParams();
		TestCase.Environment.Vegetation.OptimalWetness = 0.5;
		TestCase.Environment.Vegetation.WetnessTolerance = 20.0;
		TestCase.WindFidelity = EMassifFidelity::Off;
		return TestCase;
	}

	/** Ground no warmth reaches, so every channel the solve writes is the empty end of itself. */
	FVegetationTestCase MakeFrozenCase()
	{
		FVegetationTestCase TestCase = MakeVariedCase();
		TestCase.SeaLevelMeanTemperatureCelsius = -20.0;
		return TestCase;
	}

	/**
	 * A hill gentle enough that its windward flank still carries trees.
	 *
	 * Steeper ground exposes itself so thoroughly that the wind shears the stand
	 * away outright, and a windthrow that scales with the canopy has nothing left
	 * to take. Warm at every altitude in it, so no snow lies and what disturbance
	 * there is here is the wind alone.
	 */
	FVegetationTestCase MakeWindthrowCase()
	{
		FVegetationTestCase TestCase;
		TestCase.Terrain = MakeVegetationTerrain(EMassifAnalyticTerrain::Cone, 800.0, 500.0);
		TestCase.Environment = MakeVegetationEnvironmentParams();
		TestCase.SeaLevelMeanTemperatureCelsius = 24.0;
		return TestCase;
	}

	FMassifChannelHandle VegetationChannel(
		const FMassifEnvironmentScheduler& Scheduler, const FName& ChannelName)
	{
		return Scheduler.GetState().FindChannel(ChannelName);
	}

	const FMassifChannelGrid& VegetationGrid(const FMassifEnvironmentScheduler& Scheduler)
	{
		return Scheduler.GetState().GetGrid(
			VegetationChannel(Scheduler, MassifEnvironmentChannels::VegetationCover()));
	}

	double SampleVegetationChannel(
		const FMassifEnvironmentScheduler& Scheduler, const FName& ChannelName, FVector2d PositionMeters)
	{
		return Scheduler.GetState().SampleScalar(VegetationChannel(Scheduler, ChannelName), PositionMeters);
	}

	/** All a uint16 channel can be asked to hold, so nothing here guesses a tolerance. */
	double VegetationQuantisationStep(
		const FMassifEnvironmentScheduler& Scheduler, const FName& ChannelName)
	{
		const FMassifEnvironmentState& State = Scheduler.GetState();
		const FMassifChannelHandle Channel = VegetationChannel(Scheduler, ChannelName);
		const double Span = State.GetChannelMaximum(Channel) - State.GetChannelMinimum(Channel);
		return FMath::Max(Span / VegetationTestQuantisationSteps, UE_DOUBLE_KINDA_SMALL_NUMBER);
	}

	/** The highest cell of a channel as it stands after the last relaxation round. */
	double HighestCell(const FMassifEnvironmentScheduler& Scheduler, const FName& ChannelName)
	{
		const FMassifEnvironmentState& State = Scheduler.GetState();
		const FMassifChannelHandle Channel = VegetationChannel(Scheduler, ChannelName);
		const FMassifChannelGrid& Grid = State.GetGrid(Channel);

		double Highest = TNumericLimits<double>::Lowest();
		for (int32 CellIndex = 0; CellIndex < Grid.CellCount(); ++CellIndex)
		{
			Highest = FMath::Max(
				Highest, State.ReadCell(Channel, CellIndex % Grid.Width, CellIndex / Grid.Width));
		}
		return Highest;
	}

	int32 ClassAtCell(const FMassifEnvironmentScheduler& Scheduler, int32 Column, int32 Row)
	{
		const FMassifEnvironmentState& State = Scheduler.GetState();
		return FMath::RoundToInt32(
			State.ReadCell(VegetationChannel(Scheduler, MassifEnvironmentChannels::VegetationClass()), Column, Row));
	}

	/** What a cell's class is checked against, read back out of the channels that decided it. */
	struct FVegetationReadings
	{
		double Cover = 0.0;
		double Canopy = 0.0;
		double Wetness = 0.0;
		double TemperatureCelsius = 0.0;
		double WindExposure = 1.0;
		double SnowCoverDuration = 0.0;
	};

	FVegetationReadings ReadingsAt(const FMassifEnvironmentScheduler& Scheduler, FVector2d PositionMeters)
	{
		FVegetationReadings Readings;
		Readings.Cover =
			SampleVegetationChannel(Scheduler, MassifEnvironmentChannels::VegetationCover(), PositionMeters);
		Readings.Canopy =
			SampleVegetationChannel(Scheduler, MassifEnvironmentChannels::VegetationCanopy(), PositionMeters);
		Readings.Wetness =
			SampleVegetationChannel(Scheduler, MassifEnvironmentChannels::Wetness(), PositionMeters);
		Readings.TemperatureCelsius =
			SampleVegetationChannel(Scheduler, MassifEnvironmentChannels::MeanTemperature(), PositionMeters);
		Readings.WindExposure =
			SampleVegetationChannel(Scheduler, MassifEnvironmentChannels::WindExposure(), PositionMeters);
		Readings.SnowCoverDuration =
			SampleVegetationChannel(Scheduler, MassifEnvironmentChannels::SnowCoverDuration(), PositionMeters);
		return Readings;
	}

	/** One quantisation step of whichever channel each of those readings came out of. */
	FVegetationReadings MakeReadingSteps(const FMassifEnvironmentScheduler& Scheduler)
	{
		FVegetationReadings Steps;
		Steps.Cover = VegetationQuantisationStep(Scheduler, MassifEnvironmentChannels::VegetationCover());
		Steps.Canopy = VegetationQuantisationStep(Scheduler, MassifEnvironmentChannels::VegetationCanopy());
		Steps.Wetness = VegetationQuantisationStep(Scheduler, MassifEnvironmentChannels::Wetness());
		Steps.TemperatureCelsius =
			VegetationQuantisationStep(Scheduler, MassifEnvironmentChannels::MeanTemperature());
		Steps.WindExposure = VegetationQuantisationStep(Scheduler, MassifEnvironmentChannels::WindExposure());
		Steps.SnowCoverDuration =
			VegetationQuantisationStep(Scheduler, MassifEnvironmentChannels::SnowCoverDuration());
		return Steps;
	}

	/**
	 * Whether the conditions read back at a point are conditions its class may
	 * stand in, each threshold slackened by what the channel it was read from
	 * could have rounded away.
	 */
	bool ClassHolds(
		EMassifVegetationClass Class, const FVegetationReadings& Readings, const FVegetationReadings& Steps,
		const FMassifVegetationParams& Params)
	{
		if (Class >= EMassifVegetationClass::Count)
		{
			return false;
		}
		if (Class == EMassifVegetationClass::Wetland)
		{
			return Readings.Wetness > Params.WetlandWetness - Steps.Wetness;
		}
		if (Class == EMassifVegetationClass::Bare)
		{
			return Readings.Cover < Params.BareCoverThreshold + Steps.Cover;
		}
		if (Class == EMassifVegetationClass::Snowbed)
		{
			return Readings.SnowCoverDuration > Params.SnowbedCoverDuration - Steps.SnowCoverDuration;
		}
		if (Class == EMassifVegetationClass::ConiferForest)
		{
			return Readings.Canopy > Params.ForestCanopyThreshold - Steps.Canopy
				&& Readings.TemperatureCelsius < Params.BroadleafTemperatureCelsius + Steps.TemperatureCelsius;
		}
		if (Class == EMassifVegetationClass::BroadleafForest)
		{
			return Readings.Canopy > Params.ForestCanopyThreshold - Steps.Canopy
				&& Readings.TemperatureCelsius > Params.BroadleafTemperatureCelsius - Steps.TemperatureCelsius;
		}
		if (Class == EMassifVegetationClass::DwarfShrub)
		{
			return Readings.Canopy < Params.ForestCanopyThreshold + Steps.Canopy
				&& (Readings.WindExposure > Params.ShrubExposure - Steps.WindExposure || Readings.Canopy > 0.0);
		}
		return Readings.Cover > Params.BareCoverThreshold - Steps.Cover
			&& Readings.Canopy <= Steps.Canopy
			&& Readings.WindExposure < Params.ShrubExposure + Steps.WindExposure;
	}

	/** Two cells side by side holding different classes, for reading across the boundary between them. */
	struct FClassBoundary
	{
		FVector2d FirstCentreMeters = FVector2d::ZeroVector;
		FVector2d SecondCentreMeters = FVector2d::ZeroVector;
		int32 FirstClass = INDEX_NONE;
		int32 SecondClass = INDEX_NONE;

		bool IsValid() const
		{
			return FirstClass != INDEX_NONE && FirstClass != SecondClass;
		}
	};

	FClassBoundary FindBoundaryInRow(
		const FMassifEnvironmentScheduler& Scheduler, const FMassifChannelGrid& Grid, int32 Row)
	{
		FClassBoundary Boundary;
		for (int32 Column = 1; Column < Grid.Width; ++Column)
		{
			Boundary.FirstClass = ClassAtCell(Scheduler, Column - 1, Row);
			Boundary.SecondClass = ClassAtCell(Scheduler, Column, Row);
			Boundary.FirstCentreMeters = Grid.CellCentreMeters(Column - 1, Row);
			Boundary.SecondCentreMeters = Grid.CellCentreMeters(Column, Row);
			if (Boundary.IsValid())
			{
				return Boundary;
			}
		}
		return FClassBoundary();
	}

	FClassBoundary FindClassBoundary(const FMassifEnvironmentScheduler& Scheduler)
	{
		const FMassifChannelGrid& Grid = VegetationGrid(Scheduler);
		for (int32 Row = 0; Row < Grid.Height; ++Row)
		{
			const FClassBoundary Boundary = FindBoundaryInRow(Scheduler, Grid, Row);
			if (Boundary.IsValid())
			{
				return Boundary;
			}
		}
		return FClassBoundary();
	}

	/** A cell picked out of the grid, with everything a comparison against another one needs. */
	struct FVegetationSample
	{
		FVector2d CentreMeters = FVector2d::ZeroVector;
		double AltitudeMeters = 0.0;
		double Cover = 0.0;
		double WindExposure = 1.0;
		double Disturbance = 0.0;
	};

	FVegetationSample SampleCell(const FMassifEnvironmentScheduler& Scheduler, int32 CellIndex)
	{
		const FMassifChannelGrid& Grid = VegetationGrid(Scheduler);

		FVegetationSample Sample;
		Sample.CentreMeters = Grid.CellCentreMeters(CellIndex % Grid.Width, CellIndex / Grid.Width);
		Sample.AltitudeMeters =
			SampleVegetationChannel(Scheduler, MassifEnvironmentChannels::Altitude(), Sample.CentreMeters);
		Sample.Cover = SampleVegetationChannel(
			Scheduler, MassifEnvironmentChannels::VegetationCover(), Sample.CentreMeters);
		Sample.WindExposure = SampleVegetationChannel(
			Scheduler, MassifEnvironmentChannels::WindExposure(), Sample.CentreMeters);
		Sample.Disturbance =
			SampleVegetationChannel(Scheduler, MassifEnvironmentChannels::Disturbance(), Sample.CentreMeters);
		return Sample;
	}

	/** Where the wind has taken down the most of what grew there. */
	FVegetationSample FindMostDisturbedCell(const FMassifEnvironmentScheduler& Scheduler)
	{
		const FMassifChannelGrid& Grid = VegetationGrid(Scheduler);

		FVegetationSample Worst = SampleCell(Scheduler, 0);
		for (int32 CellIndex = 1; CellIndex < Grid.CellCount(); ++CellIndex)
		{
			const FVegetationSample Sample = SampleCell(Scheduler, CellIndex);
			if (Sample.Disturbance > Worst.Disturbance)
			{
				Worst = Sample;
			}
		}
		return Worst;
	}

	/**
	 * The least exposed cell standing at the same height as a given one.
	 *
	 * Altitude is what carries the temperature and the snow, so holding it fixed is
	 * what leaves the wind as the difference between two points on this cone.
	 */
	FVegetationSample FindShelteredCellAtAltitude(
		const FMassifEnvironmentScheduler& Scheduler, double AltitudeMeters, double ToleranceMeters)
	{
		const FMassifChannelGrid& Grid = VegetationGrid(Scheduler);

		FVegetationSample Sheltered;
		Sheltered.WindExposure = TNumericLimits<double>::Max();
		for (int32 CellIndex = 0; CellIndex < Grid.CellCount(); ++CellIndex)
		{
			const FVegetationSample Sample = SampleCell(Scheduler, CellIndex);
			const bool bAtAltitude = FMath::Abs(Sample.AltitudeMeters - AltitudeMeters) <= ToleranceMeters;
			if (bAtAltitude && Sample.WindExposure < Sheltered.WindExposure)
			{
				Sheltered = Sample;
			}
		}
		return Sheltered;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegetationInterceptionTest,
	"Massif.WorldGen.Vegetation.Interception",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FVegetationInterceptionTest::RunTest(const FString& Parameters)
{
	// A canopy catches rain and gives most of it back to the air before any of it
	// reaches the ground, which is the vegetation's half of the water loop: closed
	// forest returns about a third of a year's rainfall, and bare ground none of it.
	const FVegetationTestCase ClosedCase = MakeClosedCanopyCase();
	const TUniquePtr<FMassifEnvironmentScheduler> Closed = SolveVegetation(ClosedCase);
	const FMassifVegetationParams Params = ClosedCase.Environment.Vegetation;

	const double ClosedCanopy = HighestCell(*Closed, MassifEnvironmentChannels::VegetationCanopy());
	const double ClosedInterception = HighestCell(*Closed, MassifEnvironmentChannels::Interception());
	TestTrue(
		FString::Printf(TEXT("The warm world grows a closed canopy to intercept with (%.4f)"), ClosedCanopy),
		ClosedCanopy > 0.9);

	const double ClosedTolerance =
		VegetationQuantisationStep(*Closed, MassifEnvironmentChannels::Interception())
			+ Params.MaxInterceptionFraction
				* VegetationQuantisationStep(*Closed, MassifEnvironmentChannels::VegetationCanopy());
	TestTrue(
		FString::Printf(TEXT("And intercepts what that canopy is worth (%.4f against %.4f)"),
			ClosedInterception, Params.MaxInterceptionFraction * ClosedCanopy),
		FMath::Abs(ClosedInterception - Params.MaxInterceptionFraction * ClosedCanopy) <= ClosedTolerance);

	const TUniquePtr<FMassifEnvironmentScheduler> Frozen = SolveVegetation(MakeFrozenCase());
	TestEqual(
		TEXT("Ground too cold to carry anything intercepts nothing"),
		HighestCell(*Frozen, MassifEnvironmentChannels::Interception()), 0.0);

	// The closed form over the whole varied world, so the relation is the model's
	// and not one point's coincidence.
	const TUniquePtr<FMassifEnvironmentScheduler> Varied = SolveVegetation(MakeVariedCase());
	const FMassifChannelGrid& Grid = VegetationGrid(*Varied);
	const double VariedTolerance =
		VegetationQuantisationStep(*Varied, MassifEnvironmentChannels::Interception())
			+ Params.MaxInterceptionFraction
				* VegetationQuantisationStep(*Varied, MassifEnvironmentChannels::VegetationCanopy());

	double WorstError = 0.0;
	for (int32 CellIndex = 0; CellIndex < Grid.CellCount(); ++CellIndex)
	{
		const FVector2d Position = Grid.CellCentreMeters(CellIndex % Grid.Width, CellIndex / Grid.Width);
		const double Canopy =
			SampleVegetationChannel(*Varied, MassifEnvironmentChannels::VegetationCanopy(), Position);
		const double Interception =
			SampleVegetationChannel(*Varied, MassifEnvironmentChannels::Interception(), Position);
		WorstError =
			FMath::Max(WorstError, FMath::Abs(Interception - Params.MaxInterceptionFraction * Canopy));
	}

	TestTrue(
		FString::Printf(TEXT("Interception follows the canopy everywhere (worst %.6f, tolerance %.6f)"),
			WorstError, VariedTolerance),
		WorstError <= VariedTolerance);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegetationClassesTest,
	"Massif.WorldGen.Vegetation.Classes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FVegetationClassesTest::RunTest(const FString& Parameters)
{
	// A mountain does not carry one kind of vegetation, and the class channel has to
	// be the same reading as the channels it was decided from: what is asserted here
	// is not which classes turn up but that whichever do could stand where they are.
	const FVegetationTestCase TestCase = MakeVariedCase();
	const TUniquePtr<FMassifEnvironmentScheduler> Varied = SolveVegetation(TestCase);

	const FMassifChannelGrid& Grid = VegetationGrid(*Varied);
	const FVegetationReadings Steps = MakeReadingSteps(*Varied);

	TSet<int32> ClassesSeen;
	int32 InconsistentCount = 0;
	int32 InconsistentClass = 0;
	for (int32 CellIndex = 0; CellIndex < Grid.CellCount(); ++CellIndex)
	{
		const int32 Column = CellIndex % Grid.Width;
		const int32 Row = CellIndex / Grid.Width;
		const int32 ClassIndex = ClassAtCell(*Varied, Column, Row);
		const FVegetationReadings Readings = ReadingsAt(*Varied, Grid.CellCentreMeters(Column, Row));
		ClassesSeen.Add(ClassIndex);

		const bool bHolds = ClassHolds(
			static_cast<EMassifVegetationClass>(ClassIndex), Readings, Steps, TestCase.Environment.Vegetation);
		if (!bHolds)
		{
			++InconsistentCount;
			InconsistentClass = ClassIndex;
		}
	}

	TestTrue(
		FString::Printf(TEXT("The mountain carries more than one kind of vegetation (%d)"), ClassesSeen.Num()),
		ClassesSeen.Num() > 1);
	TestEqual(
		FString::Printf(TEXT("Every cell's class is one its conditions allow (%d against, such as %s)"),
			InconsistentCount,
			MassifVegetationClassName(static_cast<EMassifVegetationClass>(InconsistentClass))),
		InconsistentCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegetationClassSamplingTest,
	"Massif.WorldGen.Vegetation.ClassSampling",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FVegetationClassSamplingTest::RunTest(const FString& Parameters)
{
	// Halfway between meadow and forest there is no half-forest. A class index is a
	// name, not a quantity, and blending two of them would produce a third that
	// stands for nothing — which is what anything scattering off this channel would
	// then be reading.
	const TUniquePtr<FMassifEnvironmentScheduler> Varied = SolveVegetation(MakeVariedCase());
	const FClassBoundary Boundary = FindClassBoundary(*Varied);
	if (!TestTrue(TEXT("The world holds two cells of different classes side by side"), Boundary.IsValid()))
	{
		return false;
	}

	const FMassifEnvironmentState& State = Varied->GetState();
	const FMassifChannelHandle Channel =
		VegetationChannel(*Varied, MassifEnvironmentChannels::VegetationClass());

	int32 BlendedCount = 0;
	for (int32 Step = 1; Step < 10; ++Step)
	{
		const FVector2d Position = FMath::Lerp(
			Boundary.FirstCentreMeters, Boundary.SecondCentreMeters, Step / 10.0);
		const int32 Sampled = State.SampleClass(Channel, Position);
		if (Sampled != Boundary.FirstClass && Sampled != Boundary.SecondClass)
		{
			++BlendedCount;
		}
	}

	TestEqual(
		FString::Printf(TEXT("Sampling across the boundary gives class %d or class %d and nothing between"),
			Boundary.FirstClass, Boundary.SecondClass),
		BlendedCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegetationDisturbanceTest,
	"Massif.WorldGen.Vegetation.Disturbance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FVegetationDisturbanceTest::RunTest(const FString& Parameters)
{
	// The wind throws trees where it gets at them, and what it took down is gone
	// from the cover the soil and the wind read back. Two points at the same height
	// on the same cone, so the only thing left between them is which way they face
	// into the prevailing wind.
	constexpr double SameAltitudeToleranceMeters = 20.0;

	const TUniquePtr<FMassifEnvironmentScheduler> Hill = SolveVegetation(MakeWindthrowCase());
	const FVegetationSample Exposed = FindMostDisturbedCell(*Hill);
	const FVegetationSample Sheltered =
		FindShelteredCellAtAltitude(*Hill, Exposed.AltitudeMeters, SameAltitudeToleranceMeters);

	TestTrue(
		FString::Printf(TEXT("Something on this hill has been knocked down (%.4f)"), Exposed.Disturbance),
		Exposed.Disturbance > 0.0);
	TestTrue(
		FString::Printf(TEXT("It stands in the wind (%.3f exposure against %.3f)"),
			Exposed.WindExposure, Sheltered.WindExposure),
		Exposed.WindExposure > Sheltered.WindExposure);
	TestTrue(
		FString::Printf(TEXT("And carries less than the sheltered ground beside it (%.4f against %.4f)"),
			Exposed.Cover, Sheltered.Cover),
		Exposed.Cover < Sheltered.Cover);
	TestTrue(
		FString::Printf(TEXT("Which has had nothing taken off it (%.4f)"), Sheltered.Disturbance),
		Sheltered.Disturbance < Exposed.Disturbance);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegetationOffTest,
	"Massif.WorldGen.Vegetation.Off",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FVegetationOffTest::RunTest(const FString& Parameters)
{
	// Off is bare ground, not an absent channel: the soil that reads the cover and
	// the wind that reads the canopy never ask whether the vegetation was solved.
	FVegetationTestCase Unsolved = MakeVariedCase();
	Unsolved.VegetationFidelity = EMassifFidelity::Off;

	const TUniquePtr<FMassifEnvironmentScheduler> Bare = SolveVegetation(Unsolved);
	const FMassifEnvironmentState& State = Bare->GetState();

	const FName Names[] = {
		MassifEnvironmentChannels::VegetationCover(), MassifEnvironmentChannels::VegetationCanopy(),
		MassifEnvironmentChannels::VegetationHeight(), MassifEnvironmentChannels::Interception(),
		MassifEnvironmentChannels::Disturbance(), MassifEnvironmentChannels::VegetationClass()};
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

	TestEqual(
		TEXT("Which is the Bare class everywhere"),
		State.SampleClass(
			State.FindChannel(MassifEnvironmentChannels::VegetationClass()), FVector2d::ZeroVector),
		static_cast<int32>(EMassifVegetationClass::Bare));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegetationDeterminismTest,
	"Massif.WorldGen.Vegetation.Determinism",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FVegetationDeterminismTest::RunTest(const FString& Parameters)
{
	// Vegetation sits in a relaxation group with the soil, the wind and the
	// snowpack, run a fixed number of times in a fixed order — which is exactly
	// where a loop that stopped once it had settled would stop somewhere else on
	// another machine (ARCHITECTURE §4).
	const uint64 FirstHash = SolveVegetation(MakeVariedCase())->GetState().ComputeContentHash();
	const uint64 SecondHash = SolveVegetation(MakeVariedCase())->GetState().ComputeContentHash();

	TestEqual(TEXT("The same seed grows the same world twice"), FirstHash, SecondHash);
	return true;
}
