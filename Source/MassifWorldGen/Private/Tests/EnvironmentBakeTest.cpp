#include "MassifEnvironmentBakeGrid.h"
#include "MassifEnvironmentGraph.h"
#include "MassifTestEnvironment.h"
#include "MassifHeightmap.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr uint32 BakeTestSeed = 4711;
	constexpr int32 BakeTestSide = 96;
	constexpr double BakeTestSpacingMeters = 8.0;
	constexpr double BakeTestHalfWorldMeters = 0.5 * BakeTestSide * BakeTestSpacingMeters;

	/** Easting and northing of a sample of the test heightmap, origin at the centre. */
	FVector2d TestSamplePosition(int32 Column, int32 Row)
	{
		return FVector2d(
			-BakeTestHalfWorldMeters + (Column + 0.5) * BakeTestSpacingMeters,
			BakeTestHalfWorldMeters - (Row + 0.5) * BakeTestSpacingMeters);
	}

	TSharedRef<const FMassifHeightmap> MakeBakeHeightmap(TFunctionRef<double(FVector2d)> AltitudeAt)
	{
		TArray<double> Altitudes;
		Altitudes.Reserve(BakeTestSide * BakeTestSide);
		for (int32 Row = 0; Row < BakeTestSide; ++Row)
		{
			for (int32 Column = 0; Column < BakeTestSide; ++Column)
			{
				Altitudes.Add(AltitudeAt(TestSamplePosition(Column, Row)));
			}
		}
		return FMassifHeightmap::FromAltitudes(Altitudes, BakeTestSide, BakeTestSide, BakeTestSpacingMeters);
	}

	/** A V-shaped valley on the X = 0 axis, falling south so everything it collects leaves the world. */
	TSharedRef<const FMassifHeightmap> MakeValley()
	{
		return MakeBakeHeightmap([](FVector2d Position)
		{
			return 1500.0 + FMath::Abs(Position.X) * 0.5 + Position.Y * 0.1;
		});
	}

	/** The same slope with a round pit punched into the middle of it: one hollow, and nothing else to fill. */
	TSharedRef<const FMassifHeightmap> MakePittedSlope(double PitDepthMeters, double PitRadiusMeters)
	{
		return MakeBakeHeightmap([PitDepthMeters, PitRadiusMeters](FVector2d Position)
		{
			const double PlaneAltitude = 1500.0 + Position.Y * 0.1;
			const double DistanceMeters = Position.Length();
			if (DistanceMeters >= PitRadiusMeters)
			{
				return PlaneAltitude;
			}
			const double Depth = PitDepthMeters * (1.0 - DistanceMeters / PitRadiusMeters);
			return PlaneAltitude - Depth;
		});
	}

	FMassifEnvironmentBakeParams MakeTestParams()
	{
		FMassifEnvironmentBakeParams Params;
		Params.CellSizeMeters = BakeTestSpacingMeters;
		return Params;
	}

	/** Highest of the eight neighbours' drops, negative where every neighbour is higher. */
	double SteepestDropMeters(const FMassifBakeGrid& Grid, TArrayView<const double> Filled, int32 Column, int32 Row)
	{
		double SteepestMeters = -TNumericLimits<double>::Max();
		const double CellAltitude = Filled[Grid.Index(Column, Row)];
		for (int32 Direction = 0; Direction < MassifBakeNeighbours::Count; ++Direction)
		{
			const int32 NeighbourColumn = Column + MassifBakeNeighbours::OffsetX[Direction];
			const int32 NeighbourRow = Row + MassifBakeNeighbours::OffsetY[Direction];
			if (Grid.Contains(NeighbourColumn, NeighbourRow))
			{
				SteepestMeters = FMath::Max(
					SteepestMeters, CellAltitude - Filled[Grid.Index(NeighbourColumn, NeighbourRow)]);
			}
		}
		return SteepestMeters;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentBakeDrainageTest,
	"Massif.WorldGen.EnvironmentBake.Drainage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentBakeDrainageTest::RunTest(const FString& Parameters)
{
	// The pit is the only hollow, so if the fill works every cell of the world
	// ends up with somewhere lower to send its water.
	const TSharedRef<const FMassifHeightmap> Heightmap = MakePittedSlope(40.0, 120.0);
	const FMassifEnvironmentBakeParams Params = MakeTestParams();
	const FMassifBakeGrid Grid = FMassifBakeGrid::FromHeightmap(*Heightmap, Params.CellSizeMeters);

	TArray<double> Measured;
	TArray<double> Filled;
	MassifBakeCpu::SampleAltitudes(*Heightmap, Grid, Measured);
	MassifBakeCpu::FillDepressions(Grid, Params, Measured, Filled);

	int32 CellsBelowMeasured = 0;
	int32 CellsWithNowhereToDrain = 0;
	for (int32 Row = 1; Row < Grid.Height - 1; ++Row)
	{
		for (int32 Column = 1; Column < Grid.Width - 1; ++Column)
		{
			const int32 CellIndex = Grid.Index(Column, Row);
			CellsBelowMeasured += static_cast<int32>(Filled[CellIndex] < Measured[CellIndex]);
			CellsWithNowhereToDrain +=
				static_cast<int32>(SteepestDropMeters(Grid, Filled, Column, Row) <= 0.0);
		}
	}

	TestEqual(TEXT("The filled surface never dips below the measured one"), CellsBelowMeasured, 0);
	TestEqual(TEXT("Every cell has a lower neighbour to drain into"), CellsWithNowhereToDrain, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentBakeStandingWaterTest,
	"Massif.WorldGen.EnvironmentBake.StandingWater",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentBakeStandingWaterTest::RunTest(const FString& Parameters)
{
	constexpr double PitDepthMeters = 40.0;
	const TUniquePtr<FMassifEnvironmentScheduler> Pitted =
		MassifSolveTestEnvironment(MakePittedSlope(PitDepthMeters, 120.0), BakeTestSeed, MakeTestParams());
	const FMassifEnvironmentState& PittedState = Pitted->GetState();
	const FMassifChannelHandle PittedWater =
		PittedState.FindChannel(MassifEnvironmentChannels::StandingWaterDepth());

	const double PitWaterMeters = PittedState.SampleScalar(PittedWater, FVector2d::ZeroVector);
	TestTrue(FString::Printf(TEXT("Water stands in the pit (%.1f m in a %.0f m hollow)"),
		PitWaterMeters, PitDepthMeters),
		PitWaterMeters > 0.5 * PitDepthMeters && PitWaterMeters <= PitDepthMeters + 1.0);

	const double SlopeWaterMeters = PittedState.SampleScalar(PittedWater, FVector2d(250.0, 250.0));
	TestTrue(FString::Printf(TEXT("None stands on the open slope (%.3f m)"), SlopeWaterMeters),
		SlopeWaterMeters < 0.05);

	// A clean valley has no hollow anywhere, so nothing at all should pond in it.
	const TUniquePtr<FMassifEnvironmentScheduler> Valley =
		MassifSolveTestEnvironment(MakeValley(), BakeTestSeed, MakeTestParams());
	const double DeepestValleyWaterMeters = Valley->GetState().GetChannelMaximum(
		Valley->GetState().FindChannel(MassifEnvironmentChannels::StandingWaterDepth()));
	TestTrue(FString::Printf(TEXT("A valley that drains has no standing water (max %.3f m)"),
		DeepestValleyWaterMeters),
		DeepestValleyWaterMeters < 0.05);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentBakeFlowTest,
	"Massif.WorldGen.EnvironmentBake.Flow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentBakeFlowTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifHeightmap> Heightmap = MakeValley();
	const FMassifEnvironmentBakeParams Params = MakeTestParams();
	const FMassifBakeGrid Grid = FMassifBakeGrid::FromHeightmap(*Heightmap, Params.CellSizeMeters);

	TArray<double> Measured;
	TArray<double> Filled;
	TArray<float> Accumulation;
	MassifBakeCpu::SampleAltitudes(*Heightmap, Grid, Measured);
	MassifBakeCpu::FillDepressions(Grid, Params, Measured, Filled);
	MassifBakeCpu::RouteFlow(Grid, Params, Filled, Accumulation);

	const double CellAreaSquareMeters = Grid.CellAreaSquareMeters();
	const double WorldAreaSquareMeters = CellAreaSquareMeters * Grid.CellCount();

	double LargestSquareMeters = 0.0;
	int32 CellsHoldingLessThanTheirOwnArea = 0;
	for (float CellAccumulation : Accumulation)
	{
		LargestSquareMeters = FMath::Max(LargestSquareMeters, static_cast<double>(CellAccumulation));
		CellsHoldingLessThanTheirOwnArea +=
			static_cast<int32>(CellAccumulation < CellAreaSquareMeters * 0.999);
	}

	TestEqual(TEXT("Every cell drains at least its own area"), CellsHoldingLessThanTheirOwnArea, 0);
	TestTrue(FString::Printf(TEXT("No cell drains more than the world (%.0f of %.0f m2)"),
		LargestSquareMeters, WorldAreaSquareMeters),
		LargestSquareMeters <= WorldAreaSquareMeters * 1.001);

	// The valley axis gathers both flanks; the ridge at the world edge gathers a
	// strip. If these came out the same, the routing would not be routing.
	const int32 MiddleRow = Grid.Height - 8;
	const double AxisSquareMeters = Accumulation[Grid.Index(Grid.Width / 2, MiddleRow)];
	const double FlankSquareMeters = Accumulation[Grid.Index(4, MiddleRow)];
	TestTrue(FString::Printf(TEXT("The valley axis carries far more than its flank (%.0f against %.0f m2)"),
		AxisSquareMeters, FlankSquareMeters),
		AxisSquareMeters > FlankSquareMeters * 20.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentBakeWetnessTest,
	"Massif.WorldGen.EnvironmentBake.Wetness",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentBakeWetnessTest::RunTest(const FString& Parameters)
{
	const TUniquePtr<FMassifEnvironmentScheduler> Environment =
		MassifSolveTestEnvironment(MakeValley(), BakeTestSeed, MakeTestParams());
	const FMassifEnvironmentState& State = Environment->GetState();
	const FMassifChannelHandle Wetness = State.FindChannel(MassifEnvironmentChannels::Wetness());

	TestTrue(FString::Printf(TEXT("Wetness stays inside 0-1 (%.3f to %.3f)"),
		State.GetChannelMinimum(Wetness), State.GetChannelMaximum(Wetness)),
		State.GetChannelMinimum(Wetness) >= 0.0 && State.GetChannelMaximum(Wetness) <= 1.0);

	// Valley floor against the flank two hundred metres up the same cross-section.
	const double FloorWetness = State.SampleScalar(Wetness, FVector2d(0.0, -200.0));
	const double FlankWetness = State.SampleScalar(Wetness, FVector2d(300.0, -200.0));
	TestTrue(FString::Printf(TEXT("The valley floor is wetter than its flank (%.3f against %.3f)"),
		FloorWetness, FlankWetness),
		FloorWetness > FlankWetness + 0.1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentBakeSoilTest,
	"Massif.WorldGen.EnvironmentBake.Soil",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentBakeSoilTest::RunTest(const FString& Parameters)
{
	const FMassifEnvironmentBakeParams Params = MakeTestParams();
	const TUniquePtr<FMassifEnvironmentScheduler> Environment =
		MassifSolveTestEnvironment(MakeValley(), BakeTestSeed, Params);
	const FMassifEnvironmentState& State = Environment->GetState();
	const FMassifChannelHandle SoilDepth = State.FindChannel(MassifEnvironmentChannels::SoilDepth());

	TestTrue(FString::Printf(TEXT("Soil depth stays inside its band (%.2f to %.2f m)"),
		State.GetChannelMinimum(SoilDepth), State.GetChannelMaximum(SoilDepth)),
		State.GetChannelMinimum(SoilDepth) >= 0.0
			&& State.GetChannelMaximum(SoilDepth) <= Params.MaxSoilDepthMeters);

	// Droplets pick material up where they run fastest and drop it where the
	// ground flattens out, so the valley floor must end up better covered than
	// the flank that feeds it.
	const double FloorSoilMeters = State.SampleScalar(SoilDepth, FVector2d(0.0, -200.0));
	const double FlankSoilMeters = State.SampleScalar(SoilDepth, FVector2d(300.0, -200.0));
	TestTrue(FString::Printf(TEXT("Sediment settles on the valley floor (%.3f m against %.3f m on the flank)"),
		FloorSoilMeters, FlankSoilMeters),
		FloorSoilMeters > FlankSoilMeters);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentBakeDeterminismTest,
	"Massif.WorldGen.EnvironmentBake.Determinism",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentBakeDeterminismTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifHeightmap> Heightmap = MakeValley();
	const FMassifEnvironmentBakeParams Params = MakeTestParams();

	const uint64 FirstHash =
		MassifSolveTestEnvironment(Heightmap, BakeTestSeed, Params)->GetState().ComputeContentHash();
	const uint64 SecondHash =
		MassifSolveTestEnvironment(Heightmap, BakeTestSeed, Params)->GetState().ComputeContentHash();
	const uint64 OtherSeedHash =
		MassifSolveTestEnvironment(Heightmap, BakeTestSeed + 1, Params)->GetState().ComputeContentHash();

	// The droplet pass runs across the thread pool and scatters into shared
	// cells. Equal hashes are what says that scatter is order-independent.
	TestEqual(TEXT("The same seed bakes the same world twice"), FirstHash, SecondHash);
	TestNotEqual(TEXT("A different seed moves the droplets"), FirstHash, OtherSeedHash);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentBakeSamplingTest,
	"Massif.WorldGen.EnvironmentBake.Sampling",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentBakeSamplingTest::RunTest(const FString& Parameters)
{
	const TUniquePtr<FMassifEnvironmentScheduler> Environment =
		MassifSolveTestEnvironment(MakeValley(), BakeTestSeed, MakeTestParams());
	const FMassifEnvironmentState& State = Environment->GetState();
	const FMassifChannelHandle Wetness = State.FindChannel(MassifEnvironmentChannels::Wetness());

	TestEqual(TEXT("The bake grid covers the world at the requested cell size"),
		State.GetGrid(Wetness).Width, BakeTestSide);

	// A position query at a cell centre has to answer that cell, or every
	// consumer of the bake is reading it half a cell off the ground it describes.
	double WorstDifference = 0.0;
	for (int32 Step = 0; Step < 32; ++Step)
	{
		const int32 Column = 3 + Step * 2;
		const int32 Row = 5 + Step;
		const FVector2d Position = TestSamplePosition(Column, Row);
		const double Sampled = State.SampleScalar(Wetness, Position);
		const double Cell = State.ReadCell(Wetness, Column, Row);
		WorstDifference = FMath::Max(WorstDifference, FMath::Abs(Sampled - Cell));
	}
	TestTrue(FString::Printf(TEXT("Sampling a cell centre reads that cell (worst %.6f)"), WorstDifference),
		WorstDifference < 1.0e-6);

	// Outside the world the bake clamps rather than reading off the end of its grid.
	const double FarOutside = State.SampleScalar(Wetness, FVector2d(50000.0, -50000.0));
	TestTrue(TEXT("Positions outside the world clamp to its border"), FarOutside >= 0.0 && FarOutside <= 1.0);
	return true;
}
