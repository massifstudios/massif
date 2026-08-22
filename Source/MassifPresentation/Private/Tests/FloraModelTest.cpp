#include "Engine/StaticMesh.h"
#include "Foliage/MassifFoliageSpecies.h"
#include "Misc/AutomationTest.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	/**
	 * The grown trees, and what the LOD ladder each was built with has to hold.
	 * Levels come from the flora simulation rather than from a reduction, and
	 * the last one is an impostor quad on its own material, so a mesh that lost
	 * its ladder — reimported flat, or rebuilt with Nanite on, which draws its
	 * own clusters and ignores authored levels — would draw the near mesh at
	 * every distance and show up only as a frame rate.
	 */
	struct FGrownModel
	{
		const TCHAR* SpeciesName;
		TArrayView<const float> ScreenSizes;
		int32 ImpostorTriangleCount;
	};

	constexpr float SpruceScreenSizes[] = {1.0f, 0.4f, 0.15f, 0.05f};

	const FGrownModel GrownModels[] = {
		{TEXT("PiceaAbies"), SpruceScreenSizes, 2},
	};

	const FMassifFoliageSpecies* FindSpecies(const TCHAR* Name)
	{
		for (const FMassifFoliageSpecies& Species : MassifFoliage::Species)
		{
			if (FCString::Strcmp(Species.Name, Name) == 0)
			{
				return &Species;
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFloraModelTest,
	"Massif.Presentation.FloraModel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FFloraModelTest::RunTest(const FString& Parameters)
{
	for (const FGrownModel& Model : GrownModels)
	{
		const FMassifFoliageSpecies* Species = FindSpecies(Model.SpeciesName);
		if (!TestNotNull(FString::Printf(TEXT("%s is in the foliage table"), Model.SpeciesName), Species))
		{
			continue;
		}

		UStaticMesh* Mesh = Cast<UStaticMesh>(FSoftObjectPath(Species->MeshAssetPath).TryLoad());
		if (!TestNotNull(FString::Printf(TEXT("%s loads from %s"), Model.SpeciesName, Species->MeshAssetPath), Mesh))
		{
			continue;
		}

		TestFalse(FString::Printf(TEXT("%s is not Nanite"), Model.SpeciesName), Mesh->IsNaniteEnabled());
		if (!TestEqual(FString::Printf(TEXT("%s keeps its authored levels"), Model.SpeciesName),
			Mesh->GetNumLODs(), Model.ScreenSizes.Num()))
		{
			continue;
		}

		for (int32 LodIndex = 0; LodIndex < Model.ScreenSizes.Num(); ++LodIndex)
		{
			TestNearlyEqual(FString::Printf(TEXT("%s LOD %d takes over at its authored screen size"),
					Model.SpeciesName, LodIndex),
				Mesh->GetRenderData()->ScreenSize[LodIndex].Default, Model.ScreenSizes[LodIndex], 0.001f);
		}

		for (const FStaticMaterial& Slot : Mesh->GetStaticMaterials())
		{
			TestNotNull(FString::Printf(TEXT("%s material slot %s is filled"),
				Model.SpeciesName, *Slot.MaterialSlotName.ToString()), Slot.MaterialInterface.Get());
		}

		const int32 ImpostorLod = Mesh->GetNumLODs() - 1;
		TestEqual(FString::Printf(TEXT("%s draws its farthest level as an impostor quad"), Model.SpeciesName),
			Mesh->GetNumTriangles(ImpostorLod), Model.ImpostorTriangleCount);
		TestEqual(FString::Printf(TEXT("%s impostor draws on one material"), Model.SpeciesName),
			Mesh->GetNumSections(ImpostorLod), 1);
	}
	return true;
}
