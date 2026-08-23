#include "MassifShallowWaterRegion.h"

#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "HAL/IConsoleManager.h"
#include "MassifRiverField.h"
#include "MassifShallowWater.h"
#include "MassifTerrainField.h"
#include "MassifUnits.h"

namespace
{
	using MassifUnits::MetersToCentimeters;
	using UE::Geometry::FDynamicMesh3;
	using UE::Geometry::FDynamicMeshColorOverlay;
	using UE::Geometry::FDynamicMeshNormalOverlay;

	/**
	 * How much world one patch covers, and how finely.
	 *
	 * Two hundred metres of channel at a quarter metre, which is a third of a
	 * million columns and a fraction of a second. The cost is the area over the
	 * square of the cell rather than the volume over its cube, so the same budget
	 * that bought eight metres of a three dimensional solve buys a whole bend here.
	 */
	constexpr double PatchAlongMeters = 200.0;
	constexpr double PatchAcrossMeters = 100.0;
	constexpr double PatchCellMeters = 0.25;

	/** The half-month the patch is solved at, from the same day the rivers are drawn at. */
	int32 DrawnTimeSlice()
	{
		const IConsoleVariable* Variable =
			IConsoleManager::Get().FindConsoleVariable(TEXT("massif.Water.DayOfYear"));
		if (Variable == nullptr)
		{
			return MassifAnnualTimeSlice;
		}

		const int32 Day = FMath::Clamp(Variable->GetInt(), 1, 365);
		return FMath::Clamp((Day - 1) * MassifSeasonalSliceCount / 365, 0, MassifSeasonalSliceCount - 1);
	}

	FMassifShallowWaterRegion MakeRegion(FVector2d CentreMeters)
	{
		FMassifShallowWaterRegion Region;
		Region.CellSizeMeters = PatchCellMeters;
		Region.ExtentMeters = FVector2d(PatchAlongMeters, PatchAcrossMeters);
		Region.OriginMeters = CentreMeters - 0.5 * Region.ExtentMeters;
		return Region;
	}

	void AppendVertices(
		const FMassifShallowWaterMesh& Surface, FVector2d OriginMeters, FDynamicMesh3& OutMesh)
	{
		FDynamicMeshNormalOverlay* Normals = OutMesh.Attributes()->PrimaryNormals();
		FDynamicMeshColorOverlay* Colors = OutMesh.Attributes()->PrimaryColors();
		for (int32 VertexIndex = 0; VertexIndex < Surface.PositionsMeters.Num(); ++VertexIndex)
		{
			const FVector3f& PositionMeters = Surface.PositionsMeters[VertexIndex];
			OutMesh.AppendVertex(FVector3d(
				(PositionMeters.X - OriginMeters.X) * MetersToCentimeters,
				(PositionMeters.Y - OriginMeters.Y) * MetersToCentimeters,
				PositionMeters.Z * MetersToCentimeters));
			Normals->AppendElement(Surface.Normals[VertexIndex]);

			// Speed and depth where the tiled surface carries them, so one material
			// draws both and a patch does not read as a different river.
			const FVector2f& DepthAndSpeed = Surface.DepthAndSpeed[VertexIndex];
			Colors->AppendElement(FVector4f(DepthAndSpeed.Y, DepthAndSpeed.X, 0.0f, DepthAndSpeed.X));
		}
	}

	void AppendTriangles(const FMassifShallowWaterMesh& Surface, FDynamicMesh3& OutMesh)
	{
		FDynamicMeshNormalOverlay* Normals = OutMesh.Attributes()->PrimaryNormals();
		FDynamicMeshColorOverlay* Colors = OutMesh.Attributes()->PrimaryColors();
		for (const FIntVector& Triangle : Surface.Triangles)
		{
			const int32 TriangleId = OutMesh.AppendTriangle(Triangle.X, Triangle.Y, Triangle.Z);
			if (TriangleId < 0)
			{
				continue;
			}

			const UE::Geometry::FIndex3i Corners(Triangle.X, Triangle.Y, Triangle.Z);
			Normals->SetTriangle(TriangleId, Corners);
			Colors->SetTriangle(TriangleId, Corners);
		}
	}

	/**
	 * Compacted, because the lattice covers the whole patch and the water covers a
	 * channel through it: most of a patch's nodes belong to no triangle at all.
	 */
	FDynamicMesh3 BuildDynamicMesh(const FMassifShallowWaterMesh& Surface, FVector2d OriginMeters)
	{
		FDynamicMesh3 Mesh;
		Mesh.EnableAttributes();
		Mesh.Attributes()->EnablePrimaryColors();
		Mesh.Attributes()->PrimaryNormals()->ClearElements();
		Mesh.Attributes()->PrimaryColors()->ClearElements();
		AppendVertices(Surface, OriginMeters, Mesh);
		AppendTriangles(Surface, Mesh);
		Mesh.CompactInPlace();
		return Mesh;
	}
}

AMassifShallowWaterRegion::AMassifShallowWaterRegion()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AMassifShallowWaterRegion::Initialize(
	TSharedPtr<const FMassifTerrainField> InTerrainField,
	TSharedPtr<const FMassifRiverField> InRiverField,
	FVector2d CentreMeters,
	UMaterialInterface* InWaterMaterial)
{
	if (!InTerrainField.IsValid() || !InRiverField.IsValid())
	{
		return;
	}

	const FMassifShallowWaterRegion Region = MakeRegion(CentreMeters);
	const FMassifShallowWaterParams Params;

	FMassifShallowWaterField Field;
	const FMassifShallowWaterSummary Summary = MassifBakeShallowWater(
		*InTerrainField, *InRiverField, DrawnTimeSlice(), Region, Params, Field);
	const FMassifShallowWaterMesh Surface = MassifBuildShallowWaterSurface(Field, Params);

	UE_LOG(LogTemp, Display,
		TEXT("Shallow water at %.0f, %.0f: %d by %d cells solved in %.2f s, %d wet over %.0f m2, "
			"%.2f m deep on average and %.2f m at most, %.2f m/s on average and %.2f m/s at most, "
			"meshed to %d triangles in %.2f s"),
		CentreMeters.X, CentreMeters.Y, Summary.CellCounts.X, Summary.CellCounts.Y,
		Summary.SolveSeconds, Summary.WetCellCount, Summary.WetAreaSquareMeters,
		Summary.MeanDepthMeters, Summary.MaxDepthMeters, Summary.MeanSpeedMetersPerSecond,
		Summary.MaxSpeedMetersPerSecond, Surface.Triangles.Num(), Surface.BuildSeconds);

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
	SurfaceComponent->SetWorldLocation(
		FVector(Region.OriginMeters.X, Region.OriginMeters.Y, 0.0) * MetersToCentimeters);
	SurfaceComponent->SetMesh(BuildDynamicMesh(Surface, Region.OriginMeters));

	PatchMeters = FBox2d(Region.OriginMeters, Region.OriginMeters + Region.ExtentMeters);
}
