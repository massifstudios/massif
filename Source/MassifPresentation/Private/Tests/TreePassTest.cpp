#include "Foliage/MassifFoliageSpecies.h"
#include "Foliage/MassifTreePass.h"
#include "MassifHabitat.h"
#include "MassifObjectStore.h"
#include "MassifObjectStoreBuilder.h"
#include "MassifObjectTypeRegistry.h"
#include "MassifTerrainField.h"
#include "MassifTestTerrain.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr uint32 TreeTestSeed = 20260824;

	/** Scale ranges are float in the table, so the band is compared to a float's worth of places. */
	constexpr double SizeBandTolerance = 1.0e-5;

	FMassifObjectStore MakeStore(const FMassifHabitatField& Habitat)
	{
		const double WorldSizeMeters = Habitat.GetTerrainField().GetWorldSizeMeters();
		return FMassifObjectStore(
			FMassifObjectChunkGrid::FromWorldSize(FVector2d(WorldSizeMeters, WorldSizeMeters)));
	}

	FMassifObjectStore PlantWorld(const FMassifHabitatField& Habitat, uint32 MasterSeed)
	{
		FMassifObjectStore Store = MakeStore(Habitat);
		MassifTreePass::PlantWorld(Habitat, MasterSeed, FMassifObjectTypeRegistry::Get(), Store);
		Store.Finalize();
		return Store;
	}

	TArray<const FMassifFoliageSpecies*> StoredSpecies()
	{
		TArray<const FMassifFoliageSpecies*> Stored;
		for (const FMassifFoliageSpecies& Species : MassifFoliage::Species)
		{
			if (Species.ObjectTypeId != MassifInvalidObjectTypeId)
			{
				Stored.Add(&Species);
			}
		}
		return Stored;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTreeTypeRegistryAgreesTest,
	"Massif.Presentation.TreePass.RegistryAgreesWithTable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FTreeTypeRegistryAgreesTest::RunTest(const FString& Parameters)
{
	// The size a record keeps is quantised over the registry's band, and the sizes
	// the pass produces come from the table's scale range. Nothing forces those two
	// to agree, and a band that is too narrow clamps every large tree to the same
	// size rather than failing — which reads as a forest that has stopped growing.
	const FMassifObjectTypeRegistry& Registry = FMassifObjectTypeRegistry::Get();
	const TArray<const FMassifFoliageSpecies*> Stored = StoredSpecies();

	TestEqual(TEXT("Every tree in the table is stored"), Stored.Num(), 10);

	for (const FMassifFoliageSpecies* Species : Stored)
	{
		const FMassifObjectTypeInfo* TypeInfo = Registry.FindTypeInfo(Species->ObjectTypeId);
		if (!TestNotNull(FString::Printf(TEXT("%s has a registered type"), Species->Name), TypeInfo))
		{
			continue;
		}

		TestTrue(FString::Printf(TEXT("%s is registered as a tree"), Species->Name),
			TypeInfo->Category == EMassifObjectCategory::Tree);
		TestTrue(FString::Printf(TEXT("%s has a crown radius"), Species->Name),
			Species->CrownRadiusMeters > 0.0);
		TestNearlyEqual(FString::Printf(TEXT("%s smallest crown matches its band"), Species->Name),
			TypeInfo->SizeBandMeters.X, Species->MinScale * Species->CrownRadiusMeters, SizeBandTolerance);
		TestNearlyEqual(FString::Printf(TEXT("%s largest crown matches its band"), Species->Name),
			TypeInfo->SizeBandMeters.Y, Species->MaxScale * Species->CrownRadiusMeters, SizeBandTolerance);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTreePassPlantsTest,
	"Massif.Presentation.TreePass.PlantsStoredTrees",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FTreePassPlantsTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifHabitatField> Habitat = MassifTestTerrain::MakeHabitat(TreeTestSeed);
	const FMassifObjectStore Store = PlantWorld(Habitat.Get(), TreeTestSeed);

	TestTrue(TEXT("The test massif grew a forest"), Store.GetRecordCount() > 1000);

	const FMassifObjectTypeRegistry& Registry = FMassifObjectTypeRegistry::Get();
	const double HalfWorldMeters = 0.5 * Habitat->GetTerrainField().GetWorldSizeMeters();

	int32 OutsideCount = 0;
	int32 WrongCategoryCount = 0;
	const FMassifObjectChunkGrid& Grid = Store.GetChunkGrid();
	for (int32 Row = 0; Row < Grid.Height; ++Row)
	{
		for (int32 Column = 0; Column < Grid.Width; ++Column)
		{
			for (const FMassifObjectRecord& Record : Store.GetChunkRecords(Column, Row))
			{
				FMassifObjectPlacement Tree;
				const bool bDecoded = MassifObjectRecordCodec::Decode(Record, Registry, Tree);
				const FMassifObjectTypeInfo* TypeInfo = Registry.FindTypeInfo(Record.TypeId);
				WrongCategoryCount += static_cast<int32>(
					!bDecoded || TypeInfo == nullptr || TypeInfo->Category != EMassifObjectCategory::Tree);

				// A tile's square reaches past the border, so this is the assertion that
				// the pass drops what falls outside rather than clamping it to the edge.
				OutsideCount += static_cast<int32>(
					FMath::Abs(Tree.PositionMeters.X) > HalfWorldMeters
					|| FMath::Abs(Tree.PositionMeters.Y) > HalfWorldMeters);
			}
		}
	}

	TestEqual(TEXT("Nothing but trees was planted"), WrongCategoryCount, 0);
	TestEqual(TEXT("Nothing was planted outside the world"), OutsideCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTreePassDeterminismTest,
	"Massif.Presentation.TreePass.Determinism",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FTreePassDeterminismTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifHabitatField> Habitat = MassifTestTerrain::MakeHabitat(TreeTestSeed);

	// The pass scatters its tiles in parallel, so this is the assertion that the
	// forest does not depend on the order they happened to finish in.
	const FMassifObjectStore First = PlantWorld(Habitat.Get(), TreeTestSeed);
	const FMassifObjectStore Second = PlantWorld(Habitat.Get(), TreeTestSeed);

	TestEqual(TEXT("The same seed plants the same count"),
		First.GetRecordCount(), Second.GetRecordCount());
	TestEqual(TEXT("The same seed plants the same forest"),
		First.ComputeContentHash(), Second.ComputeContentHash());

	const FMassifObjectStore Other = PlantWorld(Habitat.Get(), TreeTestSeed + 1);
	TestNotEqual(TEXT("A different seed plants a different forest"),
		First.ComputeContentHash(), Other.ComputeContentHash());
	return true;
}
