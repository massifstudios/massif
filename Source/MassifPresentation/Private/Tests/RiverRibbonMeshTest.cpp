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
			Node.BedAltitudeMeters = Node.WaterSurfaceMeters - 0.25;
			Node.WidthMeters = RibbonTestWidthMeters;
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverRibbonMeshShapeTest,
	"Massif.Presentation.RiverRibbonMesh",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverRibbonMeshShapeTest::RunTest(const FString& Parameters)
{
	constexpr int32 NodeCount = 5;
	const FMassifRiverReach Reach = MakeStraightReach(NodeCount, 8.0);
	const TSharedPtr<FDynamicMesh3> Mesh = BuildFrom(Reach);

	TestEqual(TEXT("One vertex per bank per node"), Mesh->VertexCount(), 2 * NodeCount);
	TestEqual(TEXT("Two triangles between neighbouring nodes"), Mesh->TriangleCount(), 2 * (NodeCount - 1));

	// The reach runs east, so its banks are the pair north and south of it and
	// the span between them is the channel width the network solved.
	int32 NodesOffWidth = 0;
	int32 NodesOffWaterLevel = 0;
	for (int32 NodeIndex = 0; NodeIndex < NodeCount; ++NodeIndex)
	{
		const FVector3d LeftBank = Mesh->GetVertex(2 * NodeIndex);
		const FVector3d RightBank = Mesh->GetVertex(2 * NodeIndex + 1);
		const double SpanMeters = (RightBank - LeftBank).Length() / MetersToCentimeters;
		NodesOffWidth += FMath::IsNearlyEqual(SpanMeters, RibbonTestWidthMeters, 0.001) ? 0 : 1;

		const double SurfaceMeters = Reach.Nodes[NodeIndex].WaterSurfaceMeters * MetersToCentimeters;
		NodesOffWaterLevel += FMath::IsNearlyEqual(LeftBank.Z, SurfaceMeters, 0.1) ? 0 : 1;
		NodesOffWaterLevel += FMath::IsNearlyEqual(RightBank.Z, SurfaceMeters, 0.1) ? 0 : 1;
	}

	TestEqual(TEXT("The banks sit the channel's width apart"), NodesOffWidth, 0);
	TestEqual(TEXT("Both banks sit at the water surface"), NodesOffWaterLevel, 0);
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

	int32 BanksOffAcross = 0;
	for (int32 NodeIndex = 0; NodeIndex < 4; ++NodeIndex)
	{
		BanksOffAcross += ChannelUVs->GetElement(2 * NodeIndex).X == 0.0f ? 0 : 1;
		BanksOffAcross += ChannelUVs->GetElement(2 * NodeIndex + 1).X == 1.0f ? 0 : 1;
	}
	TestEqual(TEXT("The channel UV runs 0 to 1 from bank to bank"), BanksOffAcross, 0);

	// The reach runs east, and the material builds the ripples' frame out of this.
	const FVector2f Flow = FlowUVs->GetElement(0);
	TestTrue(TEXT("The flow direction points down the channel"),
		FMath::IsNearlyEqual(Flow.X, 1.0f, 0.001f) && FMath::IsNearlyZero(Flow.Y, 0.001f));
	return true;
}
