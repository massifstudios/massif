#include "MassifEnvironmentPointSet.h"
#include "Misc/AutomationTest.h"
#include "Terrain/MassifRockManager.h"

namespace
{
	FMassifEnvironmentPoint MakeBlock(double RadiusMeters, double BuriedFraction)
	{
		FMassifEnvironmentPoint Block;
		Block.PositionMeters = FVector3d(-412.5, 907.25, 2143.0);
		Block.RadiusMeters = RadiusMeters;
		Block.BuriedFraction = BuriedFraction;
		return Block;
	}

	/** Where the drawn block's underside sits, in metres. */
	double UndersideAltitudeMeters(const FMassifEnvironmentPoint& Block)
	{
		const FTransform Transform = MassifRockPlacement::MakeBlockTransform(Block);
		const double HalfHeightCentimeters =
			Transform.GetScale3D().Z * MassifRockPlacement::PlaceholderRadiusCentimeters;
		return (Transform.GetLocation().Z - HalfHeightCentimeters) / 100.0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRockPlacementTest,
	"Massif.Presentation.RockPlacement.Transform",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRockPlacementTest::RunTest(const FString& Parameters)
{
	const FMassifEnvironmentPoint Block = MakeBlock(1.5, 0.0);
	const FTransform Transform = MassifRockPlacement::MakeBlockTransform(Block);

	// The solve works in metres and the engine in centimetres, so the horizontal
	// placement is the one conversion that would silently put every boulder a
	// hundred times too far out.
	TestTrue(TEXT("The block is drawn where the fall left it"),
		FMath::IsNearlyEqual(Transform.GetLocation().X, Block.PositionMeters.X * 100.0, 0.1)
			&& FMath::IsNearlyEqual(Transform.GetLocation().Y, Block.PositionMeters.Y * 100.0, 0.1));

	// A block that settled into nothing rests its underside on the ground, and one
	// buried to half its height has its middle there.
	TestTrue(TEXT("An unburied block rests on the surface"),
		FMath::IsNearlyEqual(UndersideAltitudeMeters(Block), Block.PositionMeters.Z, 0.001));
	TestTrue(TEXT("A half-buried block is centred on the surface"),
		FMath::IsNearlyEqual(
			MassifRockPlacement::MakeBlockTransform(MakeBlock(1.5, 0.5)).GetLocation().Z,
			Block.PositionMeters.Z * 100.0, 0.1));

	// Radius has to reach the drawn size, or every boulder is the same boulder.
	const double DrawnRadiusMeters =
		Transform.GetScale3D().X * MassifRockPlacement::PlaceholderRadiusCentimeters / 100.0;
	TestTrue(FString::Printf(TEXT("The block is drawn at its own radius (%.2f m of %.2f m)"),
		DrawnRadiusMeters, Block.RadiusMeters),
		FMath::IsNearlyEqual(DrawnRadiusMeters, Block.RadiusMeters, 0.001));

	const FTransform Larger = MassifRockPlacement::MakeBlockTransform(MakeBlock(3.0, 0.0));
	TestTrue(TEXT("A larger block is drawn larger"), Larger.GetScale3D().X > Transform.GetScale3D().X);
	return true;
}
