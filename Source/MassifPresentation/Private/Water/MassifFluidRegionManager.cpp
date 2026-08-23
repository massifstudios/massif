#include "MassifFluidRegionManager.h"

#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "MassifFluidBake.h"
#include "MassifFluidSurface.h"
#include "MassifTerrainField.h"

namespace
{
	using UE::Geometry::FDynamicMesh3;
	using UE::Geometry::FDynamicMeshNormalOverlay;

	constexpr double MetersToCentimeters = 100.0;

	/**
	 * How much world one region covers, and how finely.
	 *
	 * Small, because the cost is the volume over the cube of the cell: at five
	 * centimetres this box is already half a million cells and three quarters of a
	 * million particles. A river is covered by many of these rather than by one
	 * large one.
	 */
	constexpr double RegionAlongMeters = 8.0;
	constexpr double RegionAcrossMeters = 4.0;
	constexpr double RegionHeightMeters = 2.0;
	constexpr double RegionCellMeters = 0.05;

	/** How far under the water's own surface the region starts, so it holds the bed too. */
	constexpr double RegionFloorBelowSurfaceMeters = 1.2;

	FMassifFluidRegion MakeRegion(FVector2d CentreMeters, double WaterSurfaceAltitudeMeters)
	{
		FMassifFluidRegion Region;
		Region.CellSizeMeters = RegionCellMeters;
		Region.ExtentMeters = FVector(RegionAlongMeters, RegionAcrossMeters, RegionHeightMeters);
		Region.OriginMeters = FVector(
			CentreMeters.X - 0.5 * RegionAlongMeters,
			CentreMeters.Y - 0.5 * RegionAcrossMeters,
			WaterSurfaceAltitudeMeters - RegionFloorBelowSurfaceMeters);
		Region.InitialSurfaceAltitudeMeters = WaterSurfaceAltitudeMeters;
		return Region;
	}

	FMassifFluidBakeParams MakeBakeParams()
	{
		FMassifFluidBakeParams Params;
		Params.StepCount = 150;
		Params.PressureIterations = 25;
		return Params;
	}

	void AppendVertices(const FMassifFluidSurfaceMesh& Surface, FVector OriginMeters, FDynamicMesh3& OutMesh)
	{
		FDynamicMeshNormalOverlay* Normals = OutMesh.Attributes()->PrimaryNormals();
		for (int32 VertexIndex = 0; VertexIndex < Surface.PositionsMeters.Num(); ++VertexIndex)
		{
			const FVector RelativeMeters = FVector(Surface.PositionsMeters[VertexIndex]) - OriginMeters;
			OutMesh.AppendVertex(RelativeMeters * MetersToCentimeters);
			Normals->AppendElement(Surface.Normals[VertexIndex]);
		}
	}

	void AppendTriangles(const FMassifFluidSurfaceMesh& Surface, FDynamicMesh3& OutMesh)
	{
		FDynamicMeshNormalOverlay* Normals = OutMesh.Attributes()->PrimaryNormals();
		for (const FIntVector& Triangle : Surface.Triangles)
		{
			const int32 TriangleId = OutMesh.AppendTriangle(Triangle.X, Triangle.Y, Triangle.Z);
			if (TriangleId < 0)
			{
				continue;
			}
			Normals->SetTriangle(TriangleId, UE::Geometry::FIndex3i(Triangle.X, Triangle.Y, Triangle.Z));
		}
	}

	FDynamicMesh3 BuildDynamicMesh(const FMassifFluidSurfaceMesh& Surface, FVector OriginMeters)
	{
		FDynamicMesh3 Mesh;
		Mesh.EnableAttributes();
		Mesh.Attributes()->PrimaryNormals()->ClearElements();
		AppendVertices(Surface, OriginMeters, Mesh);
		AppendTriangles(Surface, Mesh);
		return Mesh;
	}
}

AMassifFluidRegionManager::AMassifFluidRegionManager()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AMassifFluidRegionManager::Initialize(
	TSharedPtr<const FMassifTerrainField> InTerrainField,
	FVector2d CentreMeters,
	double WaterSurfaceAltitudeMeters,
	UMaterialInterface* InWaterMaterial)
{
	if (!InTerrainField.IsValid())
	{
		return;
	}

	const FMassifFluidRegion Region = MakeRegion(CentreMeters, WaterSurfaceAltitudeMeters);
	const FMassifFluidBakeParams BakeParams = MakeBakeParams();

	TArray<FVector3f> PositionsMeters;
	TArray<FVector3f> VelocitiesMetersPerSecond;
	const FMassifFluidBakeSummary Summary = MassifBakeFluidRegion(
		*InTerrainField, Region, BakeParams, PositionsMeters, VelocitiesMetersPerSecond);

	FMassifFluidSurfaceParams SurfaceParams;
	SurfaceParams.ParticleSpacingMeters = Region.CellSizeMeters / BakeParams.ParticlesPerCellAxis;
	const FMassifFluidSurfaceMesh Surface =
		MassifBuildFluidSurface(Region, SurfaceParams, PositionsMeters);

	UE_LOG(LogTemp, Display,
		TEXT("Fluid region at %.0f, %.0f: %d cells, %d particles solved in %.1f s, "
			"%.2f m deep at most and %.1f m/s at fastest, meshed to %d triangles in %.2f s"),
		CentreMeters.X, CentreMeters.Y,
		Summary.CellCounts.X * Summary.CellCounts.Y * Summary.CellCounts.Z, Summary.ParticleCount,
		Summary.SolveSeconds, Summary.MaxDepthMeters, Summary.MaxSpeedMetersPerSecond,
		Surface.Triangles.Num(), Surface.BuildSeconds);

	if (Surface.Triangles.IsEmpty())
	{
		return;
	}

	SurfaceComponent = NewObject<UDynamicMeshComponent>(this);
	SurfaceComponent->SetupAttachment(RootComponent);
	SurfaceComponent->RegisterComponent();
	SurfaceComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SurfaceComponent->SetCastShadow(false);
	SurfaceComponent->SetMaterial(0, InWaterMaterial);
	SurfaceComponent->SetWorldLocation(Region.OriginMeters * MetersToCentimeters);
	SurfaceComponent->SetMesh(BuildDynamicMesh(Surface, Region.OriginMeters));
}
