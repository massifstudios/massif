#include "MassifSeed.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifSeedChannelTest,
	"Massif.Core.Seed.Channels",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifSeedChannelTest::RunTest(const FString& Parameters)
{
	const FMassifSeed Seed(1337u);

	TestEqual(TEXT("Same channel derives the same seed"),
		Seed.MakeChannelSeed(TEXT("WorldGen.Ridge")), Seed.MakeChannelSeed(TEXT("WorldGen.Ridge")));
	TestNotEqual(TEXT("Different channels derive different seeds"),
		Seed.MakeChannelSeed(TEXT("WorldGen.Ridge")), Seed.MakeChannelSeed(TEXT("WorldGen.Uplift")));

	const FMassifSeed OtherSeed(1338u);
	TestNotEqual(TEXT("Different master seeds derive different channel seeds"),
		Seed.MakeChannelSeed(TEXT("WorldGen.Ridge")), OtherSeed.MakeChannelSeed(TEXT("WorldGen.Ridge")));

	FRandomStream StreamA = Seed.MakeChannelStream(TEXT("Sim.Guests"));
	FRandomStream StreamB = Seed.MakeChannelStream(TEXT("Sim.Guests"));
	TestEqual(TEXT("Channel streams replay identically"), StreamA.RandRange(0, 1000000), StreamB.RandRange(0, 1000000));

	return true;
}
