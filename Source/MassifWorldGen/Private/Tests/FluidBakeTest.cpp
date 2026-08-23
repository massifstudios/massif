#include "MassifFluidBake.h"
#include "MassifFluidSurface.h"

#include "MassifHeightmap.h"
#include "MassifTerrainField.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr int32 FluidTestSide = 64;
	constexpr double FluidTestSpacingMeters = 1.0;

	/**
	 * A level floor with no feature in it, so the region's ground is the same
	 * altitude whichever way the heightmap maps its axes and the test measures the
	 * solve rather than the fixture.
	 */
	TSharedRef<const FMassifTerrainField> MakeLevelField()
	{
		TArray<double> AltitudesMeters;
		AltitudesMeters.Init(100.0, FluidTestSide * FluidTestSide);
		return MakeShared<const FMassifTerrainField>(FMassifHeightmap::FromAltitudes(
			AltitudesMeters, FluidTestSide, FluidTestSide, FluidTestSpacingMeters));
	}

	FMassifFluidRegion MakeTroughRegion(const FMassifTerrainField& Terrain)
	{
		const double MiddleMeters = 0.5 * (FluidTestSide - 1) * FluidTestSpacingMeters;
		FMassifFluidRegion Region;
		Region.CellSizeMeters = 0.1;
		Region.ExtentMeters = FVector(4.0, 4.0, 2.0);
		Region.OriginMeters = FVector(MiddleMeters - 2.0, MiddleMeters - 2.0, 100.0);
		Region.InitialSurfaceAltitudeMeters = Region.OriginMeters.Z + 0.6;
		return Region;
	}

	/**
	 * A plane falling along both axes at once, so the water runs downhill whichever
	 * way the heightmap maps them and the test never depends on knowing.
	 */
	TSharedRef<const FMassifTerrainField> MakeSlopedField(double FallPerMeter)
	{
		TArray<double> AltitudesMeters;
		AltitudesMeters.SetNumZeroed(FluidTestSide * FluidTestSide);
		for (int32 Second = 0; Second < FluidTestSide; ++Second)
		{
			for (int32 First = 0; First < FluidTestSide; ++First)
			{
				AltitudesMeters[Second * FluidTestSide + First] =
					100.0 - FallPerMeter * (First + Second) * FluidTestSpacingMeters;
			}
		}
		return MakeShared<const FMassifTerrainField>(FMassifHeightmap::FromAltitudes(
			AltitudesMeters, FluidTestSide, FluidTestSide, FluidTestSpacingMeters));
	}

	FMassifFluidRegion MakeSlopedRegion(const FMassifTerrainField& Terrain)
	{
		const double MiddleMeters = 0.5 * (FluidTestSide - 1) * FluidTestSpacingMeters;
		const double LowCornerMeters = Terrain.SampleHeightMeters(
			FVector2d(MiddleMeters + 2.0, MiddleMeters + 2.0), 0.1);

		FMassifFluidRegion Region;
		Region.CellSizeMeters = 0.1;
		Region.ExtentMeters = FVector(4.0, 4.0, 2.0);
		Region.OriginMeters = FVector(MiddleMeters - 2.0, MiddleMeters - 2.0, LowCornerMeters - 0.4);
		Region.InitialSurfaceAltitudeMeters = Region.OriginMeters.Z + 0.9;
		return Region;
	}

	FMassifFluidBakeParams MakeTroughParams()
	{
		FMassifFluidBakeParams Params;
		Params.StepCount = 60;
		Params.PressureIterations = 20;
		return Params;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFluidBakeSettlesTest, "Massif.WorldGen.FluidBake.Settles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFluidBakeSettlesTest::RunTest(const FString&)
{
	const TSharedRef<const FMassifTerrainField> Terrain = MakeLevelField();
	const FMassifFluidRegion Region = MakeTroughRegion(Terrain.Get());

	TArray<FVector3f> PositionsMeters;
	TArray<FVector3f> VelocitiesMetersPerSecond;
	const FMassifFluidBakeSummary Summary = MassifBakeFluidRegion(
		Terrain.Get(), Region, MakeTroughParams(), PositionsMeters, VelocitiesMetersPerSecond);

	TestTrue(TEXT("The region is seeded with water"), Summary.ParticleCount > 1000);
	TestEqual(TEXT("Every particle keeps a velocity"), VelocitiesMetersPerSecond.Num(), PositionsMeters.Num());
	TestTrue(
		FString::Printf(TEXT("The water has depth, which is %.3f m at its deepest"), Summary.MaxDepthMeters),
		Summary.MaxDepthMeters > 0.05);

	double LowestMeters = TNumericLimits<double>::Max();
	double HighestMeters = -TNumericLimits<double>::Max();
	for (const FVector3f& PositionMeters : PositionsMeters)
	{
		LowestMeters = FMath::Min(LowestMeters, static_cast<double>(PositionMeters.Z));
		HighestMeters = FMath::Max(HighestMeters, static_cast<double>(PositionMeters.Z));
	}
	TestTrue(
		FString::Printf(TEXT("No particle is left under the ground, lowest at %.3f m"), LowestMeters),
		LowestMeters >= Region.OriginMeters.Z - 0.01);
	TestTrue(
		FString::Printf(TEXT("The water stays inside the region, highest at %.3f m"), HighestMeters),
		HighestMeters <= Region.OriginMeters.Z + Region.ExtentMeters.Z + 0.01);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFluidBakeRepeatsTest, "Massif.WorldGen.FluidBake.Repeats",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFluidBakeRepeatsTest::RunTest(const FString&)
{
	const TSharedRef<const FMassifTerrainField> Terrain = MakeLevelField();
	const FMassifFluidRegion Region = MakeTroughRegion(Terrain.Get());

	TArray<FVector3f> FirstPositionsMeters;
	TArray<FVector3f> FirstVelocities;
	MassifBakeFluidRegion(
		Terrain.Get(), Region, MakeTroughParams(), FirstPositionsMeters, FirstVelocities);

	TArray<FVector3f> SecondPositionsMeters;
	TArray<FVector3f> SecondVelocities;
	MassifBakeFluidRegion(
		Terrain.Get(), Region, MakeTroughParams(), SecondPositionsMeters, SecondVelocities);

	TestEqual(TEXT("The same region seeds the same water"),
		SecondPositionsMeters.Num(), FirstPositionsMeters.Num());
	if (SecondPositionsMeters.Num() != FirstPositionsMeters.Num())
	{
		return false;
	}

	double WorstMeters = 0.0;
	for (int32 ParticleIndex = 0; ParticleIndex < FirstPositionsMeters.Num(); ++ParticleIndex)
	{
		WorstMeters = FMath::Max(WorstMeters,
			static_cast<double>(
				(SecondPositionsMeters[ParticleIndex] - FirstPositionsMeters[ParticleIndex]).Length()));
	}
	TestTrue(
		FString::Printf(TEXT("Two runs put every particle in the same place, worst %.9f m"), WorstMeters),
		WorstMeters == 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFluidBakeFlowsTest, "Massif.WorldGen.FluidBake.Flows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFluidBakeFlowsTest::RunTest(const FString&)
{
	const TSharedRef<const FMassifTerrainField> Terrain = MakeSlopedField(0.05);
	const FMassifFluidRegion Region = MakeSlopedRegion(Terrain.Get());

	FMassifFluidBakeParams Params = MakeTroughParams();
	Params.StepCount = 120;
	Params.InflowSpeedMetersPerSecond = 0.0;

	TArray<FVector3f> PositionsMeters;
	TArray<FVector3f> VelocitiesMetersPerSecond;
	const FMassifFluidBakeSummary Summary = MassifBakeFluidRegion(
		Terrain.Get(), Region, Params, PositionsMeters, VelocitiesMetersPerSecond);

	double MeanHorizontalMetersPerSecond = 0.0;
	for (const FVector3f& VelocityMetersPerSecond : VelocitiesMetersPerSecond)
	{
		MeanHorizontalMetersPerSecond +=
			FVector2d(VelocityMetersPerSecond.X, VelocityMetersPerSecond.Y).Length();
	}
	MeanHorizontalMetersPerSecond /= FMath::Max(VelocitiesMetersPerSecond.Num(), 1);

	AddInfo(FString::Printf(
		TEXT("layers %d: across %.4f m/s, fastest %.3f m/s, deepest %.4f m, volume %.4f m3"),
		Params.ExtrapolationLayers, MeanHorizontalMetersPerSecond, Summary.MaxSpeedMetersPerSecond,
		Summary.MaxDepthMeters, Summary.WetVolumeCubicMeters));

	// Started at rest, so any horizontal motion is the slope driving it. Water held
	// in by an unextrapolated free surface never gets going at all.
	TestTrue(
		FString::Printf(TEXT("The water runs downhill, at %.3f m/s across"), MeanHorizontalMetersPerSecond),
		MeanHorizontalMetersPerSecond > 0.05);
	TestTrue(
		FString::Printf(TEXT("Nothing is flung out of the solve, fastest %.2f m/s"),
			Summary.MaxSpeedMetersPerSecond),
		Summary.MaxSpeedMetersPerSecond < 15.0);
	TestTrue(
		FString::Printf(TEXT("The water keeps its depth, %.3f m at its deepest"), Summary.MaxDepthMeters),
		Summary.MaxDepthMeters > 0.05);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFluidSurfaceMeshTest, "Massif.WorldGen.FluidBake.Surface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFluidSurfaceMeshTest::RunTest(const FString&)
{
	const TSharedRef<const FMassifTerrainField> Terrain = MakeLevelField();
	const FMassifFluidRegion Region = MakeTroughRegion(Terrain.Get());
	const FMassifFluidBakeParams BakeParams = MakeTroughParams();

	TArray<FVector3f> PositionsMeters;
	TArray<FVector3f> VelocitiesMetersPerSecond;
	MassifBakeFluidRegion(
		Terrain.Get(), Region, BakeParams, PositionsMeters, VelocitiesMetersPerSecond);

	FMassifFluidSurfaceParams SurfaceParams;
	SurfaceParams.ParticleSpacingMeters = Region.CellSizeMeters / BakeParams.ParticlesPerCellAxis;
	const FMassifFluidSurfaceMesh Mesh =
		MassifBuildFluidSurface(Region, SurfaceParams, PositionsMeters);

	AddInfo(FString::Printf(
		TEXT("%d particles to %d vertices and %d triangles in %.3f s"),
		PositionsMeters.Num(), Mesh.PositionsMeters.Num(), Mesh.Triangles.Num(), Mesh.BuildSeconds));

	TestTrue(TEXT("The water is meshed"), Mesh.Triangles.Num() > 100);
	TestEqual(TEXT("Every vertex carries a normal"), Mesh.Normals.Num(), Mesh.PositionsMeters.Num());

	for (const FIntVector& Triangle : Mesh.Triangles)
	{
		const bool bInRange = Triangle.GetMin() >= 0 && Triangle.GetMax() < Mesh.PositionsMeters.Num();
		if (!bInRange)
		{
			TestTrue(TEXT("Every triangle indexes vertices that exist"), false);
			return false;
		}
	}

	double WorstNormalLength = 0.0;
	for (const FVector3f& Normal : Mesh.Normals)
	{
		WorstNormalLength = FMath::Max(WorstNormalLength, FMath::Abs(1.0 - Normal.Length()));
	}
	TestTrue(
		FString::Printf(TEXT("Every normal is a unit, worst off by %.5f"), WorstNormalLength),
		WorstNormalLength < 1.0e-3);

	double HighestMeters = -TNumericLimits<double>::Max();
	for (const FVector3f& PositionMeters : Mesh.PositionsMeters)
	{
		HighestMeters = FMath::Max(HighestMeters, static_cast<double>(PositionMeters.Z));
	}
	AddInfo(FString::Printf(TEXT("surface tops out at %.3f m, ground at %.3f m"),
		HighestMeters, Region.OriginMeters.Z));
	TestTrue(
		FString::Printf(TEXT("The surface stands over the ground, topping out at %.3f m"), HighestMeters),
		HighestMeters > Region.OriginMeters.Z + 0.1);
	return true;
}

#endif
