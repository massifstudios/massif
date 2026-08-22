#include "MassifEnvironmentBakeParams.h"
#include "MassifTestEnvironment.h"
#include "MassifHabitat.h"
#include "MassifHeightmap.h"
#include "MassifRiverField.h"
#include "MassifRiverNetwork.h"
#include "MassifTerrainField.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr uint32 ExclusionTestSeed = 4711;
	constexpr int32 ExclusionTestSide = 96;
	constexpr double ExclusionTestSpacingMeters = 4.0;
	constexpr double ExclusionSampleSpacingMeters = 1.0;
	constexpr double ExclusionChannelWidthMeters = 8.0;

	/** Valley-floor altitude: warm enough that the ground is not classified as snow. */
	constexpr double ExclusionTestAltitudeMeters = 1400.0;

	/** A channel running east down the middle of the world, at the altitude of the ground. */
	FMassifRiverReach MakeChannelReach()
	{
		FMassifRiverReach Reach;
		for (int32 NodeIndex = 0; NodeIndex < 40; ++NodeIndex)
		{
			FMassifRiverNode Node;
			Node.PositionMeters = FVector2d(-160.0 + NodeIndex * 8.0, 0.0);
			Node.GroundAltitudeMeters = ExclusionTestAltitudeMeters;
			Node.WaterSurfaceMeters = ExclusionTestAltitudeMeters;
			Node.BedAltitudeMeters = ExclusionTestAltitudeMeters - 1.0;
			Node.DischargeCubicMetersPerSecond = 6.0;
			Node.WidthMeters = ExclusionChannelWidthMeters;
			Node.DepthMeters = 1.0;
			Node.SpeedMetersPerSecond = 1.5;
			Reach.Nodes.Add(Node);
		}
		return Reach;
	}

	struct FExclusionTestWorld
	{
		TSharedPtr<const FMassifRiverField> RiverField;
		TSharedPtr<const FMassifHabitatField> Habitat;
	};

	FExclusionTestWorld MakeExclusionTestWorld()
	{
		TArray<double> Altitudes;
		Altitudes.Init(ExclusionTestAltitudeMeters, ExclusionTestSide * ExclusionTestSide);
		const TSharedRef<const FMassifHeightmap> Heightmap = FMassifHeightmap::FromAltitudes(
			Altitudes, ExclusionTestSide, ExclusionTestSide, ExclusionTestSpacingMeters);

		TArray<FMassifRiverReach> Reaches;
		Reaches.Add(MakeChannelReach());
		const TSharedRef<const FMassifRiverNetwork> Network = FMassifRiverNetwork::Create(MoveTemp(Reaches));

		const FMassifRiverCarveParams CarveParams;
		FExclusionTestWorld World;
		World.RiverField = FMassifRiverField::Build(Network.Get(), CarveParams.MaxBankMeters);

		const TSharedRef<const FMassifTerrainField> Field = MakeShared<const FMassifTerrainField>(
			Heightmap, World.RiverField.ToSharedRef(), CarveParams);

		FMassifEnvironmentBakeParams BakeParams;
		BakeParams.CellSizeMeters = ExclusionTestSpacingMeters;
		World.Habitat = MakeShared<const FMassifHabitatField>(
			Field,
			MassifSolveTestEnvironmentState(Field->GetHeightmapShared(), ExclusionTestSeed, BakeParams),
			ExclusionTestSeed);
		return World;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverExclusionGroundTest,
	"Massif.WorldGen.RiverExclusion.Ground",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverExclusionGroundTest::RunTest(const FString& Parameters)
{
	const FExclusionTestWorld World = MakeExclusionTestWorld();

	// Everything that decides what grows reads the habitat, so the habitat is
	// where a river has to become something other than damp hillside. The
	// standing water the bake solves cannot do it: a depression fill only finds
	// water that cannot leave, and a river is water that is leaving.
	int32 SamplesNotInWater = 0;
	for (int32 Step = 0; Step <= 20; ++Step)
	{
		const FVector2d PositionMeters(-100.0 + Step * 10.0, 0.0);
		const FMassifHabitatSample Ground = World.Habitat->Sample(PositionMeters, ExclusionSampleSpacingMeters);
		SamplesNotInWater += Ground.GroundType == EMassifGroundType::Water ? 0 : 1;
	}

	TestEqual(TEXT("Midstream is classified as water"), SamplesNotInWater, 0);

	// The bank beside it is wetland, which is the ground the alder and the willow
	// in the species table ask for.
	const FMassifHabitatSample Bank =
		World.Habitat->Sample(FVector2d(0.0, 0.5 * ExclusionChannelWidthMeters + 1.5), ExclusionSampleSpacingMeters);
	TestEqual(TEXT("The bank beside the channel is wetland"), Bank.GroundType, EMassifGroundType::Wetland);
	TestTrue(TEXT("The bank beside the channel is wet"), Bank.Humidity > 0.8);

	// Well away from it nothing has changed: the channel must not damp the whole
	// valley, or a river would put a bog on both sides of itself for a hundred metres.
	const FMassifHabitatSample Hillside =
		World.Habitat->Sample(FVector2d(0.0, 120.0), ExclusionSampleSpacingMeters);
	TestNotEqual(TEXT("Ground away from the channel is not water"),
		Hillside.GroundType, EMassifGroundType::Water);
	TestTrue(TEXT("Ground away from the channel keeps its distance"),
		Hillside.ChannelDistanceMeters > 20.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverExclusionFoliageTest,
	"Massif.WorldGen.RiverExclusion.Foliage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverExclusionFoliageTest::RunTest(const FString& Parameters)
{
	const FExclusionTestWorld World = MakeExclusionTestWorld();

	// The scatter samples habitat at each candidate's own position, so the ground
	// type it reads there is exactly the one classified here. Nothing in the
	// species table accepts Water, which is what makes the riverbed empty — one
	// gate rather than a second exclusion rule kept in step with the first.
	int32 SamplesAcceptingAnything = 0;
	for (int32 Step = 0; Step <= 40; ++Step)
	{
		const double AcrossMeters = -0.5 * ExclusionChannelWidthMeters
			+ Step * ExclusionChannelWidthMeters / 40.0;
		const FMassifHabitatSample Ground =
			World.Habitat->Sample(FVector2d(20.0, AcrossMeters), ExclusionSampleSpacingMeters);
		const uint32 GroundBit = MassifGroundTypeBit(Ground.GroundType);
		SamplesAcceptingAnything += (GroundBit & ~MassifGround::Water) != 0 ? 1 : 0;
	}

	TestEqual(TEXT("Every point across the channel is water and nothing else"), SamplesAcceptingAnything, 0);
	return true;
}
