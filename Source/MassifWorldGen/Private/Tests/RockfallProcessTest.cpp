#include "MassifElevationSource.h"
#include "MassifEnvironmentGraph.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifRockfallProcesses.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr uint32 RockfallTestSeed = 20260824;
	constexpr double RockfallTestWorldMeters = 2048.0;

	/**
	 * Steep enough that the cone's flanks are well over the release threshold.
	 *
	 * The analytic cone falls off linearly to its inscribed circle, so its flanks
	 * are one constant slope and the four corners outside that circle are flat.
	 * That is the shape this suite needs: ground that sheds, and ground beyond it
	 * that blocks can run out onto and stop.
	 */
	constexpr double RockfallTestReliefMeters = 1600.0;

	FMassifAnalyticElevationSource::FParams MakeRockfallTerrain(
		EMassifAnalyticTerrain Shape, double ReliefMeters = RockfallTestReliefMeters)
	{
		FMassifAnalyticElevationSource::FParams Params;
		Params.Shape = Shape;
		Params.WorldSizeMeters = RockfallTestWorldMeters;
		Params.BaseAltitudeMeters = 1500.0;
		Params.ReliefMeters = ReliefMeters;
		return Params;
	}

	FMassifRockfallParams MakeRockfallParams()
	{
		FMassifRockfallParams Params;

		// Dense over a small test world, so a run places thousands of blocks and the
		// population statistics below are not read off a handful of them.
		Params.CandidatesPerSquareKilometre = 200000.0;
		Params.TileSizeMeters = 256.0;
		return Params;
	}

	/**
	 * The rockfall over bare terrain.
	 *
	 * Nothing in this graph produces soil, rock or canopy, so the fall is shaped by
	 * the ground alone — which is the arrangement that makes what the test measures
	 * a property of the fall rather than of the rest of the bake.
	 */
	TUniquePtr<FMassifEnvironmentScheduler> SolveRockfall(
		const FMassifAnalyticElevationSource::FParams& TerrainParams, EMassifFidelity Fidelity,
		uint32 MasterSeed = RockfallTestSeed)
	{
		TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MakeUnique<FMassifEnvironmentScheduler>(
			MakeShared<FMassifAnalyticElevationSource>(TerrainParams), FMassifGeoreference(), MasterSeed);

		MassifRegisterRockfallProcesses(*Scheduler, MakeRockfallParams());

		FString Error;
		checkf(Scheduler->BuildSchedule(Error), TEXT("%s"), *Error);
		Scheduler->SetFidelity(MassifEnvironmentProcesses::Rockfall(), Fidelity);
		Scheduler->Execute();
		return Scheduler;
	}

	TArray<FMassifEnvironmentPoint> ReadPlacements(const FMassifEnvironmentState& State)
	{
		const FMassifPointSetHandle PointSet =
			State.FindPointSet(MassifEnvironmentPointSets::RockPlacements());
		check(PointSet.IsValid());

		const FMassifChannelGrid& TileGrid = State.GetPointSetTileGrid(PointSet);
		TArray<FMassifEnvironmentPoint> AllPoints;
		TArray<FMassifEnvironmentPoint> TilePoints;
		for (int32 Row = 0; Row < TileGrid.Height; ++Row)
		{
			for (int32 Column = 0; Column < TileGrid.Width; ++Column)
			{
				State.ReadTilePoints(PointSet, Column, Row, TilePoints);
				AllPoints.Append(TilePoints);
			}
		}
		return AllPoints;
	}

	double SlopeDegreesAt(const IMassifElevationSource& Elevation, FVector2d PositionMeters, double Spacing)
	{
		const double East = Elevation.SampleAltitudeMeters(PositionMeters + FVector2d(Spacing, 0.0), Spacing);
		const double West = Elevation.SampleAltitudeMeters(PositionMeters - FVector2d(Spacing, 0.0), Spacing);
		const double North = Elevation.SampleAltitudeMeters(PositionMeters + FVector2d(0.0, Spacing), Spacing);
		const double South = Elevation.SampleAltitudeMeters(PositionMeters - FVector2d(0.0, Spacing), Spacing);
		const FVector2d Gradient((East - West) / (2.0 * Spacing), (North - South) / (2.0 * Spacing));
		return FMath::RadiansToDegrees(FMath::Atan(Gradient.Length()));
	}

	double MeanAltitudeOf(TConstArrayView<FMassifEnvironmentPoint> Points)
	{
		if (Points.IsEmpty())
		{
			return 0.0;
		}

		double Total = 0.0;
		for (const FMassifEnvironmentPoint& Point : Points)
		{
			Total += Point.PositionMeters.Z;
		}
		return Total / Points.Num();
	}

	/**
	 * Mean distance from the summit, which on this cone is how far a block ran.
	 *
	 * Altitude cannot say it: the flanks are one constant slope down to flat
	 * corners, so every block that made it off the cone stopped at the same height
	 * whatever distance it covered getting there.
	 */
	double MeanDistanceFromCentre(TConstArrayView<FMassifEnvironmentPoint> Points)
	{
		if (Points.IsEmpty())
		{
			return 0.0;
		}

		double Total = 0.0;
		for (const FMassifEnvironmentPoint& Point : Points)
		{
			Total += FVector2d(Point.PositionMeters.X, Point.PositionMeters.Y).Length();
		}
		return Total / Points.Num();
	}

	/** Total volume of stone the deposit channel is holding, in cubic metres. */
	double TotalDepositVolume(const FMassifEnvironmentState& State)
	{
		const FMassifChannelHandle Deposit = State.FindChannel(MassifEnvironmentChannels::RockfallDeposit());
		check(Deposit.IsValid());

		const FMassifChannelGrid& Grid = State.GetGrid(Deposit);
		const double CellArea = Grid.CellSizeMeters * Grid.CellSizeMeters;

		double Total = 0.0;
		for (int32 Row = 0; Row < Grid.Height; ++Row)
		{
			for (int32 Column = 0; Column < Grid.Width; ++Column)
			{
				Total += State.ReadCell(Deposit, Column, Row) * CellArea;
			}
		}
		return Total;
	}

	double TotalBlockVolume(TConstArrayView<FMassifEnvironmentPoint> Points)
	{
		double Total = 0.0;
		for (const FMassifEnvironmentPoint& Point : Points)
		{
			Total += 4.0 / 3.0 * UE_DOUBLE_PI * FMath::Pow(Point.RadiusMeters, 3.0);
		}
		return Total;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRockfallRunsDownhillTest,
	"Massif.WorldGen.Rockfall.RunsDownhill",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRockfallRunsDownhillTest::RunTest(const FString& Parameters)
{
	const FMassifAnalyticElevationSource::FParams TerrainParams = MakeRockfallTerrain(EMassifAnalyticTerrain::Cone);
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler =
		SolveRockfall(TerrainParams, EMassifFidelity::Standard);

	const TArray<FMassifEnvironmentPoint> Placements = ReadPlacements(Scheduler->GetState());
	if (!TestTrue(TEXT("A steep cone places blocks"), Placements.Num() > 100))
	{
		return false;
	}

	// The claim the whole pass rests on: a block does not stay on the face it broke
	// off. Ground steeper than the repose angle is ground a block runs off, so what
	// is standing at rest on it should be a small tail at most.
	const FMassifAnalyticElevationSource Elevation(TerrainParams);
	const FMassifRockfallParams RockfallParams = MakeRockfallParams();

	int32 RestingOnSteepGround = 0;
	int32 OutsideWorld = 0;
	for (const FMassifEnvironmentPoint& Point : Placements)
	{
		const FVector2d Position(Point.PositionMeters.X, Point.PositionMeters.Y);
		const double SlopeDegrees = SlopeDegreesAt(Elevation, Position, RockfallParams.StepLengthMeters);
		RestingOnSteepGround += SlopeDegrees > RockfallParams.MinReleaseSlopeDegrees ? 1 : 0;
		OutsideWorld += FMath::Abs(Position.X) > 0.5 * RockfallTestWorldMeters
			|| FMath::Abs(Position.Y) > 0.5 * RockfallTestWorldMeters ? 1 : 0;
	}

	const double SteepShare = static_cast<double>(RestingOnSteepGround) / Placements.Num();
	TestTrue(FString::Printf(TEXT("Blocks run off the faces they release from (%.1f%% left on one)"),
		100.0 * SteepShare), SteepShare < 0.1);
	TestEqual(TEXT("No block comes to rest outside the world"), OutsideWorld, 0);

	// A cone releases from its flanks and collects at its foot, so the blocks must
	// sit below the middle of the relief they came off.
	const double MeanAltitude = MeanAltitudeOf(Placements);
	const double SummitAltitude = TerrainParams.BaseAltitudeMeters + TerrainParams.ReliefMeters;
	TestTrue(FString::Printf(TEXT("Blocks pile low on the cone (mean %.0f m of %.0f-%.0f m)"),
		MeanAltitude, TerrainParams.BaseAltitudeMeters, SummitAltitude),
		MeanAltitude < 0.5 * (TerrainParams.BaseAltitudeMeters + SummitAltitude));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRockfallSortsTheConeTest,
	"Massif.WorldGen.Rockfall.SortsTheCone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRockfallSortsTheConeTest::RunTest(const FString& Parameters)
{
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler =
		SolveRockfall(MakeRockfallTerrain(EMassifAnalyticTerrain::Cone), EMassifFidelity::Standard);
	const TArray<FMassifEnvironmentPoint> Placements = ReadPlacements(Scheduler->GetState());

	// A big block carries more energy against the same roughness, so it runs
	// further: the toe of a talus cone is coarse and the apex is fine.
	const FMassifRockfallParams Params = MakeRockfallParams();
	const double SplitRadius = FMath::Lerp(Params.MinBlockRadiusMeters, Params.MaxBlockRadiusMeters, 0.25);

	TArray<FMassifEnvironmentPoint> Small;
	TArray<FMassifEnvironmentPoint> Large;
	for (const FMassifEnvironmentPoint& Point : Placements)
	{
		TArray<FMassifEnvironmentPoint>& Bucket = Point.RadiusMeters < SplitRadius ? Small : Large;
		Bucket.Add(Point);
	}

	if (!TestTrue(TEXT("Both size classes are populated"), Small.Num() > 50 && Large.Num() > 50))
	{
		return false;
	}

	const double SmallDistance = MeanDistanceFromCentre(Small);
	const double LargeDistance = MeanDistanceFromCentre(Large);
	TestTrue(FString::Printf(TEXT("Large blocks outrun small ones (%.1f m against %.1f m)"),
		LargeDistance, SmallDistance), LargeDistance > SmallDistance);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRockfallDepositMatchesBlocksTest,
	"Massif.WorldGen.Rockfall.DepositMatchesBlocks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRockfallDepositMatchesBlocksTest::RunTest(const FString& Parameters)
{
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler =
		SolveRockfall(MakeRockfallTerrain(EMassifAnalyticTerrain::Cone), EMassifFidelity::Standard);
	const FMassifEnvironmentState& State = Scheduler->GetState();
	const TArray<FMassifEnvironmentPoint> Placements = ReadPlacements(State);

	// The field and the objects are the same blocks reduced two ways. If they can
	// drift apart, the scree the player walks on is not the scree the graph reads.
	const double BlockVolume = TotalBlockVolume(Placements);
	const double DepositVolume = TotalDepositVolume(State);
	if (!TestTrue(TEXT("Blocks were placed"), BlockVolume > 0.0))
	{
		return false;
	}

	// Tolerance is the deposit channel's own quantisation, not the model's.
	const double Error = FMath::Abs(DepositVolume - BlockVolume) / BlockVolume;
	TestTrue(FString::Printf(TEXT("Deposit carries the blocks' volume (%.3f%% out)"), 100.0 * Error),
		Error < 0.02);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRockfallFlatWorldTest,
	"Massif.WorldGen.Rockfall.FlatWorldShedsNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRockfallFlatWorldTest::RunTest(const FString& Parameters)
{
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler =
		SolveRockfall(MakeRockfallTerrain(EMassifAnalyticTerrain::Flat, 0.0), EMassifFidelity::Standard);
	const FMassifEnvironmentState& State = Scheduler->GetState();

	const FMassifPointSetHandle PointSet = State.FindPointSet(MassifEnvironmentPointSets::RockPlacements());
	TestEqual(TEXT("Flat ground places no blocks"), State.GetPointCount(PointSet), 0);
	TestEqual(TEXT("Flat ground holds no deposit"), TotalDepositVolume(State), 0.0);

	// The set still exists and reads empty, so nothing downstream branches on
	// whether this world had cliffs in it.
	TestTrue(TEXT("The point set was still produced"), PointSet.IsValid());
	TestTrue(TEXT("The point set was written"), State.IsPointSetWritten(PointSet));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRockfallOffTest,
	"Massif.WorldGen.Rockfall.Off",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRockfallOffTest::RunTest(const FString& Parameters)
{
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler =
		SolveRockfall(MakeRockfallTerrain(EMassifAnalyticTerrain::Cone), EMassifFidelity::Off);
	const FMassifEnvironmentState& State = Scheduler->GetState();

	const FMassifPointSetHandle PointSet = State.FindPointSet(MassifEnvironmentPointSets::RockPlacements());
	TestTrue(TEXT("Off still produces the point set"), PointSet.IsValid() && State.IsPointSetWritten(PointSet));
	TestEqual(TEXT("Off places no blocks"), State.GetPointCount(PointSet), 0);

	TestTrue(TEXT("Off still produces the deposit channel"),
		State.FindChannel(MassifEnvironmentChannels::RockfallDeposit()).IsValid());
	TestEqual(TEXT("Off leaves no deposit"), TotalDepositVolume(State), 0.0);

	// Off must not depend on the seed, or two machines would disagree about a world
	// in which nothing falls.
	const TUniquePtr<FMassifEnvironmentScheduler> OtherSeed =
		SolveRockfall(MakeRockfallTerrain(EMassifAnalyticTerrain::Cone), EMassifFidelity::Off, RockfallTestSeed + 1);
	TestEqual(TEXT("Off is the same world on any seed"),
		OtherSeed->GetState().ComputeContentHash(), State.ComputeContentHash());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRockfallDeterminismTest,
	"Massif.WorldGen.Rockfall.Determinism",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRockfallDeterminismTest::RunTest(const FString& Parameters)
{
	// The fall runs wide over candidates, so this is the test that the result does
	// not depend on which worker picked up which block.
	const TUniquePtr<FMassifEnvironmentScheduler> First =
		SolveRockfall(MakeRockfallTerrain(EMassifAnalyticTerrain::Cone), EMassifFidelity::Standard);
	const TUniquePtr<FMassifEnvironmentScheduler> Second =
		SolveRockfall(MakeRockfallTerrain(EMassifAnalyticTerrain::Cone), EMassifFidelity::Standard);

	TestEqual(TEXT("The same seed places the same blocks"),
		Second->GetState().ComputeContentHash(), First->GetState().ComputeContentHash());

	const TUniquePtr<FMassifEnvironmentScheduler> Other = SolveRockfall(
		MakeRockfallTerrain(EMassifAnalyticTerrain::Cone), EMassifFidelity::Standard, RockfallTestSeed + 1);
	TestNotEqual(TEXT("A different seed places them differently"),
		Other->GetState().ComputeContentHash(), First->GetState().ComputeContentHash());
	return true;
}
