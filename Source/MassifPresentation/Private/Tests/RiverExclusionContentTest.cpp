#include "Foliage/MassifFoliageScatter.h"
#include "Foliage/MassifFoliageSpecies.h"
#include "MassifHabitat.h"
#include "Misc/AutomationTest.h"
#include "Terrain/MassifSurfaceLayers.h"
#include "Terrain/MassifSurfaceMap.h"

namespace
{
	/** A wetted channel bed, as MassifHabitat classifies one. */
	FMassifHabitatSample MakeChannelBedSample()
	{
		FMassifHabitatSample Ground;
		Ground.AltitudeMeters = 1400.0;
		// Steep, because a narrow channel's bed is: the cross-section cut for a
		// brook a metre wide has banks at forty degrees.
		Ground.SlopeDegrees = 38.0;
		Ground.TemperatureCelsius = 9.9;
		Ground.Humidity = 1.0;
		Ground.SoilDepthMeters = 0.2;
		Ground.ChannelDistanceMeters = -1.0;
		Ground.GroundType = EMassifGroundType::Water;
		return Ground;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverExclusionSpeciesTest,
	"Massif.Presentation.RiverExclusionSpecies",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverExclusionSpeciesTest::RunTest(const FString& Parameters)
{
	const FMassifHabitatSample ChannelBed = MakeChannelBedSample();

	// Nothing grows in a river. This is the whole exclusion: a species is placed
	// by its ground type mask, so a table where no mask carries Water is a table
	// that cannot put a plant in a channel — there is no second rule to keep in
	// step with this one, and no order of tests that could get it wrong.
	int32 SpeciesGrowingInRivers = 0;
	for (const FMassifFoliageSpecies& Species : MassifFoliage::Species)
	{
		SpeciesGrowingInRivers += (Species.GroundTypeMask & MassifGround::Water) != 0 ? 1 : 0;
		SpeciesGrowingInRivers += MassifFoliageScatter::HabitatWeight(Species, ChannelBed) > 0.0 ? 1 : 0;
	}

	TestEqual(TEXT("No species takes root in a river"), SpeciesGrowingInRivers, 0);

	// The bank is a different matter: it is what the riparian species are for, and
	// a table with nothing on it would leave every channel in the world running
	// through bare ground.
	int32 SpeciesOnTheBank = 0;
	for (const FMassifFoliageSpecies& Species : MassifFoliage::Species)
	{
		SpeciesOnTheBank += (Species.GroundTypeMask & MassifGround::Wetland) != 0 ? 1 : 0;
	}

	TestTrue(TEXT("Something grows on the bank"), SpeciesOnTheBank > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverExclusionSurfaceTest,
	"Massif.Presentation.RiverExclusionSurface",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverExclusionSurfaceTest::RunTest(const FString& Parameters)
{
	// The bed still has to wear something. Layers are chosen by ground type, so a
	// type no layer accepts falls through the ranking and the terrain is left
	// holding whichever layer happens to sit first in the table — a rock face,
	// as it happens, drawn along the bottom of every river.
	int32 LayersCoveringTheBed = 0;
	for (const FMassifSurfaceLayer& Layer : MassifSurface::Layers)
	{
		LayersCoveringTheBed += (Layer.GroundTypeMask & MassifGround::Water) != 0 ? 1 : 0;
	}

	if (!TestTrue(TEXT("Some layer covers the bed of a channel"), LayersCoveringTheBed > 0))
	{
		return false;
	}

	// The pair is stored in index order and the share says which of them is
	// actually shown, so it is the shown one that has to be a bed.
	const FMassifSurfaceBlend Blend = MassifSurfaceMap::Classify(MakeChannelBedSample(), FVector2d(120.0, 40.0), 4711);
	int32 ShownLayer = Blend.LowerLayer;
	if (Blend.UpperWeight >= 0.5)
	{
		ShownLayer = Blend.UpperLayer;
	}

	TestTrue(TEXT("A channel bed is drawn as a layer that covers one"),
		(MassifSurface::Layers[ShownLayer].GroundTypeMask & MassifGround::Water) != 0);
	TestTrue(TEXT("A channel bed carries no permanent snow"), Blend.SnowCover < 0.5);
	return true;
}
