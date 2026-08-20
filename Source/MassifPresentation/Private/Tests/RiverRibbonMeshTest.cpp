#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "MassifRiverNetwork.h"
#include "MassifUnits.h"
#include "Misc/AutomationTest.h"
#include "Water/MassifRiverRibbonMesh.h"

using MassifUnits::MetersToCentimeters;
using UE::Geometry::FDynamicMesh3;

namespace
{
	constexpr double RibbonTestWidthMeters = 10.0;
	constexpr double RibbonTestSurfaceMeters = 1200.0;
	constexpr int32 VerticesPerNode = 4;

	/** A straight reach running east, so the channel's frame and the world's line up. */
	FMassifRiverReach MakeStraightReach(int32 NodeCount, double SpacingMeters)
	{
		FMassifRiverReach Reach;
		for (int32 NodeIndex = 0; NodeIndex < NodeCount; ++NodeIndex)
		{
			FMassifRiverNode Node;
			Node.PositionMeters = FVector2d(NodeIndex * SpacingMeters, 0.0);
			// Falling east, so the reach has a gradient to take a normal from.
			Node.WaterSurfaceMeters = RibbonTestSurfaceMeters - NodeIndex;
			Node.BedAltitudeMeters = Node.WaterSurfaceMeters - 0.5;
			Node.WidthMeters = RibbonTestWidthMeters;
			Node.DepthMeters = 0.5;
			Node.SpeedMetersPerSecond = 1.0;
			Reach.Nodes.Add(Node);
		}
		return Reach;
	}

	/**
	 * A half circle far tighter than the channel is wide: half the width is twice
	 * the radius, so an unclamped offset crosses itself all the way round.
	 */
	FMassifRiverReach MakeHairpinReach(int32 NodeCount, double RadiusMeters, double WidthMeters)
	{
		FMassifRiverReach Reach;
		for (int32 NodeIndex = 0; NodeIndex < NodeCount; ++NodeIndex)
		{
			const double Turned = UE_DOUBLE_PI * NodeIndex / (NodeCount - 1);
			FMassifRiverNode Node;
			Node.PositionMeters = RadiusMeters * FVector2d(FMath::Cos(Turned), FMath::Sin(Turned));
			Node.WaterSurfaceMeters = RibbonTestSurfaceMeters - NodeIndex;
			Node.BedAltitudeMeters = Node.WaterSurfaceMeters - 0.5;
			Node.WidthMeters = WidthMeters;
			Node.DepthMeters = 0.5;
			Node.SpeedMetersPerSecond = 1.0;
			Reach.Nodes.Add(Node);
		}
		return Reach;
	}

	TSharedPtr<FDynamicMesh3> BuildFrom(const FMassifRiverReach& Reach)
	{
		const FMassifRiverReach* Reaches[] = { &Reach };
		return MassifRiverRibbonMesh::BuildRibbonMesh(Reaches, FVector2d::ZeroVector);
	}

	FVector3d VertexAt(const FDynamicMesh3& Mesh, int32 NodeIndex, int32 Corner)
	{
		return Mesh.GetVertex(VerticesPerNode * NodeIndex + Corner);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverRibbonMeshShapeTest,
	"Massif.Presentation.RiverRibbonMesh",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverRibbonMeshShapeTest::RunTest(const FString& Parameters)
{
	constexpr int32 NodeCount = 5;
	const FMassifRiverReach Reach = MakeStraightReach(NodeCount, 8.0);
	const TSharedPtr<FDynamicMesh3> Mesh = BuildFrom(Reach);

	TestEqual(TEXT("Two sheet edges and two skirt feet per node"),
		Mesh->VertexCount(), VerticesPerNode * NodeCount);
	TestEqual(TEXT("Six triangles between neighbouring nodes"),
		Mesh->TriangleCount(), 6 * (NodeCount - 1));

	// The reach runs east, so its edges are the pair north and south of it and the
	// span between them is the channel width the network solved.
	int32 NodesOffWidth = 0;
	int32 EdgesOffSurface = 0;
	int32 FeetOffBed = 0;
	int32 FeetOffPlumb = 0;
	for (int32 NodeIndex = 0; NodeIndex < NodeCount; ++NodeIndex)
	{
		const FVector3d LeftFoot = VertexAt(*Mesh, NodeIndex, 0);
		const FVector3d LeftEdge = VertexAt(*Mesh, NodeIndex, 1);
		const FVector3d RightEdge = VertexAt(*Mesh, NodeIndex, 2);
		const FVector3d RightFoot = VertexAt(*Mesh, NodeIndex, 3);

		const double SpanMeters = (RightEdge - LeftEdge).Length() / MetersToCentimeters;
		NodesOffWidth += FMath::IsNearlyEqual(SpanMeters, RibbonTestWidthMeters, 0.001) ? 0 : 1;

		const double SurfaceMeters = Reach.Nodes[NodeIndex].WaterSurfaceMeters * MetersToCentimeters;
		const double BedMeters = Reach.Nodes[NodeIndex].BedAltitudeMeters * MetersToCentimeters;
		EdgesOffSurface += FMath::IsNearlyEqual(LeftEdge.Z, SurfaceMeters, 0.1) ? 0 : 1;
		EdgesOffSurface += FMath::IsNearlyEqual(RightEdge.Z, SurfaceMeters, 0.1) ? 0 : 1;
		FeetOffBed += FMath::IsNearlyEqual(LeftFoot.Z, BedMeters, 0.1) ? 0 : 1;
		FeetOffBed += FMath::IsNearlyEqual(RightFoot.Z, BedMeters, 0.1) ? 0 : 1;

		FeetOffPlumb += FVector2d(LeftFoot.X, LeftFoot.Y).Equals(FVector2d(LeftEdge.X, LeftEdge.Y), 0.1) ? 0 : 1;
		FeetOffPlumb += FVector2d(RightFoot.X, RightFoot.Y).Equals(FVector2d(RightEdge.X, RightEdge.Y), 0.1) ? 0 : 1;
	}

	TestEqual(TEXT("The sheet's edges sit the channel's width apart"), NodesOffWidth, 0);
	// A water surface is level across the flow. Everything the material does with
	// the across direction assumes it, and a sheet that is not level across reads
	// as a ridge running down the valley rather than as water in it.
	TestEqual(TEXT("Both edges sit at the water surface"), EdgesOffSurface, 0);
	TestEqual(TEXT("The skirts reach down to the channel invert"), FeetOffBed, 0);
	TestEqual(TEXT("The skirts hang straight down from the edges"), FeetOffPlumb, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverRibbonBendTest,
	"Massif.Presentation.RiverRibbonBend",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverRibbonBendTest::RunTest(const FString& Parameters)
{
	// Half the width is twice the radius here, so an offset taken at the full
	// channel width crosses itself right round the inside of the turn.
	constexpr int32 NodeCount = 12;
	const FMassifRiverReach Reach = MakeHairpinReach(NodeCount, 10.0, 40.0);
	const TSharedPtr<FDynamicMesh3> Mesh = BuildFrom(Reach);

	const UE::Geometry::FDynamicMeshUVOverlay* FlowUVs = Mesh->Attributes()->GetUVLayer(1);

	// A fold is an edge running back up the channel: the sheet doubles over and the
	// water is drawn twice, once in each direction.
	int32 EdgesRunningBackwards = 0;
	for (int32 NodeIndex = 0; NodeIndex + 1 < NodeCount; ++NodeIndex)
	{
		const FVector2f Flow = FlowUVs->GetElement(VerticesPerNode * NodeIndex + 1);
		for (int32 Corner = 1; Corner <= 2; ++Corner)
		{
			const FVector3d Step = VertexAt(*Mesh, NodeIndex + 1, Corner) - VertexAt(*Mesh, NodeIndex, Corner);
			const double Advance = Step.X * Flow.X + Step.Y * Flow.Y;
			EdgesRunningBackwards += Advance > 0.0 ? 0 : 1;
		}
	}

	TestEqual(TEXT("Neither edge folds back through a hairpin"), EdgesRunningBackwards, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverRibbonMeshAttributesTest,
	"Massif.Presentation.RiverRibbonAttributes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverRibbonMeshAttributesTest::RunTest(const FString& Parameters)
{
	const FMassifRiverReach Reach = MakeStraightReach(4, 8.0);
	const TSharedPtr<FDynamicMesh3> Mesh = BuildFrom(Reach);

	// The material reads all four of these and has no other way to know the
	// channel it is drawing, so a missing one is a silently wrong river.
	if (!TestTrue(TEXT("The ribbon carries a channel UV and a flow direction"),
		Mesh->Attributes()->NumUVLayers() == 2))
	{
		return false;
	}
	TestTrue(TEXT("The ribbon carries vertex colours"), Mesh->Attributes()->HasPrimaryColors());

	const UE::Geometry::FDynamicMeshUVOverlay* ChannelUVs = Mesh->Attributes()->PrimaryUV();
	const UE::Geometry::FDynamicMeshUVOverlay* FlowUVs = Mesh->Attributes()->GetUVLayer(1);

	// V is in metres along the channel rather than a 0-1 run, so a texture
	// scrolled along it travels at the same rate however wide the channel is.
	const float FirstAlong = ChannelUVs->GetElement(0).Y;
	const float LastAlong = ChannelUVs->GetElement(Mesh->VertexCount() - 1).Y;
	TestTrue(TEXT("The channel UV runs the reach's length in metres"),
		FMath::IsNearlyEqual(LastAlong - FirstAlong, 24.0f, 0.01f));

	// The skirts take the U of the edge they hang from, so the bank fade and the
	// bank foam carry on down them instead of stopping at the waterline.
	int32 CornersOffAcross = 0;
	for (int32 NodeIndex = 0; NodeIndex < 4; ++NodeIndex)
	{
		const int32 Section = VerticesPerNode * NodeIndex;
		CornersOffAcross += ChannelUVs->GetElement(Section).X == 0.0f ? 0 : 1;
		CornersOffAcross += ChannelUVs->GetElement(Section + 1).X == 0.0f ? 0 : 1;
		CornersOffAcross += ChannelUVs->GetElement(Section + 2).X == 1.0f ? 0 : 1;
		CornersOffAcross += ChannelUVs->GetElement(Section + 3).X == 1.0f ? 0 : 1;
	}
	TestEqual(TEXT("The channel UV runs 0 to 1 from bank to bank"), CornersOffAcross, 0);

	// The material multiplies the water's coefficients by this, so a zero here is
	// a river with no colour in it at all.
	const UE::Geometry::FDynamicMeshColorOverlay* Colors = Mesh->Attributes()->PrimaryColors();
	TestTrue(TEXT("Vertex alpha carries how far the channel outruns the sheet's clearance"),
		Colors->GetElement(0).W > 0.0f);

	// The reach runs east, and the material builds the ripples' frame out of this.
	const FVector2f Flow = FlowUVs->GetElement(0);
	TestTrue(TEXT("The flow direction points down the channel"),
		FMath::IsNearlyEqual(Flow.X, 1.0f, 0.001f) && FMath::IsNearlyZero(Flow.Y, 0.001f));
	return true;
}
