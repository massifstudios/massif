#include "MassifHeightmap.h"
#include "MassifRiverField.h"
#include "MassifRiverNetwork.h"
#include "MassifShallowWater.h"
#include "MassifTerrainField.h"
#include "Misc/AutomationTest.h"

/**
 * The shallow water solve over a patch of carved channel.
 *
 * What it has to produce is water that stays where the bed can hold it, runs
 * downhill, and neither drains away nor piles up over a patch the reach is
 * feeding at one end and draining at the other.
 */
namespace
{
	constexpr int32 PatchTestSide = 128;
	constexpr double PatchTestSpacingMeters = 2.0;
	constexpr double PatchTestRidgeMeters = 1400.0;
	constexpr double PatchTestSlope = 0.02;

	/** A plane falling east, so a channel cut into it runs somewhere. */
	double PatchTestAltitudeMeters(FVector2d PositionMeters)
	{
		return PatchTestRidgeMeters - PatchTestSlope * PositionMeters.X;
	}

	TSharedRef<const FMassifHeightmap> MakePatchTestSlope()
	{
		const double HalfWorldMeters = 0.5 * PatchTestSide * PatchTestSpacingMeters;
		TArray<double> Altitudes;
		Altitudes.Reserve(PatchTestSide * PatchTestSide);
		for (int32 Row = 0; Row < PatchTestSide; ++Row)
		{
			for (int32 Column = 0; Column < PatchTestSide; ++Column)
			{
				const double EastingMeters =
					-HalfWorldMeters + (Column + 0.5) * PatchTestSpacingMeters;
				Altitudes.Add(PatchTestAltitudeMeters(FVector2d(EastingMeters, 0.0)));
			}
		}
		return FMassifHeightmap::FromAltitudes(
			Altitudes, PatchTestSide, PatchTestSide, PatchTestSpacingMeters);
	}

	/** A channel running east down the middle of the slope, a metre under the ground. */
	FMassifRiverReach MakePatchTestReach()
	{
		constexpr double DepthMeters = 1.0;
		FMassifRiverReach Reach;
		for (int32 NodeIndex = 0; NodeIndex < 60; ++NodeIndex)
		{
			const double EastingMeters = -120.0 + NodeIndex * 6.0;

			FMassifRiverNode Node;
			Node.PositionMeters = FVector2d(EastingMeters, 0.0);
			Node.GroundAltitudeMeters = PatchTestAltitudeMeters(Node.PositionMeters);
			Node.WaterSurfaceMeters = Node.GroundAltitudeMeters;
			Node.BedAltitudeMeters = Node.WaterSurfaceMeters - DepthMeters;
			Node.DischargeCubicMetersPerSecond = 20.0;
			Node.WidthMeters = 14.0;
			Node.DepthMeters = DepthMeters;
			Node.SpeedMetersPerSecond = 1.6;
			Reach.Nodes.Add(Node);
		}
		return Reach;
	}

	struct FPatchTestWorld
	{
		TSharedPtr<const FMassifRiverField> RiverField;
		TSharedPtr<const FMassifTerrainField> TerrainField;
		FMassifShallowWaterField Field;
		FMassifShallowWaterSummary Summary;
		FMassifShallowWaterParams Params;
	};

	FMassifShallowWaterRegion MakeTestRegion()
	{
		FMassifShallowWaterRegion Region;
		Region.CellSizeMeters = 0.5;
		Region.ExtentMeters = FVector2d(80.0, 40.0);
		Region.OriginMeters = FVector2d(-40.0, -20.0);
		return Region;
	}

	FPatchTestWorld SolvePatch(int32 StepCount)
	{
		TArray<FMassifRiverReach> Reaches;
		Reaches.Add(MakePatchTestReach());
		const TSharedRef<const FMassifRiverNetwork> Network =
			FMassifRiverNetwork::Create(MoveTemp(Reaches));

		FPatchTestWorld World;
		World.RiverField = FMassifRiverField::Build(Network.Get(), MassifDefaultBankReachMeters);
		World.TerrainField =
			MakeShared<const FMassifTerrainField>(MakePatchTestSlope(), World.RiverField.ToSharedRef());
		World.Params.StepCount = StepCount;
		World.Summary = MassifBakeShallowWater(
			*World.TerrainField, *World.RiverField, MassifAnnualTimeSlice, MakeTestRegion(),
			World.Params, World.Field);
		return World;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShallowWaterSettlesTest,
	"Massif.WorldGen.ShallowWater.Settles",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FShallowWaterSettlesTest::RunTest(const FString& Parameters)
{
	const FPatchTestWorld World = SolvePatch(600);

	TestTrue(TEXT("The patch has water in it"), World.Summary.WetCellCount > 0);
	TestTrue(TEXT("The water is about as deep as the channel is"),
		World.Summary.MaxDepthMeters > 0.5 && World.Summary.MaxDepthMeters < 2.0);

	// The one thing an explicit scheme is capable of getting catastrophically
	// wrong: a cell asked for more water than it holds goes negative, and its
	// neighbours follow on the step after until the patch is full of infinities.
	int32 CellsWithImpossibleWater = 0;
	for (int32 Cell = 0; Cell < World.Field.DepthMeters.Num(); ++Cell)
	{
		const bool bFinite = FMath::IsFinite(World.Field.DepthMeters[Cell]);
		CellsWithImpossibleWater += bFinite && World.Field.DepthMeters[Cell] >= 0.0f ? 0 : 1;
	}

	TestEqual(TEXT("No cell ends with negative or infinite water"), CellsWithImpossibleWater, 0);
	TestTrue(TEXT("Nothing runs faster than the guard allows"),
		World.Summary.MaxSpeedMetersPerSecond <= World.Params.MaxSpeedMetersPerSecond + 1.0e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShallowWaterStaysInTheChannelTest,
	"Massif.WorldGen.ShallowWater.StaysInTheChannel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FShallowWaterStaysInTheChannelTest::RunTest(const FString& Parameters)
{
	const FPatchTestWorld World = SolvePatch(600);

	// Water stands in the trench and not on the hillside beside it. A solve that
	// drove flow on the head alone rather than on the depth in the gap between two
	// cells spreads a film over the whole patch instead.
	int32 WetCellsOutsideTheChannel = 0;
	for (int32 Row = 0; Row < World.Field.CellCounts.Y; ++Row)
	{
		for (int32 Column = 0; Column < World.Field.CellCounts.X; ++Column)
		{
			const int32 Cell = World.Field.CellIndex(Column, Row);
			if (World.Field.DepthMeters[Cell] < World.Params.MinWetDepthMeters)
			{
				continue;
			}

			const FVector2d PositionMeters =
				World.Field.NodePositionMeters(Column, Row) + FVector2d(0.25, 0.25);
			WetCellsOutsideTheChannel += FMath::Abs(PositionMeters.Y) > 14.0 ? 1 : 0;
		}
	}

	TestEqual(TEXT("The water stays in the trench"), WetCellsOutsideTheChannel, 0);

	// And it runs downhill. The channel falls east, so the flow has to as well.
	const int32 MidRow = World.Field.CellCounts.Y / 2;
	double AlongChannelMetersPerSecond = 0.0;
	int32 CellsCounted = 0;
	for (int32 Column = 0; Column < World.Field.CellCounts.X; ++Column)
	{
		const int32 Cell = World.Field.CellIndex(Column, MidRow);
		if (World.Field.DepthMeters[Cell] < World.Params.MinWetDepthMeters)
		{
			continue;
		}
		AlongChannelMetersPerSecond += World.Field.VelocityMetersPerSecond[Cell].X;
		++CellsCounted;
	}

	TestTrue(TEXT("Midstream cells were found"), CellsCounted > 0);
	TestTrue(TEXT("The water runs the way the bed falls"),
		AlongChannelMetersPerSecond / FMath::Max(CellsCounted, 1) > 0.05);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShallowWaterDeterminismTest,
	"Massif.WorldGen.ShallowWater.Determinism",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FShallowWaterDeterminismTest::RunTest(const FString& Parameters)
{
	// Every pass writes only the cell it is solving for, so the grid may be split
	// across any number of threads and still land on the same answer to the bit.
	const FPatchTestWorld First = SolvePatch(200);
	const FPatchTestWorld Second = SolvePatch(200);

	int32 CellsDisagreeing = 0;
	for (int32 Cell = 0; Cell < First.Field.DepthMeters.Num(); ++Cell)
	{
		CellsDisagreeing += First.Field.DepthMeters[Cell] == Second.Field.DepthMeters[Cell] ? 0 : 1;
	}

	TestEqual(TEXT("Two runs settle to the same depth in every cell"), CellsDisagreeing, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShallowWaterMeshTest,
	"Massif.WorldGen.ShallowWater.Mesh",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FShallowWaterMeshTest::RunTest(const FString& Parameters)
{
	const FPatchTestWorld World = SolvePatch(600);
	const FMassifShallowWaterMesh Mesh = MassifBuildShallowWaterSurface(World.Field, World.Params);

	TestTrue(TEXT("The wet patch produces a surface"), Mesh.Triangles.Num() > 0);
	TestEqual(TEXT("Every vertex carries a normal"), Mesh.Normals.Num(), Mesh.PositionsMeters.Num());
	TestEqual(
		TEXT("Every vertex carries its depth and speed"), Mesh.DepthAndSpeed.Num(),
		Mesh.PositionsMeters.Num());
	TestEqual(
		TEXT("Every vertex carries the direction it is running"), Mesh.FlowDirection.Num(),
		Mesh.PositionsMeters.Num());

	// The surface is drawn over the bed, never under it: a sheet below the ground
	// it stands on is a sheet nobody can see.
	int32 VerticesUnderTheGround = 0;
	for (const FVector3f& PositionMeters : Mesh.PositionsMeters)
	{
		const FVector2d GroundAtMeters(PositionMeters.X, PositionMeters.Y);
		const double BedMeters = World.TerrainField->SampleHeightMeters(GroundAtMeters, 0.5);
		VerticesUnderTheGround += PositionMeters.Z >= BedMeters - 0.05 ? 0 : 1;
	}

	TestEqual(TEXT("No part of the surface is under the bed"), VerticesUnderTheGround, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShallowWaterFlowDirectionTest,
	"Massif.WorldGen.ShallowWater.FlowDirection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

/**
 * The direction the material lays its ripples along. Unit or nothing: anything
 * between is a length the shader would divide by, and the sheet is drawn with
 * whatever comes out of that.
 */
bool FShallowWaterFlowDirectionTest::RunTest(const FString& Parameters)
{
	const FPatchTestWorld World = SolvePatch(600);
	const FMassifShallowWaterMesh Mesh = MassifBuildShallowWaterSurface(World.Field, World.Params);

	int32 BadLengths = 0;
	int32 RunningEast = 0;
	int32 RunningWest = 0;
	for (const FVector2f& Direction : Mesh.FlowDirection)
	{
		const double Length = FVector2d(Direction).Length();
		if (Length >= 1.0e-4 && !FMath::IsNearlyEqual(Length, 1.0, 1.0e-3))
		{
			++BadLengths;
		}
		if (Direction.X > 0.5f)
		{
			++RunningEast;
		}
		if (Direction.X < -0.5f)
		{
			++RunningWest;
		}
	}

	TestEqual(TEXT("Every direction is a unit vector or no direction at all"), BadLengths, 0);
	// The channel falls east, so the water in it does.
	TestTrue(TEXT("The water runs down the channel"), RunningEast > RunningWest);
	return true;
}
