#include "MassifUpliftField.h"
#include "Misc/AutomationTest.h"
#include "RiverNetwork.h"
#include "WorldGenConfig.h"

namespace
{
	/**
	 * A world small enough to build in a test, with the node spacing scaled down
	 * with it so the graph still branches several levels deep.
	 */
	FWorldGenConfig MakeNetworkTestConfig(int32 MasterSeed)
	{
		FWorldGenConfig Config;
		Config.MasterSeed = MasterSeed;
		Config.WorldSizeMeters = 24000.0f;
		Config.RiverMouthCount = 6;
		Config.RiverMouthSpacingMeters = 4000.0f;
		Config.TrunkSegmentLengthMeters = 900.0f;
		Config.HeadwaterSegmentLengthMeters = 240.0f;
		Config.RiverMinNodeSpacingMeters = 200.0f;
		Config.RiverMaxNodeCount = 20000;
		return Config;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverNetworkTest,
	"Massif.WorldGen.RiverNetwork",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverNetworkTest::RunTest(const FString& Parameters)
{
	const FWorldGenConfig Config = MakeNetworkTestConfig(1337);
	const FMassifUpliftField Uplift(Config, 1337);
	const FRiverNetwork Network = FRiverNetwork::Build(Config, Uplift);
	const TArray<FRiverNode>& Nodes = Network.GetNodes();

	TestTrue(FString::Printf(TEXT("Network grew past its mouths (%d nodes)"), Nodes.Num()),
		Nodes.Num() > Config.RiverMouthCount * 4);

	const double HalfWorld = Config.WorldSizeMeters * 0.5;
	for (const FRiverNode& Node : Nodes)
	{
		const bool bInsideWorld = FMath::Abs(Node.PositionMeters.X) <= HalfWorld && FMath::Abs(Node.PositionMeters.Y) <= HalfWorld;
		if (!bInsideWorld)
		{
			AddError(FString::Printf(TEXT("Node outside the world at (%f, %f)"), Node.PositionMeters.X, Node.PositionMeters.Y));
			return false;
		}
		if (Node.ParentIndex == INDEX_NONE)
		{
			continue;
		}
		if (Node.ParentIndex >= Nodes.Num())
		{
			AddError(TEXT("Node references a downstream node that does not exist"));
			return false;
		}

		// Water has to run downhill: every node stands above the one it drains into.
		const FRiverNode& Downstream = Nodes[Node.ParentIndex];
		if (Node.ElevationMeters <= Downstream.ElevationMeters)
		{
			AddError(FString::Printf(TEXT("Node at %.0f m does not stand above its downstream node at %.0f m"),
				Node.ElevationMeters, Downstream.ElevationMeters));
			return false;
		}

		// No watercourse may climb over the range that contains it.
		if (Node.ElevationMeters > Uplift.MaxWaterAltitudeMeters(Node.PositionMeters) + 1.0)
		{
			AddError(FString::Printf(TEXT("Node at %.0f m stands above the ground the uplift field allows (%.0f m)"),
				Node.ElevationMeters, Uplift.MaxWaterAltitudeMeters(Node.PositionMeters)));
			return false;
		}
	}

	// Discharge accumulates downstream, so a trunk drains more than its tributaries.
	int32 HeadwaterCount = 0;
	for (const FRiverNode& Node : Nodes)
	{
		HeadwaterCount += Node.UpstreamNodeCount == 0;
	}
	TestTrue(TEXT("Network has headwaters"), HeadwaterCount > 0);
	TestTrue(TEXT("Mouths drain more than headwaters"), Nodes[0].UpstreamNodeCount > 0);

	// Small streams cut notches into the mountainside instead of claiming the
	// relief: somewhere the ground at a small stream stands well above the
	// water the growth assigned it. A trunk's floor stays at its own water.
	const double TrunkDischarge = FMath::Loge(1.0 + static_cast<double>(Config.RiverMaxNodeCount));
	int32 BuriedSmallStreamCount = 0;
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); NodeIndex += 25)
	{
		const FRiverNode& Node = Nodes[NodeIndex];
		const double Fraction = FMath::Loge(1.0 + static_cast<double>(Node.UpstreamNodeCount)) / TrunkDischarge;
		const double GroundMeters = Network.Sample(Node.PositionMeters, Config, Uplift).HeightMeters;
		if (Fraction < Config.BackboneDischargeFraction)
		{
			BuriedSmallStreamCount += GroundMeters > Node.ElevationMeters + Config.StreamNotchDepthMeters;
			continue;
		}
		if (GroundMeters > Node.ElevationMeters + 1.0)
		{
			AddError(FString::Printf(TEXT("Trunk floor at (%.0f, %.0f) stands %.1f m above its own water"),
				Node.PositionMeters.X, Node.PositionMeters.Y, GroundMeters - Node.ElevationMeters));
			return false;
		}
	}
	TestTrue(TEXT("Small streams sit in notches shallower than the relief above them"), BuriedSmallStreamCount > 0);

	const FRiverNetwork RepeatRun = FRiverNetwork::Build(MakeNetworkTestConfig(1337), Uplift);
	const FMassifUpliftField OtherUplift(Config, 99);
	const FRiverNetwork OtherSeedRun = FRiverNetwork::Build(MakeNetworkTestConfig(99), OtherUplift);
	TestEqual(TEXT("Same seed grows the same network"), RepeatRun.GetNodes().Num(), Nodes.Num());
	TestNotEqual(TEXT("Different seed grows a different network"), OtherSeedRun.GetNodes().Num(), Nodes.Num());

	return true;
}
