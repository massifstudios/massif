#include "MassifTerrainTileMesh.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "MassifTerrainField.h"

using UE::Geometry::FDynamicMesh3;
using UE::Geometry::FDynamicMeshNormalOverlay;

namespace
{
	constexpr double MetersToCentimeters = 100.0;

	/**
	 * Tile vertex heights plus a one-sample apron on every side, so normals can
	 * be central-differenced without sampling the field again per vertex.
	 */
	struct FTileHeightGrid
	{
		int32 QuadsPerSide = 0;
		double SpacingMeters = 0.0;
		TArray<double> HeightMeters;

		double At(int32 GridX, int32 GridY) const
		{
			const int32 Side = QuadsPerSide + 3;
			return HeightMeters[(GridY + 1) * Side + (GridX + 1)];
		}
	};

	FTileHeightGrid SampleHeightGrid(
		const FMassifTerrainField& Field, FVector2d TileCenterMeters, double TileSizeMeters, int32 QuadsPerSide)
	{
		FTileHeightGrid Grid;
		Grid.QuadsPerSide = QuadsPerSide;
		Grid.SpacingMeters = TileSizeMeters / QuadsPerSide;

		const int32 Side = QuadsPerSide + 3;
		const double HalfSizeMeters = TileSizeMeters * 0.5;
		Grid.HeightMeters.SetNumUninitialized(Side * Side);
		for (int32 SampleY = 0; SampleY < Side; ++SampleY)
		{
			const double PositionY = TileCenterMeters.Y - HalfSizeMeters + (SampleY - 1) * Grid.SpacingMeters;
			for (int32 SampleX = 0; SampleX < Side; ++SampleX)
			{
				const double PositionX = TileCenterMeters.X - HalfSizeMeters + (SampleX - 1) * Grid.SpacingMeters;
				Grid.HeightMeters[SampleY * Side + SampleX] =
					Field.SampleHeightMeters(FVector2d(PositionX, PositionY), Grid.SpacingMeters);
			}
		}
		return Grid;
	}

	FVector3f GridNormal(const FTileHeightGrid& Grid, int32 GridX, int32 GridY)
	{
		const FVector3d Normal(
			Grid.At(GridX - 1, GridY) - Grid.At(GridX + 1, GridY),
			Grid.At(GridX, GridY - 1) - Grid.At(GridX, GridY + 1),
			2.0 * Grid.SpacingMeters);
		return FVector3f(Normal.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector3d::UpVector));
	}

	void AppendGridTriangles(FDynamicMesh3& Mesh, int32 QuadsPerSide)
	{
		const int32 VerticesPerSide = QuadsPerSide + 1;
		for (int32 QuadY = 0; QuadY < QuadsPerSide; ++QuadY)
		{
			for (int32 QuadX = 0; QuadX < QuadsPerSide; ++QuadX)
			{
				const int32 Corner = QuadY * VerticesPerSide + QuadX;
				Mesh.AppendTriangle(Corner, Corner + VerticesPerSide + 1, Corner + 1);
				Mesh.AppendTriangle(Corner, Corner + VerticesPerSide, Corner + VerticesPerSide + 1);
			}
		}
	}

	void AppendSkirts(FDynamicMesh3& Mesh, double SkirtDropCentimeters)
	{
		TArray<int32> BoundaryEdges;
		for (const int32 EdgeId : Mesh.BoundaryEdgeIndicesItr())
		{
			BoundaryEdges.Add(EdgeId);
		}

		for (const int32 EdgeId : BoundaryEdges)
		{
			UE::Geometry::FIndex2i EdgeVertices = Mesh.GetEdgeV(EdgeId);
			// GetEdgeV order is by vertex index, not winding. The skirt has to run
			// against the interior triangle's direction to stay manifold and face
			// outward, so flip when the interior already runs A to B.
			const UE::Geometry::FIndex3i InteriorTriangle = Mesh.GetTriangle(Mesh.GetEdgeT(EdgeId).A);
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const bool bInteriorRunsAToB =
					InteriorTriangle[Corner] == EdgeVertices.A && InteriorTriangle[(Corner + 1) % 3] == EdgeVertices.B;
				if (bInteriorRunsAToB)
				{
					Swap(EdgeVertices.A, EdgeVertices.B);
				}
			}

			const FVector3d PositionA = Mesh.GetVertex(EdgeVertices.A);
			const FVector3d PositionB = Mesh.GetVertex(EdgeVertices.B);
			const int32 DroppedA = Mesh.AppendVertex(PositionA - FVector3d(0, 0, SkirtDropCentimeters));
			const int32 DroppedB = Mesh.AppendVertex(PositionB - FVector3d(0, 0, SkirtDropCentimeters));
			Mesh.AppendTriangle(EdgeVertices.A, EdgeVertices.B, DroppedB);
			Mesh.AppendTriangle(EdgeVertices.A, DroppedB, DroppedA);
		}
	}

	void WriteGridNormals(FDynamicMesh3& Mesh, const FTileHeightGrid& Grid, int32 GridVertexCount)
	{
		Mesh.EnableAttributes();
		FDynamicMeshNormalOverlay* NormalOverlay = Mesh.Attributes()->PrimaryNormals();
		NormalOverlay->ClearElements();

		const int32 VerticesPerSide = Grid.QuadsPerSide + 1;
		TArray<int32> VertexToElement;
		VertexToElement.SetNumUninitialized(Mesh.MaxVertexID());
		for (const int32 VertexId : Mesh.VertexIndicesItr())
		{
			// Skirt vertices reuse the boundary normal of the vertex above them,
			// which the grid lookup clamps to.
			const int32 GridIndex = FMath::Min(VertexId, GridVertexCount - 1);
			VertexToElement[VertexId] = NormalOverlay->AppendElement(
				GridNormal(Grid, GridIndex % VerticesPerSide, GridIndex / VerticesPerSide));
		}

		for (const int32 TriangleId : Mesh.TriangleIndicesItr())
		{
			const UE::Geometry::FIndex3i Triangle = Mesh.GetTriangle(TriangleId);
			NormalOverlay->SetTriangle(TriangleId,
				UE::Geometry::FIndex3i(VertexToElement[Triangle.A], VertexToElement[Triangle.B], VertexToElement[Triangle.C]));
		}
	}
}

TSharedPtr<FDynamicMesh3> MassifTerrainTileMesh::BuildTileMesh(
	const FMassifTerrainField& Field, FVector2d TileCenterMeters, double TileSizeMeters, int32 QuadsPerSide)
{
	const FTileHeightGrid Grid = SampleHeightGrid(Field, TileCenterMeters, TileSizeMeters, QuadsPerSide);

	TSharedPtr<FDynamicMesh3> Mesh = MakeShared<FDynamicMesh3>();
	const int32 VerticesPerSide = QuadsPerSide + 1;
	const double HalfSizeCentimeters = TileSizeMeters * 0.5 * MetersToCentimeters;
	for (int32 VertexY = 0; VertexY < VerticesPerSide; ++VertexY)
	{
		for (int32 VertexX = 0; VertexX < VerticesPerSide; ++VertexX)
		{
			Mesh->AppendVertex(FVector3d(
				-HalfSizeCentimeters + VertexX * Grid.SpacingMeters * MetersToCentimeters,
				-HalfSizeCentimeters + VertexY * Grid.SpacingMeters * MetersToCentimeters,
				Grid.At(VertexX, VertexY) * MetersToCentimeters));
		}
	}
	AppendGridTriangles(*Mesh, QuadsPerSide);

	const int32 GridVertexCount = Mesh->VertexCount();
	const double SkirtDropMeters = 4.0 + 2.0 * Grid.SpacingMeters;
	AppendSkirts(*Mesh, SkirtDropMeters * MetersToCentimeters);
	WriteGridNormals(*Mesh, Grid, GridVertexCount);

	return Mesh;
}
