#include "MassifRiverRibbonMesh.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "MassifRiverNetwork.h"
#include "MassifUnits.h"

using MassifUnits::MetersToCentimeters;

using UE::Geometry::FDynamicMesh3;
using UE::Geometry::FDynamicMeshColorOverlay;
using UE::Geometry::FDynamicMeshNormalOverlay;
using UE::Geometry::FDynamicMeshUVOverlay;
using UE::Geometry::FIndex3i;

namespace
{
	/** Rotated a quarter turn from the flow, so it points at the right bank. */
	FVector2d AcrossFlow(FVector2d FlowDirection)
	{
		return FVector2d(FlowDirection.Y, -FlowDirection.X);
	}

	/**
	 * Which way the channel runs at a node: the chord over the neighbours it lies
	 * between, so a bend is crossed square rather than mitred to one of its sides.
	 */
	FVector2d FlowDirectionAt(const FMassifRiverReach& Reach, int32 NodeIndex)
	{
		const int32 BeforeIndex = FMath::Max(NodeIndex - 1, 0);
		const int32 AfterIndex = FMath::Min(NodeIndex + 1, Reach.Nodes.Num() - 1);
		const FVector2d Chord =
			Reach.Nodes[AfterIndex].PositionMeters - Reach.Nodes[BeforeIndex].PositionMeters;
		const FVector2d Direction = Chord.GetSafeNormal();
		if (Direction.IsNearlyZero())
		{
			// Two nodes on top of each other: any direction will do, and east is one.
			return FVector2d(1.0, 0.0);
		}
		return Direction;
	}

	FVector3f SurfaceNormalAt(const FMassifRiverReach& Reach, int32 NodeIndex, FVector2d FlowDirection)
	{
		const int32 BeforeIndex = FMath::Max(NodeIndex - 1, 0);
		const int32 AfterIndex = FMath::Min(NodeIndex + 1, Reach.Nodes.Num() - 1);
		const double RunMeters =
			(Reach.Nodes[AfterIndex].PositionMeters - Reach.Nodes[BeforeIndex].PositionMeters).Length();
		const double DropMeters =
			Reach.Nodes[AfterIndex].WaterSurfaceMeters - Reach.Nodes[BeforeIndex].WaterSurfaceMeters;

		const double Gradient = RunMeters > UE_DOUBLE_SMALL_NUMBER ? DropMeters / RunMeters : 0.0;
		const FVector3d AlongFlow = FVector3d(FlowDirection.X, FlowDirection.Y, Gradient).GetSafeNormal();
		const FVector2d Across = AcrossFlow(FlowDirection);
		const FVector3d Normal = FVector3d(Across.X, Across.Y, 0.0).Cross(AlongFlow);
		return FVector3f(Normal.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector3d::UpVector));
	}

	FVector4f ChannelColorAt(const FMassifRiverNode& Node)
	{
		using namespace MassifRiverRibbonMesh;
		return FVector4f(
			static_cast<float>(FMath::Clamp(Node.SpeedMetersPerSecond / SpeedReferenceMetersPerSecond, 0.0, 1.0)),
			static_cast<float>(FMath::Clamp(Node.DepthMeters / DepthReferenceMeters, 0.0, 1.0)),
			static_cast<float>(FMath::Clamp(Node.WidthMeters / WidthReferenceMeters, 0.0, 1.0)),
			1.0f);
	}

	/**
	 * Lays reaches down as triangle strips, one pair of vertices per node. Every
	 * overlay element is appended alongside its vertex, so the vertex index and
	 * the element index stay the same number and the triangles can be written
	 * against either.
	 */
	struct FRibbonBuilder
	{
		FDynamicMesh3& Mesh;
		FVector2d OriginMeters;
		FDynamicMeshUVOverlay* UVs = nullptr;
		FDynamicMeshUVOverlay* FlowUVs = nullptr;
		FDynamicMeshNormalOverlay* Normals = nullptr;
		FDynamicMeshColorOverlay* Colors = nullptr;

		void Prepare()
		{
			Mesh.EnableAttributes();
			Mesh.Attributes()->EnablePrimaryColors();
			Mesh.Attributes()->SetNumUVLayers(2);
			UVs = Mesh.Attributes()->PrimaryUV();
			FlowUVs = Mesh.Attributes()->GetUVLayer(1);
			Normals = Mesh.Attributes()->PrimaryNormals();
			Colors = Mesh.Attributes()->PrimaryColors();
		}

		void AppendReach(const FMassifRiverReach& Reach)
		{
			if (Reach.Nodes.Num() < 2)
			{
				return;
			}

			const int32 FirstVertex = Mesh.MaxVertexID();
			double AlongMeters = 0.0;
			for (int32 NodeIndex = 0; NodeIndex < Reach.Nodes.Num(); ++NodeIndex)
			{
				AlongMeters += DistanceToPreviousNode(Reach, NodeIndex);
				AppendNodeVertices(Reach, NodeIndex, AlongMeters);
			}
			AppendReachTriangles(FirstVertex, Reach.Nodes.Num());
		}

		double DistanceToPreviousNode(const FMassifRiverReach& Reach, int32 NodeIndex) const
		{
			if (NodeIndex == 0)
			{
				return 0.0;
			}
			return (Reach.Nodes[NodeIndex].PositionMeters - Reach.Nodes[NodeIndex - 1].PositionMeters).Length();
		}

		void AppendNodeVertices(const FMassifRiverReach& Reach, int32 NodeIndex, double AlongMeters)
		{
			const FMassifRiverNode& Node = Reach.Nodes[NodeIndex];
			const FVector2d FlowDirection = FlowDirectionAt(Reach, NodeIndex);
			const FVector2d HalfSpan = AcrossFlow(FlowDirection) * 0.5 * Node.WidthMeters;

			const FVector3f Normal = SurfaceNormalAt(Reach, NodeIndex, FlowDirection);
			const FVector4f Color = ChannelColorAt(Node);
			AppendBankVertex(Node, -HalfSpan, 0.0f, AlongMeters, FlowDirection, Normal, Color);
			AppendBankVertex(Node, HalfSpan, 1.0f, AlongMeters, FlowDirection, Normal, Color);
		}

		void AppendBankVertex(
			const FMassifRiverNode& Node, FVector2d Offset, float AcrossUv, double AlongMeters,
			FVector2d FlowDirection, FVector3f Normal, FVector4f Color)
		{
			const FVector2d PositionMeters = Node.PositionMeters + Offset - OriginMeters;
			Mesh.AppendVertex(FVector3d(
				PositionMeters.X * MetersToCentimeters,
				PositionMeters.Y * MetersToCentimeters,
				Node.WaterSurfaceMeters * MetersToCentimeters));

			UVs->AppendElement(FVector2f(AcrossUv, static_cast<float>(AlongMeters)));
			FlowUVs->AppendElement(FVector2f(FlowDirection));
			Normals->AppendElement(Normal);
			Colors->AppendElement(Color);
		}

		void AppendReachTriangles(int32 FirstVertex, int32 NodeCount)
		{
			for (int32 NodeIndex = 0; NodeIndex + 1 < NodeCount; ++NodeIndex)
			{
				const int32 LeftBank = FirstVertex + 2 * NodeIndex;
				AppendRibbonTriangle(LeftBank, LeftBank + 1, LeftBank + 3);
				AppendRibbonTriangle(LeftBank, LeftBank + 3, LeftBank + 2);
			}
		}

		void AppendRibbonTriangle(int32 VertexA, int32 VertexB, int32 VertexC)
		{
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
	};
}

TSharedPtr<FDynamicMesh3> MassifRiverRibbonMesh::BuildRibbonMesh(
	TArrayView<const FMassifRiverReach* const> Reaches, FVector2d OriginMeters)
{
	TSharedPtr<FDynamicMesh3> Mesh = MakeShared<FDynamicMesh3>();
	FRibbonBuilder Builder{ *Mesh, OriginMeters };
	Builder.Prepare();
	for (const FMassifRiverReach* Reach : Reaches)
	{
		Builder.AppendReach(*Reach);
	}
	return Mesh;
}
