#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "MassifRiverField.h"
#include "MassifRiverNetwork.h"
#include "MassifUnits.h"
#include "Misc/AutomationTest.h"
#include "Water/MassifWaterTileMesh.h"

using MassifUnits::MetersToCentimeters;
using UE::Geometry::FDynamicMesh3;

namespace
{
	constexpr double WaterTestWidthMeters = 6.0;
	constexpr double WaterTestBankReachMeters = 24.0;

	/**
	 * A channel running east along Y = 96, which is the middle of the tile row the
	 * tests use and well clear of any tile boundary running the other way.
	 */
	TSharedRef<const FMassifRiverField> MakeRiverField(double NorthingMeters)
	{
		FMassifRiverReach Reach;
		for (int32 NodeIndex = 0; NodeIndex < 60; ++NodeIndex)
		{
			const double EastingMeters = -64.0 + NodeIndex * 8.0;

			FMassifRiverNode Node;
			Node.PositionMeters = FVector2d(EastingMeters, NorthingMeters);
			Node.GroundAltitudeMeters = 1500.0 - 0.02 * EastingMeters;
			Node.WaterSurfaceMeters = Node.GroundAltitudeMeters;
			Node.BedAltitudeMeters = Node.WaterSurfaceMeters - 0.8;
			Node.DischargeCubicMetersPerSecond = 4.0;
			Node.WidthMeters = WaterTestWidthMeters;
			Node.DepthMeters = 0.8;
			Node.SpeedMetersPerSecond = 1.4;
			Reach.Nodes.Add(Node);
		}

		TArray<FMassifRiverReach> Reaches;
		Reaches.Add(MoveTemp(Reach));
		const TSharedRef<const FMassifRiverNetwork> Network = FMassifRiverNetwork::Create(MoveTemp(Reaches));
		return FMassifRiverField::Build(Network.Get(), WaterTestBankReachMeters);
	}

	FVector2d VertexWorldMeters(const FDynamicMesh3& Mesh, int32 VertexId, int32 TileX, int32 TileY)
	{
		const FVector3d Local = Mesh.GetVertex(VertexId);
		return MassifWaterTileMesh::TileOriginMeters(TileX, TileY)
			+ FVector2d(Local.X, Local.Y) / MetersToCentimeters;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterTileMeshCoverageTest,
	"Massif.Presentation.WaterTileMesh",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWaterTileMeshCoverageTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifRiverField> RiverField = MakeRiverField(96.0);
	const TSharedPtr<FDynamicMesh3> Mesh = MassifWaterTileMesh::BuildWaterTileMesh(*RiverField, 0, 1, MassifAnnualTimeSlice);

	if (!TestTrue(TEXT("A tile the channel crosses is meshed"), Mesh->TriangleCount() > 0))
	{
		return false;
	}

	// The mesh is the wet ground and nothing else. A vertex outside the channel
	// would be water drawn on a hillside; one well inside the bank would be a
	// channel drawn narrower than it is.
	int32 VerticesOutOfTheChannel = 0;
	int32 VerticesOffTheSurface = 0;
	for (const int32 VertexId : Mesh->VertexIndicesItr())
	{
		const FVector2d WorldMeters = VertexWorldMeters(*Mesh, VertexId, 0, 1);
		const FMassifRiverSample River = RiverField->Sample(WorldMeters);
		VerticesOutOfTheChannel += River.SignedDistanceMeters <= 0.001 ? 0 : 1;

		const double AltitudeMeters = Mesh->GetVertex(VertexId).Z / MetersToCentimeters;
		VerticesOffTheSurface +=
			FMath::IsNearlyEqual(AltitudeMeters, River.WaterSurfaceMeters, 0.001) ? 0 : 1;
	}

	TestEqual(TEXT("Every vertex is in the channel"), VerticesOutOfTheChannel, 0);
	TestEqual(TEXT("Every vertex sits on the solved water surface"), VerticesOffTheSurface, 0);

	// The channel runs the whole width of the tile, so the wet area is its width
	// times that. Loose either way: the edge is cut on a lattice, and the surface
	// has the channel's own gradient in it.
	double AreaSquareMeters = 0.0;
	for (const int32 TriangleId : Mesh->TriangleIndicesItr())
	{
		AreaSquareMeters += Mesh->GetTriArea(TriangleId) / (MetersToCentimeters * MetersToCentimeters);
	}

	const double ExpectedSquareMeters = WaterTestWidthMeters * MassifWaterTileMesh::TileSizeMeters;
	TestTrue(TEXT("The mesh covers about the wet ground of the tile"),
		FMath::Abs(AreaSquareMeters - ExpectedSquareMeters) < 0.1 * ExpectedSquareMeters);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterTileMeshSeamTest,
	"Massif.Presentation.WaterTileSeam",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWaterTileMeshSeamTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifRiverField> RiverField = MakeRiverField(96.0);
	const TSharedPtr<FDynamicMesh3> Left = MassifWaterTileMesh::BuildWaterTileMesh(*RiverField, 0, 1, MassifAnnualTimeSlice);
	const TSharedPtr<FDynamicMesh3> Right = MassifWaterTileMesh::BuildWaterTileMesh(*RiverField, 1, 1, MassifAnnualTimeSlice);

	// Two tiles meet on a line of the lattice they share, so whatever either of
	// them puts on that line the other has to put in the same place. Anything else
	// is a crack down the river every sixty-four metres.
	const double BoundaryMeters = MassifWaterTileMesh::TileOriginMeters(1, 1).X;
	TArray<FVector3d> LeftEdge;
	TArray<FVector3d> RightEdge;
	for (const int32 VertexId : Left->VertexIndicesItr())
	{
		const FVector2d WorldMeters = VertexWorldMeters(*Left, VertexId, 0, 1);
		if (FMath::IsNearlyEqual(WorldMeters.X, BoundaryMeters, 1.0e-6))
		{
			LeftEdge.Add(FVector3d(WorldMeters.X, WorldMeters.Y, Left->GetVertex(VertexId).Z));
		}
	}
	for (const int32 VertexId : Right->VertexIndicesItr())
	{
		const FVector2d WorldMeters = VertexWorldMeters(*Right, VertexId, 1, 1);
		if (FMath::IsNearlyEqual(WorldMeters.X, BoundaryMeters, 1.0e-6))
		{
			RightEdge.Add(FVector3d(WorldMeters.X, WorldMeters.Y, Right->GetVertex(VertexId).Z));
		}
	}

	if (!TestTrue(TEXT("Both tiles reach their shared boundary"), LeftEdge.Num() > 1 && RightEdge.Num() > 1))
	{
		return false;
	}
	TestEqual(TEXT("Both tiles put the same number of vertices on it"), LeftEdge.Num(), RightEdge.Num());

	const auto ByNorthing = [](const FVector3d& A, const FVector3d& B) { return A.Y < B.Y; };
	LeftEdge.Sort(ByNorthing);
	RightEdge.Sort(ByNorthing);

	int32 VerticesOutOfPlace = 0;
	for (int32 Index = 0; Index < FMath::Min(LeftEdge.Num(), RightEdge.Num()); ++Index)
	{
		VerticesOutOfPlace += LeftEdge[Index].Equals(RightEdge[Index], 1.0e-6) ? 0 : 1;
	}

	TestEqual(TEXT("Both tiles put them in the same places"), VerticesOutOfPlace, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterTileMeshAttributeTest,
	"Massif.Presentation.WaterTileAttributes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWaterTileMeshAttributeTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifRiverField> RiverField = MakeRiverField(96.0);
	const TSharedPtr<FDynamicMesh3> Mesh = MassifWaterTileMesh::BuildWaterTileMesh(*RiverField, 0, 1, MassifAnnualTimeSlice);

	// The material reads all of these and has no other way to know what channel it
	// is drawing, so a missing one is a silently wrong river.
	if (!TestTrue(TEXT("The tile carries a channel UV and a flow direction"),
		Mesh->Attributes()->NumUVLayers() == 2))
	{
		return false;
	}
	TestTrue(TEXT("The tile carries vertex colours"), Mesh->Attributes()->HasPrimaryColors());

	const UE::Geometry::FDynamicMeshUVOverlay* ChannelUVs = Mesh->Attributes()->PrimaryUV();
	const UE::Geometry::FDynamicMeshUVOverlay* FlowUVs = Mesh->Attributes()->GetUVLayer(1);

	int32 ElementsOffAcross = 0;
	int32 ElementsOffFlow = 0;
	for (const int32 ElementId : ChannelUVs->ElementIndicesItr())
	{
		const float AcrossUv = ChannelUVs->GetElement(ElementId).X;
		ElementsOffAcross += AcrossUv >= -0.001f && AcrossUv <= 1.001f ? 0 : 1;
	}
	for (const int32 ElementId : FlowUVs->ElementIndicesItr())
	{
		const FVector2f Flow = FlowUVs->GetElement(ElementId);
		// The reach runs east, and the material builds the ripples' frame out of this.
		ElementsOffFlow += FMath::IsNearlyEqual(Flow.X, 1.0f, 0.001f) && FMath::IsNearlyZero(Flow.Y, 0.001f) ? 0 : 1;
	}

	TestEqual(TEXT("The channel UV runs 0 to 1 from bank to bank"), ElementsOffAcross, 0);
	TestEqual(TEXT("The flow direction points down the channel"), ElementsOffFlow, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterTileMeshDryGroundTest,
	"Massif.Presentation.WaterTileDryGround",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWaterTileMeshDryGroundTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifRiverField> RiverField = MakeRiverField(96.0);

	// Most of the world has no river in it. A tile that meshes dry ground anyway
	// is both water where there is none and the whole cost of a tile for it.
	const TSharedPtr<FDynamicMesh3> Hillside = MassifWaterTileMesh::BuildWaterTileMesh(*RiverField, 40, 40, MassifAnnualTimeSlice);
	TestEqual(TEXT("A tile with no channel in it is not meshed"), Hillside->TriangleCount(), 0);

	// The channel is six metres wide on a tile sixty-four across, so a tile it
	// crosses is nearly all dry too and only the wet strip should be built.
	const TSharedPtr<FDynamicMesh3> Crossed = MassifWaterTileMesh::BuildWaterTileMesh(*RiverField, 0, 1, MassifAnnualTimeSlice);
	const int32 WholeTileTriangles = 2 * MassifWaterTileMesh::QuadsPerSide * MassifWaterTileMesh::QuadsPerSide;
	TestTrue(TEXT("Only the wet strip of a crossed tile is built"),
		Crossed->TriangleCount() < WholeTileTriangles / 8);
	return true;
}
