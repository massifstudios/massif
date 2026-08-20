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

	/**
	 * Where the sheet is drawn. The solved surface, except where the network's
	 * running minimum handed it down below the ground measured under it, which
	 * would bury the river.
	 */
	double SurfaceAltitudeAt(const FMassifRiverNode& Node)
	{
		using namespace MassifRiverRibbonMesh;
		return FMath::Max(Node.WaterSurfaceMeters, Node.GroundAltitudeMeters + SheetClearanceMeters);
	}

	/**
	 * How much deeper the solved channel is than the clearance the sheet has over
	 * the bed. The material multiplies the water's absorption and scattering by
	 * this, so a sheet a few centimetres above the ground still reads as the depth
	 * of water the catchment actually puts there.
	 */
	double ColumnScaleAt(const FMassifRiverNode& Node)
	{
		using namespace MassifRiverRibbonMesh;
		const double ClearanceMeters = SurfaceAltitudeAt(Node) - Node.GroundAltitudeMeters;
		return FMath::Clamp(Node.DepthMeters / ClearanceMeters, 1.0, ColumnScaleReference);
	}

	/**
	 * The radius the channel turns through at a node, from the angle between the
	 * segments meeting there over the distance they cover. Infinite down a straight
	 * run, where there is no bend to fold across.
	 *
	 * The two ends of a reach have no angle of their own, so they take the bend
	 * their one neighbour is in. Left unlimited they would overshoot the corner
	 * they stand on, which at a confluence is most of them.
	 */
	double TurningRadiusMeters(const FMassifRiverReach& Reach, int32 NodeIndex)
	{
		if (Reach.Nodes.Num() < 3)
		{
			return TNumericLimits<double>::Max();
		}

		const int32 TurnIndex = FMath::Clamp(NodeIndex, 1, Reach.Nodes.Num() - 2);
		const FVector2d Incoming =
			Reach.Nodes[TurnIndex].PositionMeters - Reach.Nodes[TurnIndex - 1].PositionMeters;
		const FVector2d Outgoing =
			Reach.Nodes[TurnIndex + 1].PositionMeters - Reach.Nodes[TurnIndex].PositionMeters;

		const double SpanMeters = 0.5 * (Incoming.Length() + Outgoing.Length());
		const double Cosine = FMath::Clamp(Incoming.GetSafeNormal().Dot(Outgoing.GetSafeNormal()), -1.0, 1.0);
		const double TurnRadians = FMath::Acos(Cosine);
		if (SpanMeters <= UE_DOUBLE_SMALL_NUMBER || TurnRadians <= UE_DOUBLE_SMALL_NUMBER)
		{
			return TNumericLimits<double>::Max();
		}
		return SpanMeters / TurnRadians;
	}

	/** Half the channel's width, narrowed through a bend tight enough to fold it. */
	double HalfWidthMeters(const FMassifRiverReach& Reach, int32 NodeIndex)
	{
		using namespace MassifRiverRibbonMesh;
		const double BendLimitMeters = BendHalfWidthFraction * TurningRadiusMeters(Reach, NodeIndex);
		return FMath::Min(0.5 * Reach.Nodes[NodeIndex].WidthMeters, BendLimitMeters);
	}

	FVector4f ChannelColorAt(const FMassifRiverNode& Node)
	{
		using namespace MassifRiverRibbonMesh;
		return FVector4f(
			static_cast<float>(FMath::Clamp(Node.SpeedMetersPerSecond / SpeedReferenceMetersPerSecond, 0.0, 1.0)),
			static_cast<float>(FMath::Clamp(Node.DepthMeters / DepthReferenceMeters, 0.0, 1.0)),
			static_cast<float>(FMath::Clamp(Node.WidthMeters / WidthReferenceMeters, 0.0, 1.0)),
			static_cast<float>(ColumnScaleAt(Node) / ColumnScaleReference));
	}

	/** Everything the four vertices of one cross-section are placed from. */
	struct FCrossSection
	{
		FVector2d CentreMeters = FVector2d::ZeroVector;

		/** From the centre to the right bank; the left one is its negative. */
		FVector2d HalfSpanMeters = FVector2d::ZeroVector;

		double SurfaceAltitudeMeters = 0.0;
		double GroundAltitudeMeters = 0.0;
		double AlongMeters = 0.0;
		FVector2d FlowDirection = FVector2d::ZeroVector;

		/** Level across the flow, carrying the channel's gradient along it. */
		FVector3f SurfaceNormal = FVector3f::UpVector;

		/** Horizontal, out of the channel towards the right bank. What a skirt faces. */
		FVector3f OutwardNormal = FVector3f::ZeroVector;

		FVector4f Color = FVector4f::Zero();
	};

	/**
	 * Lays reaches down as triangle strips, four vertices per node. Every overlay
	 * element is appended alongside its vertex, so the vertex index and the element
	 * index stay the same number and the triangles can be written against either.
	 */
	struct FRibbonBuilder
	{
		/** A skirt foot, the sheet's edge, its other edge, the other skirt foot. */
		static constexpr int32 VerticesPerNode = 4;

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

		static FCrossSection MakeCrossSection(
			const FMassifRiverReach& Reach, int32 NodeIndex, double AlongMeters)
		{
			const FMassifRiverNode& Node = Reach.Nodes[NodeIndex];
			const FVector2d FlowDirection = FlowDirectionAt(Reach, NodeIndex);
			const FVector2d Across = AcrossFlow(FlowDirection);

			FCrossSection Section;
			Section.CentreMeters = Node.PositionMeters;
			Section.HalfSpanMeters = Across * HalfWidthMeters(Reach, NodeIndex);
			Section.SurfaceAltitudeMeters = SurfaceAltitudeAt(Node);
			Section.GroundAltitudeMeters = Node.GroundAltitudeMeters;
			Section.AlongMeters = AlongMeters;
			Section.FlowDirection = FlowDirection;
			Section.SurfaceNormal = SurfaceNormalAt(Reach, NodeIndex, FlowDirection);
			Section.OutwardNormal = FVector3f(static_cast<float>(Across.X), static_cast<float>(Across.Y), 0.0f);
			Section.Color = ChannelColorAt(Node);
			return Section;
		}

		/** Left skirt foot, left edge, right edge, right skirt foot, in that order. */
		void AppendNodeVertices(const FMassifRiverReach& Reach, int32 NodeIndex, double AlongMeters)
		{
			const FCrossSection Section = MakeCrossSection(Reach, NodeIndex, AlongMeters);
			AppendCrossSectionVertex(Section, -1.0, 0.0f, Section.GroundAltitudeMeters, -Section.OutwardNormal);
			AppendCrossSectionVertex(Section, -1.0, 0.0f, Section.SurfaceAltitudeMeters, Section.SurfaceNormal);
			AppendCrossSectionVertex(Section, 1.0, 1.0f, Section.SurfaceAltitudeMeters, Section.SurfaceNormal);
			AppendCrossSectionVertex(Section, 1.0, 1.0f, Section.GroundAltitudeMeters, Section.OutwardNormal);
		}

		void AppendCrossSectionVertex(
			const FCrossSection& Section, double SideSign, float AcrossUv, double AltitudeMeters, FVector3f Normal)
		{
			const FVector2d PositionMeters =
				Section.CentreMeters + SideSign * Section.HalfSpanMeters - OriginMeters;
			Mesh.AppendVertex(FVector3d(
				PositionMeters.X * MetersToCentimeters,
				PositionMeters.Y * MetersToCentimeters,
				AltitudeMeters * MetersToCentimeters));

			UVs->AppendElement(FVector2f(AcrossUv, static_cast<float>(Section.AlongMeters)));
			FlowUVs->AppendElement(FVector2f(Section.FlowDirection));
			Normals->AppendElement(Normal);
			Colors->AppendElement(Section.Color);
		}

		/** Three strips per span: the left skirt, the sheet between the edges, the right skirt. */
		void AppendReachTriangles(int32 FirstVertex, int32 NodeCount)
		{
			for (int32 NodeIndex = 0; NodeIndex + 1 < NodeCount; ++NodeIndex)
			{
				const int32 Section = FirstVertex + VerticesPerNode * NodeIndex;
				for (int32 Strip = 0; Strip + 1 < VerticesPerNode; ++Strip)
				{
					AppendRibbonQuad(Section + Strip, Section + Strip + 1);
				}
			}
		}

		/**
		 * The quad between one pair of neighbouring vertices in a cross-section and
		 * the same pair in the next, wound so its face points out of the water.
		 */
		void AppendRibbonQuad(int32 Left, int32 Right)
		{
			const int32 NextLeft = Left + VerticesPerNode;
			const int32 NextRight = Right + VerticesPerNode;
			AppendRibbonTriangle(Left, Right, NextRight);
			AppendRibbonTriangle(Left, NextRight, NextLeft);
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
