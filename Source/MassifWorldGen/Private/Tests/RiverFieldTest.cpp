#include "MassifRiverField.h"
#include "MassifRiverNetwork.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr double FieldTestBankReachMeters = 10.0;

	/** A straight reach running east along Y = 0, at a constant width and a steady drop. */
	FMassifRiverReach MakeStraightReach(
		double WidthMeters, double NorthingMeters, double StartAltitudeMeters, int32 NodeCount)
	{
		FMassifRiverReach Reach;
		Reach.Nodes.Reserve(NodeCount);
		for (int32 NodeIndex = 0; NodeIndex < NodeCount; ++NodeIndex)
		{
			const double EastingMeters = NodeIndex * 10.0;

			FMassifRiverNode Node;
			Node.PositionMeters = FVector2d(EastingMeters, NorthingMeters);
			Node.GroundAltitudeMeters = StartAltitudeMeters - 0.05 * EastingMeters;
			Node.WaterSurfaceMeters = Node.GroundAltitudeMeters;
			Node.BedAltitudeMeters = Node.WaterSurfaceMeters - 1.0;
			Node.DischargeCubicMetersPerSecond = 5.0;
			Node.WidthMeters = WidthMeters;
			Node.DepthMeters = 1.0;
			Node.SpeedMetersPerSecond = 1.5;
			Reach.Nodes.Add(Node);
		}
		return Reach;
	}

	TSharedRef<const FMassifRiverField> MakeField(TArray<FMassifRiverReach>&& Reaches)
	{
		return FMassifRiverField::Build(
			FMassifRiverNetwork::Create(MoveTemp(Reaches)), FieldTestBankReachMeters);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverFieldChannelTest,
	"Massif.WorldGen.RiverField.Channel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverFieldChannelTest::RunTest(const FString& Parameters)
{
	constexpr double WidthMeters = 8.0;
	TArray<FMassifRiverReach> Reaches;
	Reaches.Add(MakeStraightReach(WidthMeters, 0.0, 1500.0, 12));
	const TSharedRef<const FMassifRiverField> Field = MakeField(MoveTemp(Reaches));

	// Distance is measured from the wetted edge, so it is minus the half width in
	// midstream, zero at the bank, and positive on the ground beyond it.
	const FMassifRiverSample Midstream = Field->Sample(FVector2d(50.0, 0.0));
	TestTrue(TEXT("Midstream is in the channel"), Midstream.IsInChannel());
	TestEqual(TEXT("Midstream is half a width from the bank"),
		Midstream.SignedDistanceMeters, -0.5 * WidthMeters, 1.0e-6);
	TestEqual(TEXT("Midstream carries the channel's width"), Midstream.WidthMeters, WidthMeters, 1.0e-6);

	const FMassifRiverSample Bank = Field->Sample(FVector2d(50.0, 0.5 * WidthMeters));
	TestEqual(TEXT("The bank is where the distance crosses zero"), Bank.SignedDistanceMeters, 0.0, 1.0e-6);

	const FMassifRiverSample Terrace = Field->Sample(FVector2d(50.0, 0.5 * WidthMeters + 4.0));
	TestFalse(TEXT("Ground beyond the bank is not in the channel"), Terrace.IsInChannel());
	TestEqual(TEXT("Ground beyond the bank still measures its distance"),
		Terrace.SignedDistanceMeters, 4.0, 1.0e-6);

	// Past the bank reach the field stops answering, which is what lets a caller
	// test one distance instead of carrying a separate "is there a river here".
	const FMassifRiverSample Hillside =
		Field->Sample(FVector2d(50.0, 0.5 * WidthMeters + FieldTestBankReachMeters + 5.0));
	TestFalse(TEXT("A point out of reach of any channel is not in one"), Hillside.IsInChannel());
	TestTrue(TEXT("A point out of reach of any channel has no distance to one"),
		Hillside.SignedDistanceMeters > 1.0e6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverFieldFrameTest,
	"Massif.WorldGen.RiverField.Frame",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverFieldFrameTest::RunTest(const FString& Parameters)
{
	constexpr double WidthMeters = 8.0;
	TArray<FMassifRiverReach> Reaches;
	Reaches.Add(MakeStraightReach(WidthMeters, 0.0, 1500.0, 12));
	const TSharedRef<const FMassifRiverField> Field = MakeField(MoveTemp(Reaches));

	const FMassifRiverSample Midstream = Field->Sample(FVector2d(50.0, 0.0));
	TestEqual(TEXT("Flow runs the way the reach does"), Midstream.FlowDirection.X, 1.0, 1.0e-6);
	TestEqual(TEXT("Flow has no component across the reach"), Midstream.FlowDirection.Y, 0.0, 1.0e-6);
	TestEqual(TEXT("Distance run is the distance from the head"), Midstream.AlongMeters, 50.0, 1.0e-6);
	TestEqual(TEXT("Midstream is midway across"), Midstream.AcrossFraction, 0.0, 1.0e-6);

	// The two banks have to come out on opposite sides, or a material cannot tell
	// which one it is on and every asymmetry across the channel is lost.
	const FMassifRiverSample RightBank = Field->Sample(FVector2d(50.0, -0.5 * WidthMeters));
	const FMassifRiverSample LeftBank = Field->Sample(FVector2d(50.0, 0.5 * WidthMeters));
	TestEqual(TEXT("One bank is the full half width out"), RightBank.AcrossFraction, 1.0, 1.0e-6);
	TestEqual(TEXT("The other is the same distance the other way"), LeftBank.AcrossFraction, -1.0, 1.0e-6);

	// The surface falls at the rate the reach does, measured over a window rather
	// than over one span.
	TestEqual(TEXT("The channel drops at its own gradient"), Midstream.GradientAlongFlow, 0.05, 1.0e-6);
	TestTrue(TEXT("The bed sits under the water surface"),
		Midstream.BedAltitudeMeters < Midstream.WaterSurfaceMeters);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverFieldConfluenceTest,
	"Massif.WorldGen.RiverField.Confluence",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverFieldConfluenceTest::RunTest(const FString& Parameters)
{
	// A wide trunk with a brook running along inside it, which is what a
	// tributary looks like for the last stretch before it arrives.
	constexpr double TrunkWidthMeters = 40.0;
	constexpr double BrookWidthMeters = 1.0;
	constexpr double DistantBrookNorthingMeters = 200.0;
	TArray<FMassifRiverReach> Reaches;
	Reaches.Add(MakeStraightReach(TrunkWidthMeters, 0.0, 1500.0, 12));
	Reaches.Add(MakeStraightReach(BrookWidthMeters, 12.0, 1520.0, 12));
	Reaches.Add(MakeStraightReach(BrookWidthMeters, DistantBrookNorthingMeters, 1520.0, 12));
	const TSharedRef<const FMassifRiverField> Field = MakeField(MoveTemp(Reaches));

	// Where both can answer, the one whose bank is further away wins. Ranking by
	// distance to the centreline instead would hand this point to the brook and
	// put its surface twenty metres above the trunk it is standing in — which is
	// how a confluence ends up drawn as two sheets in the same place.
	const FMassifRiverSample Overlap = Field->Sample(FVector2d(50.0, 12.0));
	TestEqual(TEXT("The wider channel owns the ground both cover"),
		Overlap.WidthMeters, TrunkWidthMeters, 1.0e-6);
	TestTrue(TEXT("The surface there is the trunk's, not the brook's"),
		Overlap.WaterSurfaceMeters < 1510.0);

	// Well clear of the trunk a brook is the only answer there is, so the rule
	// that picks between channels cannot have swallowed the small ones outright.
	const FMassifRiverSample BrookOnly = Field->Sample(FVector2d(50.0, DistantBrookNorthingMeters));
	TestEqual(TEXT("Away from the trunk a brook answers for itself"),
		BrookOnly.WidthMeters, BrookWidthMeters, 1.0e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverFieldRejectionTest,
	"Massif.WorldGen.RiverField.Rejection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverFieldRejectionTest::RunTest(const FString& Parameters)
{
	TArray<FMassifRiverReach> Reaches;
	Reaches.Add(MakeStraightReach(8.0, 0.0, 1500.0, 12));
	const TSharedRef<const FMassifRiverField> Field = MakeField(MoveTemp(Reaches));

	TestTrue(TEXT("A box on the channel is not rejected"),
		Field->IntersectsBox(FVector2d(40.0, -20.0), FVector2d(60.0, 20.0)));
	TestFalse(TEXT("A box a kilometre away is rejected"),
		Field->IntersectsBox(FVector2d(4000.0, 4000.0), FVector2d(4100.0, 4100.0)));

	// A world with no rivers in it has to answer as much as one that has them, or
	// every caller needs a null check that a river-less seed would be the only
	// thing to exercise.
	const TSharedRef<const FMassifRiverField> Empty = MakeField({});
	TestEqual(TEXT("An empty network indexes no segments"), Empty->GetSegmentCount(), 0);
	TestFalse(TEXT("An empty network rejects every box"),
		Empty->IntersectsBox(FVector2d(-100.0, -100.0), FVector2d(100.0, 100.0)));
	TestFalse(TEXT("An empty network puts no point in a channel"),
		Empty->Sample(FVector2d::ZeroVector).IsInChannel());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverFieldNodeAgreementTest,
	"Massif.WorldGen.RiverField.NodeAgreement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverFieldNodeAgreementTest::RunTest(const FString& Parameters)
{
	TArray<FMassifRiverReach> Reaches;
	Reaches.Add(MakeStraightReach(8.0, 0.0, 1500.0, 12));
	const FMassifRiverReach Reach = Reaches[0];
	const TSharedRef<const FMassifRiverField> Field = MakeField(MoveTemp(Reaches));

	// The field is a second reading of the network, so at a node it has to give
	// back what the network put there. Anything else means the terrain would be
	// cut to one altitude and the water drawn at another.
	int32 NodesDisagreeing = 0;
	for (const FMassifRiverNode& Node : Reach.Nodes)
	{
		const FMassifRiverSample Sample = Field->Sample(Node.PositionMeters);
		NodesDisagreeing +=
			FMath::IsNearlyEqual(Sample.WaterSurfaceMeters, Node.WaterSurfaceMeters, 1.0e-6) ? 0 : 1;
		NodesDisagreeing +=
			FMath::IsNearlyEqual(Sample.BedAltitudeMeters, Node.BedAltitudeMeters, 1.0e-6) ? 0 : 1;
	}

	TestEqual(TEXT("The field reads back what the network wrote at every node"), NodesDisagreeing, 0);
	return true;
}
