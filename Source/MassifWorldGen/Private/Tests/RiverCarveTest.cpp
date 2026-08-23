#include "MassifEnvironmentBakeParams.h"
#include "MassifTestEnvironment.h"
#include "MassifHeightmap.h"
#include "MassifRiverField.h"
#include "MassifRiverNetwork.h"
#include "MassifTerrainField.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr int32 CarveTestSide = 64;
	constexpr double CarveTestSpacingMeters = 4.0;
	constexpr double CarveTestHalfWorldMeters = 0.5 * CarveTestSide * CarveTestSpacingMeters;

	/** A plane falling east, so the channel cut into it runs downhill like a real one. */
	double CarveTestAltitudeMeters(FVector2d PositionMeters)
	{
		return 1500.0 - 0.04 * PositionMeters.X;
	}

	FVector2d CarveTestSamplePosition(int32 Column, int32 Row)
	{
		return FVector2d(
			-CarveTestHalfWorldMeters + (Column + 0.5) * CarveTestSpacingMeters,
			CarveTestHalfWorldMeters - (Row + 0.5) * CarveTestSpacingMeters);
	}

	void AppendSlopeRow(int32 Row, TArray<double>& OutAltitudes)
	{
		for (int32 Column = 0; Column < CarveTestSide; ++Column)
		{
			OutAltitudes.Add(CarveTestAltitudeMeters(CarveTestSamplePosition(Column, Row)));
		}
	}

	TSharedRef<const FMassifHeightmap> MakeSlope()
	{
		TArray<double> Altitudes;
		Altitudes.Reserve(CarveTestSide * CarveTestSide);
		for (int32 Row = 0; Row < CarveTestSide; ++Row)
		{
			AppendSlopeRow(Row, Altitudes);
		}
		return FMassifHeightmap::FromAltitudes(
			Altitudes, CarveTestSide, CarveTestSide, CarveTestSpacingMeters);
	}

	/** A channel running east down the middle of the slope, at the altitude the ground has there. */
	FMassifRiverReach MakeReachOnSlope(double WidthMeters, double DepthMeters)
	{
		FMassifRiverReach Reach;
		for (int32 NodeIndex = 0; NodeIndex < 24; ++NodeIndex)
		{
			const double EastingMeters = -100.0 + NodeIndex * 8.0;

			FMassifRiverNode Node;
			Node.PositionMeters = FVector2d(EastingMeters, 0.0);
			Node.GroundAltitudeMeters = CarveTestAltitudeMeters(Node.PositionMeters);
			Node.WaterSurfaceMeters = Node.GroundAltitudeMeters;
			Node.BedAltitudeMeters = Node.WaterSurfaceMeters - DepthMeters;
			Node.DischargeCubicMetersPerSecond = 12.0;
			Node.WidthMeters = WidthMeters;
			Node.DepthMeters = DepthMeters;
			Node.SpeedMetersPerSecond = 1.5;
			Reach.Nodes.Add(Node);
		}
		return Reach;
	}

	struct FCarveTestWorld
	{
		TSharedPtr<const FMassifHeightmap> Heightmap;
		TSharedPtr<const FMassifRiverField> RiverField;
		TSharedPtr<const FMassifTerrainField> Measured;
		TSharedPtr<const FMassifTerrainField> Carved;
	};

	FCarveTestWorld MakeCarveTestWorld(double WidthMeters, double DepthMeters)
	{
		TArray<FMassifRiverReach> Reaches;
		Reaches.Add(MakeReachOnSlope(WidthMeters, DepthMeters));

		FCarveTestWorld World;
		World.Heightmap = MakeSlope();
		const TSharedRef<const FMassifRiverNetwork> Network = FMassifRiverNetwork::Create(MoveTemp(Reaches));
		World.RiverField = FMassifRiverField::Build(Network.Get(), MassifDefaultBankReachMeters);
		World.Measured = MakeShared<const FMassifTerrainField>(World.Heightmap.ToSharedRef());
		World.Carved = MakeShared<const FMassifTerrainField>(
			World.Heightmap.ToSharedRef(), World.RiverField.ToSharedRef());
		return World;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverCarveTrenchTest,
	"Massif.WorldGen.RiverCarve.Trench",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverCarveTrenchTest::RunTest(const FString& Parameters)
{
	constexpr double DepthMeters = 1.2;
	const FCarveTestWorld World = MakeCarveTestWorld(10.0, DepthMeters);

	// The one invariant everything downstream of this stands on: inside the
	// channel the ground is at or below the invert, so the water surface always
	// has the column the network solved under it and can never z-fight the bank.
	//
	// To the millimetre, because the trench is a depth taken off the measured
	// surface rather than a level the ground is clamped to, and the elevation
	// model is quantised to uint16 over its own altitude band. That quantisation
	// is the only difference, and it is four orders of magnitude under the
	// shallowest water the network will draw.
	//
	// Between the reach's own end stations. Past the last one the channel is
	// closing, and the trench rounds off there the same way it does at either
	// bank rather than running on at full depth into ground no station stands in.
	int32 PointsWithoutClearance = 0;
	for (int32 Step = 0; Step < 60; ++Step)
	{
		const FVector2d PositionMeters(-100.0 + Step * 3.0, 0.0);
		const double GroundMeters = World.Carved->SampleHeightMeters(PositionMeters, 0.25);
		const FMassifRiverSample River = World.RiverField->Sample(PositionMeters);
		PointsWithoutClearance += GroundMeters <= River.BedAltitudeMeters + 1.0e-3 ? 0 : 1;
	}

	TestEqual(TEXT("Midstream ground is cut to the invert"), PointsWithoutClearance, 0);

	// The nose still has water in it, because the section reaches further out than
	// the water does. A channel that ended on dry ground would be drawn with its
	// last few metres of surface lying on the bank.
	const FVector2d NoseMeters(87.0, 0.0);
	const FMassifRiverSample Nose = World.RiverField->Sample(NoseMeters);
	TestTrue(TEXT("The reach closes into water rather than onto the bank"),
		Nose.WaterSurfaceMeters - World.Carved->SampleHeightMeters(NoseMeters, 0.25) > 0.01);

	const FVector2d MidstreamMeters(0.0, 0.0);
	const double CarvedMeters = World.Carved->SampleHeightMeters(MidstreamMeters, 0.25);
	const double MeasuredMeters = World.Measured->SampleHeightMeters(MidstreamMeters, 0.25);
	TestTrue(TEXT("The trench is at least as deep as the channel"),
		MeasuredMeters - CarvedMeters >= DepthMeters - 1.0e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverCarveBankTest,
	"Massif.WorldGen.RiverCarve.Bank",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverCarveBankTest::RunTest(const FString& Parameters)
{
	const FCarveTestWorld World = MakeCarveTestWorld(10.0, 1.2);

	// Cutting a bed is a subtraction and never an addition: a channel that has
	// silted up above its banks must not be drawn as a dyke across the valley.
	// The bank has to close too, or every river in the world is a canyon.
	int32 PointsRaised = 0;
	int32 PointsDisturbedBeyondTheBank = 0;
	for (int32 Step = 0; Step <= 80; ++Step)
	{
		const FVector2d PositionMeters(0.0, -80.0 + Step * 2.0);
		const double CarvedMeters = World.Carved->SampleHeightMeters(PositionMeters, 0.25);
		const double MeasuredMeters = World.Measured->SampleHeightMeters(PositionMeters, 0.25);
		PointsRaised += CarvedMeters > MeasuredMeters + 1.0e-9 ? 1 : 0;

		const bool bBeyondBank =
			FMath::Abs(PositionMeters.Y) > 0.5 * 10.0 + MassifDefaultBankReachMeters;
		PointsDisturbedBeyondTheBank +=
			bBeyondBank && !FMath::IsNearlyEqual(CarvedMeters, MeasuredMeters, 1.0e-9) ? 1 : 0;
	}

	TestEqual(TEXT("The carve never raises the ground"), PointsRaised, 0);
	TestEqual(TEXT("Ground beyond the bank is the measured ground"), PointsDisturbedBeyondTheBank, 0);

	// The ground climbs out of the water without a step in it. It is no longer
	// pulled down over a band outside the channel to get there: the trench is a
	// depth taken off the measured surface, so outside the station's own section
	// there is nothing to take off and the bank is the hillside the elevation
	// model already has. What has to hold is that the two meet continuously.
	double PreviousMeters = World.Carved->SampleHeightMeters(FVector2d(0.0, 0.0), 0.25);
	double LargestStepMeters = 0.0;
	for (int32 Step = 1; Step <= 400; ++Step)
	{
		const FVector2d PositionMeters(0.0, Step * 0.1);
		const double CarvedMeters = World.Carved->SampleHeightMeters(PositionMeters, 0.25);
		LargestStepMeters = FMath::Max(LargestStepMeters, FMath::Abs(CarvedMeters - PreviousMeters));
		PreviousMeters = CarvedMeters;
	}

	TestTrue(TEXT("The ground climbs out of the channel without a step"), LargestStepMeters < 0.1);

	// Partway out the cut is partial, which is what makes the side of a channel a
	// bank rather than a wall.
	const FVector2d BankMeters(0.0, 4.5);
	const double CarvedMeters = World.Carved->SampleHeightMeters(BankMeters, 0.25);
	const double MeasuredMeters = World.Measured->SampleHeightMeters(BankMeters, 0.25);
	const FMassifRiverSample River = World.RiverField->Sample(BankMeters);
	TestTrue(TEXT("The bank is cut"), CarvedMeters < MeasuredMeters - 1.0e-6);
	TestTrue(TEXT("The bank is not cut all the way to the invert"),
		CarvedMeters > River.BedAltitudeMeters + 1.0e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverCarveCrossSectionTest,
	"Massif.WorldGen.RiverCarve.CrossSection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverCarveCrossSectionTest::RunTest(const FString& Parameters)
{
	constexpr double WidthMeters = 10.0;
	constexpr double DepthMeters = 1.2;
	const FCarveTestWorld World = MakeCarveTestWorld(WidthMeters, DepthMeters);

	// Water has to shallow towards its own edge. Cut as a flat-bottomed trench it
	// would stand the whole column against a wall at the waterline, and every
	// river in the world would be drawn with a hard line down each side.
	int32 SamplesDeeperThanTheOneInside = 0;
	double PreviousColumnMeters = TNumericLimits<double>::Max();
	for (int32 Step = 0; Step <= 10; ++Step)
	{
		const FVector2d PositionMeters(0.0, Step * 0.5 * WidthMeters / 10.0);
		const FMassifRiverSample River = World.RiverField->Sample(PositionMeters);
		const double ColumnMeters =
			River.WaterSurfaceMeters - World.Carved->SampleHeightMeters(PositionMeters, 0.25);
		SamplesDeeperThanTheOneInside += ColumnMeters > PreviousColumnMeters + 1.0e-9 ? 1 : 0;
		PreviousColumnMeters = ColumnMeters;
	}

	TestEqual(TEXT("The water only gets shallower towards the bank"), SamplesDeeperThanTheOneInside, 0);

	const FVector2d MidstreamMeters(0.0, 0.0);
	const FVector2d WaterlineMeters(0.0, 0.5 * WidthMeters);
	const FMassifRiverSample Midstream = World.RiverField->Sample(MidstreamMeters);
	const double MidstreamColumnMeters =
		Midstream.WaterSurfaceMeters - World.Carved->SampleHeightMeters(MidstreamMeters, 0.25);
	const double WaterlineColumnMeters =
		World.RiverField->Sample(WaterlineMeters).WaterSurfaceMeters
		- World.Carved->SampleHeightMeters(WaterlineMeters, 0.25);

	TestTrue(TEXT("Midstream carries the whole column"), MidstreamColumnMeters >= DepthMeters - 1.0e-6);
	// Not zero at the edge: the water mesh ends there, and a surface drawn exactly
	// on the ground it meets would fight it along that whole line.
	TestTrue(TEXT("The waterline still has water in it"), WaterlineColumnMeters > 0.01);
	TestTrue(TEXT("The waterline is a shore rather than a wall"),
		WaterlineColumnMeters < 0.25 * MidstreamColumnMeters);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverCarveLodTest,
	"Massif.WorldGen.RiverCarve.Lod",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverCarveLodTest::RunTest(const FString& Parameters)
{
	const FCarveTestWorld World = MakeCarveTestWorld(10.0, 1.2);

	// A tile sampled coarsely gets a filtered version of the same cut, not a
	// different surface: midstream stays on the invert whatever spacing asks, so
	// the water drawn over a coarse tile is not left standing on the ground.
	//
	// The trench itself no longer reads the spacing at all, so what this now
	// measures is that the filtered ground under it still clears the invert.
	const FVector2d MidstreamMeters(0.0, 0.0);
	const FMassifRiverSample River = World.RiverField->Sample(MidstreamMeters);
	int32 SpacingsLeavingTheWaterDry = 0;
	for (const double SpacingMeters : { 0.25, 1.0, 4.0, 16.0, 64.0 })
	{
		const double GroundMeters = World.Carved->SampleHeightMeters(MidstreamMeters, SpacingMeters);
		SpacingsLeavingTheWaterDry += GroundMeters <= River.BedAltitudeMeters + 1.0e-3 ? 0 : 1;
	}

	TestEqual(TEXT("Midstream is cut to the invert at every sample spacing"), SpacingsLeavingTheWaterDry, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverCarveBakeIndependenceTest,
	"Massif.WorldGen.RiverCarve.BakeIndependence",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverCarveBakeIndependenceTest::RunTest(const FString& Parameters)
{
	const FCarveTestWorld World = MakeCarveTestWorld(10.0, 1.2);

	// The bake traces the rivers the carve cuts, so if the carve ever reached the
	// bake the two would chase each other: a deeper channel routing more water
	// routing a deeper channel. The bake reads the elevation model directly, and
	// this is what says so out loud.
	FMassifEnvironmentBakeParams BakeParams;
	BakeParams.CellSizeMeters = 8.0;
	const uint64 FirstHash =
		MassifSolveTestEnvironment(World.Heightmap.ToSharedRef(), 4711, BakeParams)
			->GetState().ComputeContentHash();
	const uint64 SecondHash =
		MassifSolveTestEnvironment(World.Carved->GetHeightmapShared(), 4711, BakeParams)
			->GetState().ComputeContentHash();

	TestEqual(TEXT("A carved field hands the bake the same elevation model it started from"),
		SecondHash, FirstHash);
	TestNotNull(TEXT("The carved field knows about its rivers"), World.Carved->GetRiverField());
	TestNull(TEXT("A field built without rivers has none"), World.Measured->GetRiverField());
	return true;
}
