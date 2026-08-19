#include "DynamicMesh/DynamicMesh3.h"
#include "MassifTerrainField.h"
#include "Misc/AutomationTest.h"
#include "Terrain/MassifTerrainTileMesh.h"

namespace
{
	FWorldGenConfig MakeTileTestConfig()
	{
		FWorldGenConfig Config;
		Config.MasterSeed = 1337;
		Config.WorldSizeMeters = 24000.0f;
		Config.RangeSpacingMeters = 9000.0f;
		Config.RangeWarpWavelengthMeters = 12000.0f;
		Config.RangeWarpMeters = 2500.0f;
		Config.RiverMouthCount = 6;
		Config.RiverMouthSpacingMeters = 4000.0f;
		Config.TrunkSegmentLengthMeters = 900.0f;
		Config.HeadwaterSegmentLengthMeters = 240.0f;
		Config.RiverMinNodeSpacingMeters = 200.0f;
		Config.RiverMaxNodeCount = 20000;
		return Config;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainTileMeshTest,
	"Massif.Presentation.TerrainTileMesh",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FTerrainTileMeshTest::RunTest(const FString& Parameters)
{
	const FMassifTerrainField Field(MakeTileTestConfig());
	const FVector2d TileCenterMeters(-1500.0, 800.0);
	const double TileSizeMeters = 128.0;
	const int32 QuadsPerSide = 64;

	const TSharedPtr<UE::Geometry::FDynamicMesh3> Mesh =
		MassifTerrainTileMesh::BuildTileMesh(Field, TileCenterMeters, TileSizeMeters, QuadsPerSide);
	if (!Mesh.IsValid())
	{
		AddError(TEXT("Tile mesh build returned nothing"));
		return false;
	}

	const int32 GridVertexCount = (QuadsPerSide + 1) * (QuadsPerSide + 1);
	TestTrue(FString::Printf(TEXT("Mesh has the grid and its skirt (%d vertices)"), Mesh->VertexCount()),
		Mesh->VertexCount() > GridVertexCount);

	// Grid vertices must sit exactly on the field: the mesh is a view of the
	// terrain, not a source of ground truth, and any offset here would make
	// tiles disagree with sim queries and with their neighbours.
	const double SpacingMeters = TileSizeMeters / QuadsPerSide;
	for (int32 VertexId = 0; VertexId < GridVertexCount; VertexId += 97)
	{
		const FVector3d LocalCentimeters = Mesh->GetVertex(VertexId);
		const FVector2d WorldMeters = TileCenterMeters + FVector2d(LocalCentimeters.X, LocalCentimeters.Y) / 100.0;
		const double ExpectedMeters = Field.SampleHeightMeters(WorldMeters, SpacingMeters);
		if (!FMath::IsNearlyEqual(LocalCentimeters.Z / 100.0, ExpectedMeters, 0.001))
		{
			AddError(FString::Printf(TEXT("Vertex %d is at %.4f m where the field says %.4f m"),
				VertexId, LocalCentimeters.Z / 100.0, ExpectedMeters));
			return false;
		}
	}

	// Skirts close the boundary: every remaining open edge must belong to the
	// skirt's own lower rim, never to the terrain surface.
	for (const int32 EdgeId : Mesh->BoundaryEdgeIndicesItr())
	{
		const UE::Geometry::FIndex2i EdgeVertices = Mesh->GetEdgeV(EdgeId);
		if (EdgeVertices.A < GridVertexCount && EdgeVertices.B < GridVertexCount)
		{
			AddError(TEXT("Terrain surface has an open boundary edge without a skirt"));
			return false;
		}
	}

	// Normals face up on average; a wound-backwards grid shows up here.
	FVector3d NormalSum = FVector3d::ZeroVector;
	for (int32 TriangleId = 0; TriangleId < QuadsPerSide * QuadsPerSide * 2; ++TriangleId)
	{
		NormalSum += Mesh->GetTriNormal(TriangleId);
	}
	TestTrue(TEXT("Surface triangles face upward"), NormalSum.Z > 0.0);

	return true;
}
