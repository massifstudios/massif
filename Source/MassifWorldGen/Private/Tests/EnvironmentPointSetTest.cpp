#include "MassifEnvironmentState.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr double PointSetTestWorldSideMeters = 2048.0;
	constexpr double PointSetTestTileSizeMeters = 512.0;

	FVector2d PointSetTestWorldSize()
	{
		return FVector2d(PointSetTestWorldSideMeters, PointSetTestWorldSideMeters);
	}

	FMassifPointSetDescriptor MakePointSetDescriptor(const TCHAR* Name)
	{
		FMassifPointSetDescriptor Descriptor;
		Descriptor.Name = FName(Name);
		Descriptor.TileSizeMeters = PointSetTestTileSizeMeters;
		Descriptor.RadiusRangeMeters = FVector2d(0.25, 8.0);
		Descriptor.ClassNames = { TEXT("Small"), TEXT("Large") };
		return Descriptor;
	}

	FMassifEnvironmentPoint MakePoint(FVector3d PositionMeters, double RadiusMeters)
	{
		FMassifEnvironmentPoint Point;
		Point.PositionMeters = PositionMeters;
		Point.RadiusMeters = RadiusMeters;
		return Point;
	}

	void CommitPoints(
		FMassifEnvironmentState& State, FMassifPointSetHandle PointSet,
		const TArray<FMassifEnvironmentPoint>& Points)
	{
		FMassifPointSetWriteScope Scope(State, PointSet);
		TArrayView<FMassifEnvironmentPoint> Slots = Scope.Reserve(Points.Num());
		for (int32 SlotIndex = 0; SlotIndex < Points.Num(); ++SlotIndex)
		{
			Slots[SlotIndex] = Points[SlotIndex];
		}
		Scope.Commit();
	}

	/** Every point in the set, walked bucket by bucket. */
	TArray<FMassifEnvironmentPoint> ReadAllPoints(
		const FMassifEnvironmentState& State, FMassifPointSetHandle PointSet)
	{
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentPointSetRoundTripTest,
	"Massif.WorldGen.EnvironmentPointSet.RoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentPointSetRoundTripTest::RunTest(const FString& Parameters)
{
	FMassifEnvironmentState State(PointSetTestWorldSize());
	const FMassifPointSetHandle PointSet = State.RegisterPointSet(MakePointSetDescriptor(TEXT("Test.Blocks")));

	FMassifEnvironmentPoint Written = MakePoint(FVector3d(-731.25, 402.5, 1873.75), 2.5);
	Written.RotationDegrees = FVector3d(47.0, 191.0, 303.0);
	Written.BuriedFraction = 0.4;
	Written.ClassIndex = 1;
	CommitPoints(State, PointSet, { Written });

	const TArray<FMassifEnvironmentPoint> ReadBack = ReadAllPoints(State, PointSet);
	if (!TestEqual(TEXT("One point was stored"), ReadBack.Num(), 1))
	{
		return false;
	}

	const FMassifEnvironmentPoint& Read = ReadBack[0];

	// Position is stored as a float offset from the tile corner, so a tile resolves
	// to well under a millimetre; the rest carry their own quantisation step.
	TestTrue(TEXT("Position survives to under a millimetre"),
		(Read.PositionMeters - Written.PositionMeters).Length() < 0.001);
	TestTrue(TEXT("Radius survives to its quantisation step"),
		FMath::Abs(Read.RadiusMeters - Written.RadiusMeters) <= (8.0 - 0.25) / 65535.0);
	TestTrue(TEXT("Rotation survives to its quantisation step"),
		(Read.RotationDegrees - Written.RotationDegrees).GetAbsMax() <= 360.0 / 65535.0);
	TestTrue(TEXT("Buried fraction survives to its quantisation step"),
		FMath::Abs(Read.BuriedFraction - Written.BuriedFraction) <= 1.0 / 255.0);
	TestEqual(TEXT("Class index survives exactly"), Read.ClassIndex, Written.ClassIndex);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentPointSetBucketingTest,
	"Massif.WorldGen.EnvironmentPointSet.Bucketing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentPointSetBucketingTest::RunTest(const FString& Parameters)
{
	FMassifEnvironmentState State(PointSetTestWorldSize());
	const FMassifPointSetHandle PointSet = State.RegisterPointSet(MakePointSetDescriptor(TEXT("Test.Blocks")));

	// One point per tile, at each tile's own centre.
	const FMassifChannelGrid& TileGrid = State.GetPointSetTileGrid(PointSet);
	TArray<FMassifEnvironmentPoint> Points;
	for (int32 Row = 0; Row < TileGrid.Height; ++Row)
	{
		for (int32 Column = 0; Column < TileGrid.Width; ++Column)
		{
			const FVector2d Centre = TileGrid.CellCentreMeters(Column, Row);
			Points.Add(MakePoint(FVector3d(Centre.X, Centre.Y, 1000.0), 1.0));
		}
	}
	CommitPoints(State, PointSet, Points);

	TestEqual(TEXT("Every point was stored"), State.GetPointCount(PointSet), Points.Num());

	TArray<FMassifEnvironmentPoint> TilePoints;
	int32 TilesHoldingOne = 0;
	for (int32 Row = 0; Row < TileGrid.Height; ++Row)
	{
		for (int32 Column = 0; Column < TileGrid.Width; ++Column)
		{
			State.ReadTilePoints(PointSet, Column, Row, TilePoints);
			TilesHoldingOne += TilePoints.Num() == 1 ? 1 : 0;
		}
	}
	TestEqual(TEXT("Each point landed in its own tile"), TilesHoldingOne, TileGrid.CellCount());

	State.ReadTilePoints(PointSet, TileGrid.Width, 0, TilePoints);
	TestEqual(TEXT("A tile outside the grid reads empty"), TilePoints.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentPointSetEmptySlotTest,
	"Massif.WorldGen.EnvironmentPointSet.EmptySlots",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentPointSetEmptySlotTest::RunTest(const FString& Parameters)
{
	FMassifEnvironmentState State(PointSetTestWorldSize());
	const FMassifPointSetHandle PointSet = State.RegisterPointSet(MakePointSetDescriptor(TEXT("Test.Blocks")));

	// A candidate that placed nothing leaves its slot at zero radius and must not
	// reach storage, nor move the slots either side of it.
	TArray<FMassifEnvironmentPoint> Points;
	Points.Add(MakePoint(FVector3d(-700.0, 700.0, 1000.0), 1.0));
	Points.Add(MakePoint(FVector3d(0.0, 0.0, 1000.0), 0.0));
	Points.Add(MakePoint(FVector3d(700.0, -700.0, 1000.0), 3.0));
	CommitPoints(State, PointSet, Points);

	const TArray<FMassifEnvironmentPoint> ReadBack = ReadAllPoints(State, PointSet);
	TestEqual(TEXT("The empty slot was dropped"), State.GetPointCount(PointSet), 2);
	if (!TestEqual(TEXT("Two points read back"), ReadBack.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("The surviving points are the ones that placed something"),
		ReadBack[0].RadiusMeters > 0.0 && ReadBack[1].RadiusMeters > 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentPointSetHashTest,
	"Massif.WorldGen.EnvironmentPointSet.Hash",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentPointSetHashTest::RunTest(const FString& Parameters)
{
	const TArray<FMassifEnvironmentPoint> Points = {
		MakePoint(FVector3d(-700.0, 700.0, 1000.0), 1.0),
		MakePoint(FVector3d(120.0, -340.0, 1400.0), 2.0),
	};

	FMassifEnvironmentState First(PointSetTestWorldSize());
	const FMassifPointSetHandle FirstSet = First.RegisterPointSet(MakePointSetDescriptor(TEXT("Test.Blocks")));
	CommitPoints(First, FirstSet, Points);

	FMassifEnvironmentState Second(PointSetTestWorldSize());
	const FMassifPointSetHandle SecondSet = Second.RegisterPointSet(MakePointSetDescriptor(TEXT("Test.Blocks")));
	CommitPoints(Second, SecondSet, Points);

	TestEqual(TEXT("The same points hash the same"), First.ComputeContentHash(), Second.ComputeContentHash());
	TestTrue(TEXT("The point set is counted as resident"), First.GetResidentByteCount() > 0);

	// A point set carrying no objects must still be distinguishable from one carrying some.
	FMassifEnvironmentState Third(PointSetTestWorldSize());
	const FMassifPointSetHandle ThirdSet = Third.RegisterPointSet(MakePointSetDescriptor(TEXT("Test.Blocks")));
	CommitPoints(Third, ThirdSet, {});
	TestNotEqual(TEXT("An empty set hashes differently"), Third.ComputeContentHash(), First.ComputeContentHash());

	// Moving one object must reach the hash, or a solve that placed rocks elsewhere
	// would claim to have produced the same world.
	TArray<FMassifEnvironmentPoint> Moved = Points;
	Moved[1].PositionMeters.X += 1.0;
	FMassifEnvironmentState Fourth(PointSetTestWorldSize());
	const FMassifPointSetHandle FourthSet = Fourth.RegisterPointSet(MakePointSetDescriptor(TEXT("Test.Blocks")));
	CommitPoints(Fourth, FourthSet, Moved);
	TestNotEqual(TEXT("A moved object changes the hash"), Fourth.ComputeContentHash(), First.ComputeContentHash());
	return true;
}
