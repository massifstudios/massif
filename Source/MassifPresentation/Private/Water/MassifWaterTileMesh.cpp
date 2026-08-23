#include "MassifWaterTileMesh.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "MassifRiverField.h"
#include "MassifUnits.h"

using MassifUnits::MetersToCentimeters;

using UE::Geometry::FDynamicMesh3;
using UE::Geometry::FDynamicMeshColorOverlay;
using UE::Geometry::FDynamicMeshNormalOverlay;
using UE::Geometry::FDynamicMeshUVOverlay;
using UE::Geometry::FIndex3i;

namespace
{
	using namespace MassifWaterTileMesh;

	constexpr int32 VerticesPerSide = QuadsPerSide + 1;

	/** Smallest triangle worth keeping, in square centimetres. */
	constexpr double MinTriangleAreaCentimetersSquared = 1.0e-4;

	/**
	 * How close to the end of a lattice edge a crossing is taken to be that end.
	 *
	 * A hair's breadth of an edge, and it has to be taken: a crossing that lands on
	 * a lattice point would otherwise be a second vertex in the same place as the
	 * first, and which cell produces it depends on which way that cell's diagonal
	 * runs — so two tiles sharing a boundary would disagree about how many vertices
	 * are on it. Snapping keeps one vertex per place, and the sliver triangles
	 * around it never exist to be culled.
	 */
	constexpr double CrossingSnapFraction = 1.0e-3;

	/** Metres out of a box, negative inside it. */
	double DistanceOutOfBoxMeters(const FBox2d& BoxMeters, FVector2d PositionMeters)
	{
		const FVector2d OutwardMeters = FVector2d(
			FMath::Abs(PositionMeters.X - BoxMeters.GetCenter().X),
			FMath::Abs(PositionMeters.Y - BoxMeters.GetCenter().Y)) - BoxMeters.GetExtent();
		const double OutsideMeters =
			FVector2d(FMath::Max(OutwardMeters.X, 0.0), FMath::Max(OutwardMeters.Y, 0.0)).Length();
		return OutsideMeters + FMath::Min(FMath::Max(OutwardMeters.X, OutwardMeters.Y), 0.0);
	}

	/**
	 * How far the point is from water this surface may draw: outside the nearest
	 * channel, or inside a box some other surface is drawing.
	 *
	 * The larger of the two, which is the intersection of the two regions and
	 * still a distance — so the crossing solve interpolates onto the box's edge
	 * the same way it interpolates onto a bank.
	 */
	double DrawableDistanceMeters(
		const FMassifRiverField& RiverField, FVector2d PositionMeters, int32 TimeSlice,
		const FBox2d& SolvedPatchMeters)
	{
		const double ChannelMeters =
			RiverField.SampleSignedDistanceMeters(PositionMeters, TimeSlice);
		if (!SolvedPatchMeters.bIsValid)
		{
			return ChannelMeters;
		}
		return FMath::Max(ChannelMeters, -DistanceOutOfBoxMeters(SolvedPatchMeters, PositionMeters));
	}

	/** Signed distance to the water's edge over the tile's own lattice. */
	struct FWaterLattice
	{
		FVector2d OriginMeters = FVector2d::ZeroVector;
		TArray<double> SignedDistanceMeters;

		double At(int32 LatticeIndex) const
		{
			return SignedDistanceMeters[LatticeIndex];
		}

		bool IsWet(int32 LatticeIndex) const
		{
			return SignedDistanceMeters[LatticeIndex] <= 0.0;
		}

		FVector2d PositionMeters(int32 LatticeIndex) const
		{
			return OriginMeters
				+ FVector2d(LatticeIndex % VerticesPerSide, LatticeIndex / VerticesPerSide) * CellSizeMeters;
		}
	};

	FWaterLattice SampleLattice(
		const FMassifRiverField& RiverField, FVector2d OriginMeters, int32 TimeSlice,
		const FBox2d& SolvedPatchMeters)
	{
		FWaterLattice Lattice;
		Lattice.OriginMeters = OriginMeters;
		Lattice.SignedDistanceMeters.SetNumUninitialized(VerticesPerSide * VerticesPerSide);
		for (int32 LatticeIndex = 0; LatticeIndex < Lattice.SignedDistanceMeters.Num(); ++LatticeIndex)
		{
			Lattice.SignedDistanceMeters[LatticeIndex] = DrawableDistanceMeters(
				RiverField, Lattice.PositionMeters(LatticeIndex), TimeSlice, SolvedPatchMeters);
		}
		return Lattice;
	}

	FVector4f ChannelColor(const FMassifRiverSample& River)
	{
		return FVector4f(
			static_cast<float>(FMath::Clamp(River.SpeedMetersPerSecond / SpeedReferenceMetersPerSecond, 0.0, 1.0)),
			static_cast<float>(FMath::Clamp(River.DepthMeters / DepthReferenceMeters, 0.0, 1.0)),
			static_cast<float>(FMath::Clamp(River.WidthMeters / WidthReferenceMeters, 0.0, 1.0)),
			static_cast<float>(1.0 / ColumnScaleReference));
	}

	/**
	 * Cuts the wet part out of the lattice, one cell at a time.
	 *
	 * Every vertex is either a lattice point or the crossing on a lattice edge
	 * where the distance changes sign, and both are cached by the thing they
	 * belong to rather than by the cell that asked for them. So a crossing is
	 * computed once from one ordered pair of distances, and two cells — or two
	 * tiles — sharing that edge get the same point rather than two points that
	 * agree to within a rounding error.
	 */
	struct FWaterTileBuilder
	{
		const FMassifRiverField& RiverField;
		const FWaterLattice& Lattice;
		FDynamicMesh3& Mesh;
		int32 TimeSlice = 0;

		FDynamicMeshUVOverlay* UVs = nullptr;
		FDynamicMeshUVOverlay* FlowUVs = nullptr;
		FDynamicMeshNormalOverlay* Normals = nullptr;
		FDynamicMeshColorOverlay* Colors = nullptr;

		TArray<int32> LatticeVertices;
		TArray<int32> HorizontalCrossings;
		TArray<int32> VerticalCrossings;
		TArray<int32> DiagonalCrossings;

		void Prepare()
		{
			Mesh.EnableAttributes();
			Mesh.Attributes()->EnablePrimaryColors();
			Mesh.Attributes()->SetNumUVLayers(2);
			UVs = Mesh.Attributes()->PrimaryUV();
			FlowUVs = Mesh.Attributes()->GetUVLayer(1);
			Normals = Mesh.Attributes()->PrimaryNormals();
			Colors = Mesh.Attributes()->PrimaryColors();

			LatticeVertices.Init(INDEX_NONE, VerticesPerSide * VerticesPerSide);
			HorizontalCrossings.Init(INDEX_NONE, QuadsPerSide * VerticesPerSide);
			VerticalCrossings.Init(INDEX_NONE, VerticesPerSide * QuadsPerSide);
			DiagonalCrossings.Init(INDEX_NONE, QuadsPerSide * QuadsPerSide);
		}

		void Run()
		{
			for (int32 CellY = 0; CellY < QuadsPerSide; ++CellY)
			{
				AppendCellRow(CellY);
			}
		}

	private:
		void AppendCellRow(int32 CellY)
		{
			for (int32 CellX = 0; CellX < QuadsPerSide; ++CellX)
			{
				AppendCell(CellX, CellY);
			}
		}

		/**
		 * Both triangles of one cell, split on the same diagonal the terrain tiles
		 * use, and wound the same way. Clipping a triangle rather than the whole
		 * cell keeps the cases down to what a walk round three edges produces:
		 * splitting a square has an ambiguity in the middle that a triangle cannot.
		 */
		void AppendCell(int32 CellX, int32 CellY)
		{
			const int32 LowerLeft = CellY * VerticesPerSide + CellX;
			const int32 LowerRight = LowerLeft + 1;
			const int32 UpperLeft = LowerLeft + VerticesPerSide;
			const int32 UpperRight = UpperLeft + 1;
			if (!IsCellWet(LowerLeft, LowerRight, UpperLeft, UpperRight))
			{
				return;
			}

			AppendClippedTriangle(LowerLeft, UpperRight, LowerRight);
			AppendClippedTriangle(LowerLeft, UpperLeft, UpperRight);
		}

		bool IsCellWet(int32 LowerLeft, int32 LowerRight, int32 UpperLeft, int32 UpperRight) const
		{
			return Lattice.IsWet(LowerLeft) || Lattice.IsWet(LowerRight)
				|| Lattice.IsWet(UpperLeft) || Lattice.IsWet(UpperRight);
		}

		/**
		 * The wet part of one triangle, by walking its three edges in order: a
		 * corner is kept if it is wet, and where an edge changes sign the crossing
		 * on it is taken. Walking in order is what carries the winding through, so
		 * the piece that comes out faces the same way the whole triangle did.
		 */
		void AppendClippedTriangle(int32 CornerA, int32 CornerB, int32 CornerC)
		{
			const int32 Corners[3] = { CornerA, CornerB, CornerC };
			TArray<int32, TInlineAllocator<4>> Polygon;
			for (int32 Edge = 0; Edge < 3; ++Edge)
			{
				AppendClippedEdge(Corners[Edge], Corners[(Edge + 1) % 3], Polygon);
			}

			for (int32 Corner = 1; Corner + 1 < Polygon.Num(); ++Corner)
			{
				AppendTriangle(Polygon[0], Polygon[Corner], Polygon[Corner + 1]);
			}
		}

		void AppendClippedEdge(int32 From, int32 To, TArray<int32, TInlineAllocator<4>>& InOutPolygon)
		{
			if (Lattice.IsWet(From))
			{
				InOutPolygon.Add(GetOrCreateLatticeVertex(From));
			}
			if (Lattice.IsWet(From) != Lattice.IsWet(To))
			{
				InOutPolygon.Add(GetOrCreateCrossing(From, To));
			}
		}

		void AppendTriangle(int32 VertexA, int32 VertexB, int32 VertexC)
		{
			if (IsDegenerate(VertexA, VertexB, VertexC))
			{
				return;
			}

			const int32 TriangleId = Mesh.AppendTriangle(VertexA, VertexB, VertexC);
			if (TriangleId < 0)
			{
				return;
			}

			const FIndex3i Corners(VertexA, VertexB, VertexC);
			UVs->SetTriangle(TriangleId, Corners);
			FlowUVs->SetTriangle(TriangleId, Corners);
			Normals->SetTriangle(TriangleId, Corners);
			Colors->SetTriangle(TriangleId, Corners);
		}

		/**
		 * A crossing that lands exactly on a lattice point makes a triangle with no
		 * area out of two vertices in the same place. Harmless to look at and not
		 * harmless to keep.
		 */
		bool IsDegenerate(int32 VertexA, int32 VertexB, int32 VertexC) const
		{
			const FVector3d Edge0 = Mesh.GetVertex(VertexB) - Mesh.GetVertex(VertexA);
			const FVector3d Edge1 = Mesh.GetVertex(VertexC) - Mesh.GetVertex(VertexA);
			return 0.5 * Edge0.Cross(Edge1).Length() < MinTriangleAreaCentimetersSquared;
		}

		int32 GetOrCreateLatticeVertex(int32 LatticeIndex)
		{
			if (LatticeVertices[LatticeIndex] == INDEX_NONE)
			{
				LatticeVertices[LatticeIndex] = AppendWaterVertex(Lattice.PositionMeters(LatticeIndex));
			}
			return LatticeVertices[LatticeIndex];
		}

		/**
		 * Where the water's edge crosses one lattice edge. Always solved from the
		 * lower lattice index towards the higher, so the two cells sharing the edge
		 * ask the same question and get the same answer back.
		 */
		int32 GetOrCreateCrossing(int32 From, int32 To)
		{
			const int32 Lower = FMath::Min(From, To);
			const int32 Upper = FMath::Max(From, To);
			int32& Cached = FindCrossingSlot(Lower, Upper);
			if (Cached != INDEX_NONE)
			{
				return Cached;
			}

			const double Fraction = SolveCrossingFraction(Lower, Upper);
			if (Fraction <= CrossingSnapFraction)
			{
				Cached = GetOrCreateLatticeVertex(Lower);
				return Cached;
			}
			if (Fraction >= 1.0 - CrossingSnapFraction)
			{
				Cached = GetOrCreateLatticeVertex(Upper);
				return Cached;
			}

			Cached = AppendWaterVertex(
				FMath::Lerp(Lattice.PositionMeters(Lower), Lattice.PositionMeters(Upper), Fraction));
			return Cached;
		}

		double SolveCrossingFraction(int32 Lower, int32 Upper) const
		{
			const double LowerDistance = Lattice.At(Lower);
			const double Span = LowerDistance - Lattice.At(Upper);
			if (FMath::Abs(Span) <= UE_DOUBLE_SMALL_NUMBER)
			{
				return 0.5;
			}
			return FMath::Clamp(LowerDistance / Span, 0.0, 1.0);
		}

		/** The cache slot for one lattice edge, chosen by which way the edge runs. */
		int32& FindCrossingSlot(int32 Lower, int32 Upper)
		{
			const int32 LowerX = Lower % VerticesPerSide;
			const int32 LowerY = Lower / VerticesPerSide;
			if (Upper - Lower == 1)
			{
				return HorizontalCrossings[LowerY * QuadsPerSide + LowerX];
			}
			if (Upper - Lower == VerticesPerSide)
			{
				return VerticalCrossings[LowerY * VerticesPerSide + LowerX];
			}
			return DiagonalCrossings[LowerY * QuadsPerSide + LowerX];
		}

		int32 AppendWaterVertex(FVector2d PositionMeters)
		{
			const FMassifRiverSample River = RiverField.Sample(PositionMeters, TimeSlice);
			const FVector2d LocalMeters = PositionMeters - Lattice.OriginMeters;

			const int32 VertexId = Mesh.AppendVertex(FVector3d(
				LocalMeters.X * MetersToCentimeters,
				LocalMeters.Y * MetersToCentimeters,
				River.WaterSurfaceMeters * MetersToCentimeters));

			UVs->AppendElement(FVector2f(
				static_cast<float>(0.5 * (River.AcrossFraction + 1.0)),
				static_cast<float>(River.AlongMeters)));
			FlowUVs->AppendElement(FVector2f(River.FlowDirection));
			Normals->AppendElement(FVector3f::UpVector);
			Colors->AppendElement(ChannelColor(River));
			return VertexId;
		}
	};
}

FVector2d MassifWaterTileMesh::TileOriginMeters(int32 TileX, int32 TileY)
{
	return FVector2d(TileX, TileY) * TileSizeMeters;
}

TSharedPtr<FDynamicMesh3> MassifWaterTileMesh::BuildWaterTileMesh(
	const FMassifRiverField& RiverField, int32 TileX, int32 TileY, int32 TimeSlice,
	const FBox2d& SolvedPatchMeters)
{
	TSharedPtr<FDynamicMesh3> Mesh = MakeShared<FDynamicMesh3>();

	const FVector2d OriginMeters = TileOriginMeters(TileX, TileY);
	if (!RiverField.IntersectsBox(OriginMeters, OriginMeters + FVector2d(TileSizeMeters, TileSizeMeters)))
	{
		return Mesh;
	}

	const FWaterLattice Lattice = SampleLattice(RiverField, OriginMeters, TimeSlice, SolvedPatchMeters);
	FWaterTileBuilder Builder{ RiverField, Lattice, *Mesh, TimeSlice };
	Builder.Prepare();
	Builder.Run();
	return Mesh;
}
