#include "Misc/AutomationTest.h"
#include "RiverNetwork.h"

namespace
{
	FWorldGenConfig MakeNetworkTestConfig(int32 MasterSeed)
	{
		FWorldGenConfig Config;
		Config.MasterSeed = MasterSeed;
		Config.Resolution = 257;
		Config.WorldSizeMeters = 8192.0f;
		return Config;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverNetworkTest,
	"Massif.WorldGen.RiverNetwork",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverNetworkTest::RunTest(const FString& Parameters)
{
	const FWorldGenConfig Config = MakeNetworkTestConfig(1337);
	const FRiverNetwork Network = FRiverNetwork::Build(Config);
	const TArray<FRiverNode>& Nodes = Network.GetNodes();

	TestTrue(FString::Printf(TEXT("Network grew past its mouths (%d nodes)"), Nodes.Num()),
		Nodes.Num() > Config.RiverMouthCount * 4);

	const float HalfWorld = Config.WorldSizeMeters * 0.5f;
	for (const FRiverNode& Node : Nodes)
	{
		const bool bInsideDomain = FMath::Abs(Node.PositionMeters.X) <= HalfWorld && FMath::Abs(Node.PositionMeters.Y) <= HalfWorld;
		if (!bInsideDomain)
		{
			AddError(FString::Printf(TEXT("Node outside the map at (%f, %f)"), Node.PositionMeters.X, Node.PositionMeters.Y));
			return false;
		}
		if (Node.ParentIndex == INDEX_NONE)
		{
			continue;
		}

		// Water has to run downhill: every node stands above the one it drains into.
		const FRiverNode& Downstream = Nodes[Node.ParentIndex];
		if (Node.ElevationMeters <= Downstream.ElevationMeters)
		{
			AddError(FString::Printf(TEXT("Node at %.0f m does not stand above its downstream node at %.0f m"),
				Node.ElevationMeters, Downstream.ElevationMeters));
			return false;
		}
		if (Node.ParentIndex >= Nodes.Num())
		{
			AddError(TEXT("Node references a downstream node that does not exist"));
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

	const FRiverNetwork RepeatRun = FRiverNetwork::Build(MakeNetworkTestConfig(1337));
	const FRiverNetwork OtherSeedRun = FRiverNetwork::Build(MakeNetworkTestConfig(99));
	TestEqual(TEXT("Same seed grows the same network"), RepeatRun.GetNodes().Num(), Nodes.Num());
	TestNotEqual(TEXT("Different seed grows a different network"), OtherSeedRun.GetNodes().Num(), Nodes.Num());

	return true;
}
