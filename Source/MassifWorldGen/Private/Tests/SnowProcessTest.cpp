#include "MassifClimateProcesses.h"
#include "MassifElevationSource.h"
#include "MassifEnvironmentGraph.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifPrecipitationProcesses.h"
#include "MassifSnowProcesses.h"
#include "MassifSunProcesses.h"
#include "MassifVegetationProcesses.h"
#include "MassifWindProcesses.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr uint32 SnowTestSeed = 4711;
	constexpr double SnowTestWorldMeters = 2048.0;

	/** Slices the pack is written at, which is what every reduction here is counted against. */
	constexpr int32 SnowTestSliceCount = 24;

	/** The first half of January and the first half of July, as the sliced year counts them. */
	constexpr int32 SnowTestMidwinterSlice = 0;
	constexpr int32 SnowTestMidsummerSlice = 12;

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
		EMassifFidelity VegetationFidelity = EMassifFidelity::Standard;
		EMassifFidelity WindFidelity = EMassifFidelity::Standard;
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
		Scheduler->SetFidelity(MassifEnvironmentProcesses::Vegetation(), TestCase.VegetationFidelity);
		Scheduler->SetFidelity(MassifEnvironmentProcesses::Wind(), TestCase.WindFidelity);
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
		const FMassifEnvironmentScheduler& Scheduler, const FName& ChannelName, FVector2d PositionMeters,
		int32 TimeSlice = 0)
	{
		return Scheduler.GetState().SampleScalar(
			SnowChannel(Scheduler, ChannelName), PositionMeters, TimeSlice);
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

	/** One cell's depth summed over every slice: how much pack the year held, not just its peak. */
	double YearDepthMetresAtCell(const FMassifEnvironmentScheduler& Scheduler, int32 Column, int32 Row)
	{
		double TotalMetres = 0.0;
		for (int32 TimeSlice = 0; TimeSlice < SnowTestSliceCount; ++TimeSlice)
		{
			TotalMetres += ReadSnowCell(Scheduler, MassifEnvironmentChannels::SnowDepth(), Column, Row, TimeSlice);
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
	 * Ground steepness at a cell of the snow grid, differenced exactly the way the
	 * release reads it: the neighbouring cell centres of that same grid.
	 */
	double SnowSlopeDegrees(const FMassifEnvironmentScheduler& Scheduler, int32 Column, int32 Row)
	{
		const FMassifChannelGrid& Grid = SnowGrid(Scheduler);
		const FName& Altitude = MassifEnvironmentChannels::Altitude();

		const double East = SampleSnowChannel(Scheduler, Altitude, Grid.CellCentreMeters(Column + 1, Row));
		const double West = SampleSnowChannel(Scheduler, Altitude, Grid.CellCentreMeters(Column - 1, Row));
		const double North = SampleSnowChannel(Scheduler, Altitude, Grid.CellCentreMeters(Column, Row - 1));
		const double South = SampleSnowChannel(Scheduler, Altitude, Grid.CellCentreMeters(Column, Row + 1));

		const double SpanMeters = 2.0 * Grid.CellSizeMeters;
		const FVector2d Gradient((East - West) / SpanMeters, (South - North) / SpanMeters);
		return FMath::RadiansToDegrees(FMath::Atan(Gradient.Length()));
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

	/** A world nothing ever melts in, so the pack only ever deepens and settles. */
	FSnowTestCase MakeFrozenSnowCase()
	{
		FSnowTestCase TestCase;
		TestCase.SeaLevelMeanTemperatureCelsius = -20.0;
		return TestCase;
	}

	/**
	 * A world with a real winter and a short summer, level enough that the sun over
	 * it carries the season and nothing else.
	 *
	 * The insolation channel is measured against an average growing-season day, so
	 * on ground this flat a midwinter slice reads well below one and a midsummer
	 * slice above it. Which of the two the melt reads is then legible in the pack it
	 * leaves behind.
	 */
	FSnowTestCase MakeSeasonalMeltCase(double InsolationMeltGain)
	{
		FSnowTestCase TestCase;

		// A hair off level rather than dead flat: enough tilt for the water solve to
		// have a direction, far too little for the sun to tell the two ends apart.
		TestCase.Terrain = MakeSnowTerrain(EMassifAnalyticTerrain::TiltedPlane, 90.0, 4.0);
		TestCase.Snow.InsolationMeltGain = InsolationMeltGain;
		TestCase.Snow.WindRedistributionStrength = 0.0;

		// Cold enough that nothing melts outside high summer, so what melt there is
		// happens squarely under the brightest slices of the year.
		TestCase.SeaLevelMeanTemperatureCelsius = 6.0;

		// The swing peaks at the solstice rather than a month after it, which puts the
		// melt season on the slices whose sun stands furthest above the reference day.
		TestCase.Environment.Climate.WarmestSlice = SnowTestMidsummerSlice;

		TestCase.PrecipitationFidelity = EMassifFidelity::Cheap;
		TestCase.Environment.Precipitation.BaseAnnualMillimetres = 1200.0;
		TestCase.Environment.Precipitation.AltitudeGainMillimetresPerKilometre = 0.0;
		return TestCase;
	}

	/**
	 * A saddle steep enough to run out of release band at both ends of it.
	 *
	 * The shape carries every slope from level at its passes and summits to well
	 * past vertical-enough-to-slough at its steepest, and the datum is cold enough
	 * that every cell of it is loaded past the full-load depth — so the only thing
	 * left that can move the release across the grid is the slope.
	 */
	FSnowTestCase MakeSlopeBandCase()
	{
		FSnowTestCase TestCase;
		TestCase.Terrain = MakeSnowTerrain(EMassifAnalyticTerrain::Saddle, 0.0, 2400.0);
		TestCase.Terrain.BaseAltitudeMeters = 1300.0;
		TestCase.SeaLevelMeanTemperatureCelsius = -5.0;

		// Nothing may leave a corner of this world short of a full load: no rain
		// shadow to dry one flank out and no wind to scour a crest bare.
		TestCase.PrecipitationFidelity = EMassifFidelity::Cheap;
		TestCase.Snow.WindRedistributionStrength = 0.0;
		return TestCase;
	}

	/**
	 * A cone whose whole flank sits in the release band and whose corners lie
	 * outside it, dead level at the base altitude: release everywhere on the hill,
	 * nowhere on the ground around it, and a runout that has somewhere to stop.
	 */
	FSnowTestCase MakePathCase()
	{
		FSnowTestCase TestCase;
		TestCase.Terrain = MakeSnowTerrain(EMassifAnalyticTerrain::Cone, 0.0, 900.0);
		TestCase.Terrain.BaseAltitudeMeters = 1500.0;
		TestCase.SeaLevelMeanTemperatureCelsius = -5.0;
		TestCase.PrecipitationFidelity = EMassifFidelity::Cheap;
		TestCase.Snow.WindRedistributionStrength = 0.0;
		return TestCase;
	}

	/**
	 * A wooded hill in the release band, solved with the wind off.
	 *
	 * The wind is what vegetation otherwise reaches the snowpack through, so with it
	 * off the two solves lay down the same pack on the same ground and the canopy is
	 * the only thing between their release areas.
	 */
	FSnowTestCase MakeForestAnchorCase(EMassifFidelity VegetationFidelity)
	{
		FSnowTestCase TestCase;
		TestCase.Terrain = MakeSnowTerrain(EMassifAnalyticTerrain::Cone, 0.0, 700.0);
		TestCase.Terrain.BaseAltitudeMeters = 1200.0;
		TestCase.VegetationFidelity = VegetationFidelity;
		TestCase.WindFidelity = EMassifFidelity::Off;

		// Deep regolith and a forgiving moisture range, so what limits a stand here is
		// the climate rather than whatever ground a short droplet run happened to leave.
		TestCase.Environment.Water.BaseSoilDepthMeters = 0.8;
		TestCase.Environment.Vegetation.WetnessTolerance = 1.0;
		return TestCase;
	}

	/** Highest and lowest a channel reaches across every cell of every slice it holds. */
	struct FChannelExtremes
	{
		double Lowest = TNumericLimits<double>::Max();
		double Highest = -TNumericLimits<double>::Max();
	};

	FChannelExtremes SlicedChannelExtremes(
		const FMassifEnvironmentScheduler& Scheduler, const FName& ChannelName)
	{
		const FMassifChannelGrid& Grid = SnowGrid(Scheduler);
		FChannelExtremes Extremes;
		for (int32 TimeSlice = 0; TimeSlice < SnowTestSliceCount; ++TimeSlice)
		{
			for (int32 CellIndex = 0; CellIndex < Grid.CellCount(); ++CellIndex)
			{
				const double Value = ReadSnowCell(
					Scheduler, ChannelName, CellIndex % Grid.Width, CellIndex / Grid.Width, TimeSlice);
				Extremes.Lowest = FMath::Min(Extremes.Lowest, Value);
				Extremes.Highest = FMath::Max(Extremes.Highest, Value);
			}
		}
		return Extremes;
	}

	/** The strongest release found on ground whose slope falls in a given range. */
	struct FReleaseSample
	{
		bool bFound = false;
		double Release = 0.0;
		double SlopeDegrees = 0.0;
	};

	void ConsiderReleaseCell(
		const FMassifEnvironmentScheduler& Scheduler, int32 Column, int32 Row, double MinSlopeDegrees,
		double MaxSlopeDegrees, FReleaseSample& InOutStrongest)
	{
		const double SlopeDegrees = SnowSlopeDegrees(Scheduler, Column, Row);
		if (SlopeDegrees < MinSlopeDegrees || SlopeDegrees > MaxSlopeDegrees)
		{
			return;
		}

		const double Release =
			ReadSnowCell(Scheduler, MassifEnvironmentChannels::AvalancheRelease(), Column, Row);
		if (InOutStrongest.bFound && Release <= InOutStrongest.Release)
		{
			return;
		}

		InOutStrongest.bFound = true;
		InOutStrongest.Release = Release;
		InOutStrongest.SlopeDegrees = SlopeDegrees;
	}

	/** Border cells are skipped: their slope is differenced against a clamped neighbour. */
	FReleaseSample StrongestReleaseBetween(
		const FMassifEnvironmentScheduler& Scheduler, double MinSlopeDegrees, double MaxSlopeDegrees)
	{
		const FMassifChannelGrid& Grid = SnowGrid(Scheduler);
		FReleaseSample Strongest;
		for (int32 Row = 1; Row < Grid.Height - 1; ++Row)
		{
			for (int32 Column = 1; Column < Grid.Width - 1; ++Column)
			{
				ConsiderReleaseCell(Scheduler, Column, Row, MinSlopeDegrees, MaxSlopeDegrees, Strongest);
			}
		}
		return Strongest;
	}

	/** What the traced paths cover, against the release areas they came off. */
	struct FPathExtent
	{
		double StrongestPath = 0.0;
		double HighestPathAltitudeMetres = 0.0;
		double HighestReleaseAltitudeMetres = 0.0;

		/** Ground well beyond the foot of the hill, which nothing should still be running over. */
		int32 FarFlatCellCount = 0;
		double StrongestPathOnFarFlatGround = 0.0;
	};

	void ConsiderPathCell(
		const FMassifEnvironmentScheduler& Scheduler, int32 Column, int32 Row, double MarkedThreshold,
		double FarFlatRadiusMeters, FPathExtent& InOutExtent)
	{
		const FVector2d Position = SnowGrid(Scheduler).CellCentreMeters(Column, Row);
		const double AltitudeMetres =
			SampleSnowChannel(Scheduler, MassifEnvironmentChannels::Altitude(), Position);

		const double Path = ReadSnowCell(Scheduler, MassifEnvironmentChannels::AvalanchePath(), Column, Row);
		const double Release =
			ReadSnowCell(Scheduler, MassifEnvironmentChannels::AvalancheRelease(), Column, Row);

		InOutExtent.StrongestPath = FMath::Max(InOutExtent.StrongestPath, Path);
		if (Release > MarkedThreshold)
		{
			InOutExtent.HighestReleaseAltitudeMetres =
				FMath::Max(InOutExtent.HighestReleaseAltitudeMetres, AltitudeMetres);
		}
		if (Path > MarkedThreshold)
		{
			InOutExtent.HighestPathAltitudeMetres =
				FMath::Max(InOutExtent.HighestPathAltitudeMetres, AltitudeMetres);
		}
		if (Position.Length() > FarFlatRadiusMeters)
		{
			++InOutExtent.FarFlatCellCount;
			InOutExtent.StrongestPathOnFarFlatGround =
				FMath::Max(InOutExtent.StrongestPathOnFarFlatGround, Path);
		}
	}

	FPathExtent MeasurePathExtent(
		const FMassifEnvironmentScheduler& Scheduler, double MarkedThreshold, double FarFlatRadiusMeters)
	{
		const FMassifChannelGrid& Grid = SnowGrid(Scheduler);
		FPathExtent Extent;
		for (int32 Row = 0; Row < Grid.Height; ++Row)
		{
			for (int32 Column = 0; Column < Grid.Width; ++Column)
			{
				ConsiderPathCell(Scheduler, Column, Row, MarkedThreshold, FarFlatRadiusMeters, Extent);
			}
		}
		return Extent;
	}

	/** The cell where a stand and a release area overlap most strongly, which is where anchoring shows. */
	struct FAnchorCell
	{
		int32 Column = 0;
		int32 Row = 0;
		double Canopy = 0.0;
		double BareRelease = 0.0;

		double Overlap() const
		{
			return Canopy * BareRelease;
		}
	};

	void ConsiderAnchorCell(
		const FMassifEnvironmentScheduler& Bare, const FMassifEnvironmentScheduler& Wooded, int32 Column,
		int32 Row, FAnchorCell& InOutStrongest)
	{
		FAnchorCell Candidate;
		Candidate.Column = Column;
		Candidate.Row = Row;
		Candidate.Canopy = SampleSnowChannel(
			Wooded, MassifEnvironmentChannels::VegetationCanopy(), SnowGrid(Wooded).CellCentreMeters(Column, Row));
		Candidate.BareRelease = ReadSnowCell(Bare, MassifEnvironmentChannels::AvalancheRelease(), Column, Row);

		if (Candidate.Overlap() > InOutStrongest.Overlap())
		{
			InOutStrongest = Candidate;
		}
	}

	FAnchorCell FindAnchorCell(
		const FMassifEnvironmentScheduler& Bare, const FMassifEnvironmentScheduler& Wooded)
	{
		const FMassifChannelGrid& Grid = SnowGrid(Bare);
		FAnchorCell Strongest;
		for (int32 Row = 0; Row < Grid.Height; ++Row)
		{
			for (int32 Column = 0; Column < Grid.Width; ++Column)
			{
				ConsiderAnchorCell(Bare, Wooded, Column, Row, Strongest);
			}
		}
		return Strongest;
	}

	/** Whether the scheduler put two processes in one relaxation group, which is what a declared cycle becomes. */
	bool SharesFeedbackGroup(
		const FMassifEnvironmentScheduler& Scheduler, const FName& FirstProcess, const FName& SecondProcess)
	{
		for (const TArray<FName>& Group : Scheduler.GetFeedbackGroups())
		{
			if (Group.Contains(FirstProcess) && Group.Contains(SecondProcess))
			{
				return true;
			}
		}
		return false;
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
	FSnowTestCase Warm;
	Warm.SeaLevelMeanTemperatureCelsius = 40.0;

	const TUniquePtr<FMassifEnvironmentScheduler> FrozenWorld = SolveSnow(MakeFrozenSnowCase());
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSnowSeasonalMeltTest,
	"Massif.WorldGen.Snow.MeltFollowsTheSeasonsSun",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnowSeasonalMeltTest::RunTest(const FString& Parameters)
{
	// The melt used to read Sun.DirectInsolation without a slice index. That meant
	// "the one slice" while the sun was a single surface; since the sun was sliced
	// it means the first half of January, so every half-month of melt ran under a
	// midwinter sun that no melt ever happens under.
	const TUniquePtr<FMassifEnvironmentScheduler> Blind = SolveSnow(MakeSeasonalMeltCase(0.0));
	const TUniquePtr<FMassifEnvironmentScheduler> Sunlit = SolveSnow(MakeSeasonalMeltCase(1.5));

	const FMassifChannelGrid& Grid = SnowGrid(*Sunlit);
	const int32 Column = Grid.Width / 2;
	const int32 Row = Grid.Height / 2;
	const FVector2d Position = Grid.CellCentreMeters(Column, Row);

	// Read out of the sun channel rather than assumed: the whole claim rests on the
	// two slices being different numbers on opposite sides of the reference day.
	const double WinterInsolation = SampleSnowChannel(
		*Sunlit, MassifEnvironmentChannels::DirectInsolation(), Position, SnowTestMidwinterSlice);
	const double SummerInsolation = SampleSnowChannel(
		*Sunlit, MassifEnvironmentChannels::DirectInsolation(), Position, SnowTestMidsummerSlice);
	TestTrue(
		FString::Printf(TEXT("January stands below the reference day and July above it (%.3f against %.3f)"),
			WinterInsolation, SummerInsolation),
		WinterInsolation < 1.0 && SummerInsolation > 1.0);

	const double BlindMetres = YearDepthMetresAtCell(*Blind, Column, Row);
	const double SunlitMetres = YearDepthMetresAtCell(*Sunlit, Column, Row);
	TestTrue(
		FString::Printf(TEXT("There is a pack for the sun to take (%.2f m summed over the year)"), BlindMetres),
		BlindMetres > 0.0);

	// Turning the sun term up has to melt more, because the slices the melt happens
	// in are the ones standing above the reference day. A melt reading January would
	// scale that same melt by well under one and leave a deeper pack, not a
	// shallower one, so the sign of this comparison is the bug.
	TestTrue(
		FString::Printf(TEXT("The season's own sun melts more, not less (%.3f m against %.3f m over the year)"),
			SunlitMetres, BlindMetres),
		SunlitMetres < BlindMetres - SnowQuantisationStep(*Sunlit, MassifEnvironmentChannels::SnowDepth()));
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

	// Read out of the sun channel at the slice the melting is done in, rather than
	// at whichever slice happened to be first: a shading claim about a summer melt
	// is only worth making about the summer sun.
	const double SouthInsolation = SampleSnowChannel(
		*Ridge, MassifEnvironmentChannels::DirectInsolation(), SouthPosition, SnowTestMidsummerSlice);
	const double NorthInsolation = SampleSnowChannel(
		*Ridge, MassifEnvironmentChannels::DirectInsolation(), NorthPosition, SnowTestMidsummerSlice);
	TestTrue(
		FString::Printf(TEXT("The south flank takes the more summer sun of the two (%.3f against %.3f)"),
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSnowDensityTest,
	"Massif.WorldGen.Snow.Density",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnowDensityTest::RunTest(const FString& Parameters)
{
	// A pack under its own weight only ever goes one way. Through a winter that
	// never melts out, each half-month mixes a little fresh snow into what is
	// already lying and settles the result further, so the density at the end of the
	// year stands above the density at the start of it.
	const TUniquePtr<FMassifEnvironmentScheduler> Frozen = SolveSnow(MakeFrozenSnowCase());

	const FMassifChannelGrid& Grid = SnowGrid(*Frozen);
	const int32 Column = Grid.Width / 2;
	const int32 Row = Grid.Height / 2;

	const FName& Density = MassifEnvironmentChannels::SnowDensity();
	const double FirstSlice = ReadSnowCell(*Frozen, Density, Column, Row, 0);
	const double LastSlice = ReadSnowCell(*Frozen, Density, Column, Row, SnowTestSliceCount - 1);
	TestTrue(
		FString::Printf(TEXT("The pack settles through the year (%.1f kg/m3 against %.1f kg/m3)"),
			LastSlice, FirstSlice),
		LastSlice > FirstSlice + SnowQuantisationStep(*Frozen, Density));

	// Snow cannot fall lighter than fresh nor pack tighter than settled, whatever
	// the year does to it — including the year that melts out and starts again.
	const FMassifSnowParams Defaults;
	const TUniquePtr<FMassifEnvironmentScheduler> Ridge = SolveSnow(MakeAspectCase());
	const FChannelExtremes Extremes = SlicedChannelExtremes(*Ridge, Density);
	const double ToleranceKilogramsPerCubicMetre = SnowQuantisationStep(*Ridge, Density);

	TestTrue(
		FString::Printf(TEXT("Nothing lies lighter than fresh snow (%.1f kg/m3)"), Extremes.Lowest),
		Extremes.Lowest
			>= Defaults.FreshSnowDensityKilogramsPerCubicMetre - ToleranceKilogramsPerCubicMetre);
	TestTrue(
		FString::Printf(TEXT("And nothing packs tighter than settled snow (%.1f kg/m3)"), Extremes.Highest),
		Extremes.Highest
			<= Defaults.SettledSnowDensityKilogramsPerCubicMetre + ToleranceKilogramsPerCubicMetre);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSnowAlbedoTest,
	"Massif.WorldGen.Snow.Albedo",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnowAlbedoTest::RunTest(const FString& Parameters)
{
	// Fresh snow is nearly white and a settled pack is not, so the same ground under
	// the same depth throws back less light in April than it did in November.
	const TUniquePtr<FMassifEnvironmentScheduler> Frozen = SolveSnow(MakeFrozenSnowCase());

	const FMassifChannelGrid& Grid = SnowGrid(*Frozen);
	const int32 Column = Grid.Width / 2;
	const int32 Row = Grid.Height / 2;

	const FName& Albedo = MassifEnvironmentChannels::SnowAlbedo();
	const double FirstSlice = ReadSnowCell(*Frozen, Albedo, Column, Row, 0);
	const double LastSlice = ReadSnowCell(*Frozen, Albedo, Column, Row, SnowTestSliceCount - 1);
	TestTrue(
		FString::Printf(TEXT("The pack darkens as it settles (%.3f against %.3f)"), LastSlice, FirstSlice),
		LastSlice < FirstSlice - SnowQuantisationStep(*Frozen, Albedo));

	// And where nothing lies at all it is the ground itself being read, not a pack
	// of no depth.
	FSnowTestCase Warm;
	Warm.SeaLevelMeanTemperatureCelsius = 40.0;
	const TUniquePtr<FMassifEnvironmentScheduler> WarmWorld = SolveSnow(Warm);

	const FMassifSnowParams Defaults;
	const FChannelExtremes Extremes = SlicedChannelExtremes(*WarmWorld, Albedo);
	const double Tolerance = SnowQuantisationStep(*WarmWorld, Albedo);
	TestTrue(
		FString::Printf(TEXT("Bare ground reads its own albedo throughout (%.3f to %.3f against %.3f)"),
			Extremes.Lowest, Extremes.Highest, Defaults.BareGroundAlbedo),
		FMath::Abs(Extremes.Lowest - Defaults.BareGroundAlbedo) <= Tolerance
			&& FMath::Abs(Extremes.Highest - Defaults.BareGroundAlbedo) <= Tolerance);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSnowAlbedoFeedbackTest,
	"Massif.WorldGen.Snow.AlbedoFeedback",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnowAlbedoFeedbackTest::RunTest(const FString& Parameters)
{
	// Snow keeps its own ground cold: it throws back the sunlight that would have
	// warmed the soil under it. That makes the seasonal temperature part of a cycle
	// with the pack it decides, which the scheduler has to have solved as one group
	// rather than refused to order.
	FSnowTestCase Bare = MakeFrozenSnowCase();
	Bare.SnowFidelity = EMassifFidelity::Off;

	const TUniquePtr<FMassifEnvironmentScheduler> Snowy = SolveSnow(MakeFrozenSnowCase());
	const TUniquePtr<FMassifEnvironmentScheduler> Unsolved = SolveSnow(Bare);

	TestTrue(
		TEXT("The temperature and the pack are solved as one loop"),
		SharesFeedbackGroup(
			*Snowy, MassifEnvironmentProcesses::SeasonalClimate(), MassifEnvironmentProcesses::Snowpack()));

	const FMassifChannelGrid& Grid = SnowGrid(*Snowy);
	const int32 Column = Grid.Width / 2;
	const int32 Row = Grid.Height / 2;
	const FVector2d Position = Grid.CellCentreMeters(Column, Row);

	const double PeakDepthMetres =
		ReadSnowCell(*Snowy, MassifEnvironmentChannels::PeakSnowDepth(), Column, Row);
	TestTrue(
		FString::Printf(TEXT("There is deep snow lying here to reflect anything (%.2f m)"), PeakDepthMetres),
		PeakDepthMetres > 0.0);

	// Read both back rather than assuming how far it moves: what is being asserted
	// is that the loop closed at all, not what the gain of it is.
	const FName& Seasonal = MassifEnvironmentChannels::SeasonalTemperature();
	const double SnowyCelsius = SampleSnowChannel(*Snowy, Seasonal, Position, SnowTestMidwinterSlice);
	const double UnsolvedCelsius = SampleSnowChannel(*Unsolved, Seasonal, Position, SnowTestMidwinterSlice);
	TestTrue(
		FString::Printf(TEXT("Ground under snow runs the colder of the two (%.3f C against %.3f C)"),
			SnowyCelsius, UnsolvedCelsius),
		SnowyCelsius < UnsolvedCelsius - SnowQuantisationStep(*Snowy, Seasonal));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSnowReleaseSlopeBandTest,
	"Massif.WorldGen.Snow.ReleaseSlopeBand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnowReleaseSlopeBandTest::RunTest(const FString& Parameters)
{
	// A slab needs ground steep enough to slide and shallow enough to hold snow long
	// enough to become one. Below the band snow simply sits where it fell; above it
	// the ground sheds every load as it arrives and never builds a slab at all.
	const TUniquePtr<FMassifEnvironmentScheduler> Saddle = SolveSnow(MakeSlopeBandCase());

	const FMassifSnowParams Defaults;
	const FMassifEnvironmentState& State = Saddle->GetState();
	const double ShallowestPackMetres =
		State.GetChannelMinimum(SnowChannel(*Saddle, MassifEnvironmentChannels::PeakSnowDepth()));
	TestTrue(
		FString::Printf(TEXT("Every cell of this world is loaded past the full-load depth (%.2f m)"),
			ShallowestPackMetres),
		ShallowestPackMetres > Defaults.ReleaseFullLoadDepthMetres);

	const double Tolerance = SnowQuantisationStep(*Saddle, MassifEnvironmentChannels::AvalancheRelease());

	const FReleaseSample Gentle = StrongestReleaseBetween(*Saddle, 0.0, Defaults.MinReleaseSlopeDegrees - 3.0);
	TestTrue(TEXT("The world holds ground gentler than the band"), Gentle.bFound);
	TestTrue(
		FString::Printf(TEXT("Nothing releases below the band (%.4f at %.1f degrees)"),
			Gentle.Release, Gentle.SlopeDegrees),
		Gentle.Release <= Tolerance);

	const FReleaseSample Cliff = StrongestReleaseBetween(*Saddle, Defaults.MaxReleaseSlopeDegrees + 3.0, 90.0);
	TestTrue(TEXT("The world holds ground steeper than the band"), Cliff.bFound);
	TestTrue(
		FString::Printf(TEXT("Nor above it (%.4f at %.1f degrees)"), Cliff.Release, Cliff.SlopeDegrees),
		Cliff.Release <= Tolerance);

	const FReleaseSample Band = StrongestReleaseBetween(
		*Saddle, Defaults.MinReleaseSlopeDegrees + 5.0, Defaults.MaxReleaseSlopeDegrees - 5.0);
	TestTrue(TEXT("The world holds ground inside the band"), Band.bFound);
	TestTrue(
		FString::Printf(TEXT("And the band between them releases readily (%.3f at %.1f degrees)"),
			Band.Release, Band.SlopeDegrees),
		Band.Release > 0.5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSnowForestAnchorTest,
	"Massif.WorldGen.Snow.ForestAnchorsSnow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnowForestAnchorTest::RunTest(const FString& Parameters)
{
	// Trunks pin the layers of a slab to the ground they stand on, which is why an
	// avalanche track is a stripe through a forest rather than over one. With the
	// wind off, the canopy is the only thing vegetation can reach the pack through.
	const TUniquePtr<FMassifEnvironmentScheduler> Bare =
		SolveSnow(MakeForestAnchorCase(EMassifFidelity::Off));
	const TUniquePtr<FMassifEnvironmentScheduler> Wooded =
		SolveSnow(MakeForestAnchorCase(EMassifFidelity::Standard));

	const FAnchorCell Anchor = FindAnchorCell(*Bare, *Wooded);
	TestTrue(
		FString::Printf(TEXT("A stand grows on ground that would otherwise release (%.3f canopy, %.3f release)"),
			Anchor.Canopy, Anchor.BareRelease),
		Anchor.Overlap() > 0.0);

	// The two solves have to be laying down the same pack for the comparison to be
	// about the trees: the wind is off, so nothing the vegetation does can reach it.
	const FName& PeakDepth = MassifEnvironmentChannels::PeakSnowDepth();
	const double BareDepthMetres = ReadSnowCell(*Bare, PeakDepth, Anchor.Column, Anchor.Row);
	const double WoodedDepthMetres = ReadSnowCell(*Wooded, PeakDepth, Anchor.Column, Anchor.Row);
	const double DepthToleranceMetres =
		SnowQuantisationStep(*Bare, PeakDepth) + SnowQuantisationStep(*Wooded, PeakDepth);
	TestTrue(
		FString::Printf(TEXT("Both solves lie under the same pack (%.4f m against %.4f m)"),
			WoodedDepthMetres, BareDepthMetres),
		FMath::Abs(WoodedDepthMetres - BareDepthMetres) <= DepthToleranceMetres);

	const FName& Release = MassifEnvironmentChannels::AvalancheRelease();
	const double WoodedRelease = ReadSnowCell(*Wooded, Release, Anchor.Column, Anchor.Row);
	TestTrue(
		FString::Printf(TEXT("And the wooded one lets go the less readily (%.4f against %.4f)"),
			WoodedRelease, Anchor.BareRelease),
		WoodedRelease < Anchor.BareRelease - SnowQuantisationStep(*Wooded, Release));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSnowAvalanchePathTest,
	"Massif.WorldGen.Snow.AvalanchePath",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnowAvalanchePathTest::RunTest(const FString& Parameters)
{
	// A path is where the slabs above it went, so it lies below its release areas
	// and it ends where the hill does. The corners of this cone are dead level, and
	// nothing should still be running once it is out there.
	const TUniquePtr<FMassifEnvironmentScheduler> Cone = SolveSnow(MakePathCase());

	const FMassifChannelGrid& Grid = SnowGrid(*Cone);
	const double MarkedThreshold = SnowQuantisationStep(*Cone, MassifEnvironmentChannels::AvalanchePath());

	// Four cells clear of the foot of the cone, so the ring the runout stops in is
	// not counted as ground it ran across.
	const double FarFlatRadiusMeters = 0.5 * SnowTestWorldMeters + 4.0 * Grid.CellSizeMeters;
	const FPathExtent Extent = MeasurePathExtent(*Cone, MarkedThreshold, FarFlatRadiusMeters);

	TestTrue(
		FString::Printf(TEXT("The release areas run somewhere (%.3f at the strongest)"), Extent.StrongestPath),
		Extent.StrongestPath > MarkedThreshold);
	TestTrue(
		FString::Printf(TEXT("And every one of them runs downhill (%.1f m against a release at %.1f m)"),
			Extent.HighestPathAltitudeMetres, Extent.HighestReleaseAltitudeMetres),
		Extent.HighestPathAltitudeMetres < Extent.HighestReleaseAltitudeMetres);

	TestTrue(
		FString::Printf(TEXT("There is level ground beyond the foot of the hill (%d cells)"),
			Extent.FarFlatCellCount),
		Extent.FarFlatCellCount > 0);
	TestTrue(
		FString::Printf(TEXT("Nothing is still running over it (%.4f)"), Extent.StrongestPathOnFarFlatGround),
		Extent.StrongestPathOnFarFlatGround <= MarkedThreshold);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSnowOffTest,
	"Massif.WorldGen.Snow.Off",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnowOffTest::RunTest(const FString& Parameters)
{
	// Off is bare ground all year, not an absent channel: nothing downstream has to
	// ask whether the pack was solved before reading it. Every channel reads empty
	// rather than plausible, including the density and the albedo — a bare-ground
	// albedo here would be a claim the temperature that reads it would act on.
	FSnowTestCase Bare;
	Bare.SnowFidelity = EMassifFidelity::Off;

	// Cold enough that a solved pack would bury the world, so an empty channel here
	// can only be the fidelity and not the weather.
	Bare.SeaLevelMeanTemperatureCelsius = -20.0;

	const TUniquePtr<FMassifEnvironmentScheduler> Unsolved = SolveSnow(Bare);
	const FMassifEnvironmentState& State = Unsolved->GetState();

	const FName Names[] = {
		MassifEnvironmentChannels::SnowDepth(), MassifEnvironmentChannels::PeakSnowDepth(),
		MassifEnvironmentChannels::SnowCoverDuration(), MassifEnvironmentChannels::SnowDensity(),
		MassifEnvironmentChannels::SnowAlbedo(), MassifEnvironmentChannels::AvalancheRelease(),
		MassifEnvironmentChannels::AvalanchePath()};
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
