#include "MassifHabitat.h"
#include "MassifTerrainField.h"
#include "MassifTestTerrain.h"
#include "Misc/AutomationTest.h"
#include "Terrain/MassifSurfaceLayers.h"
#include "Terrain/MassifSurfaceMap.h"

namespace
{
	constexpr uint32 SurfaceTestSeed = 8123;

	/** Points the world is classified at, per side. */
	constexpr int32 SamplesPerSide = 220;

	constexpr double SurfaceSampleSpacingMeters = 8.0;

	/** Slope past which MassifHabitat calls the ground rock whatever else is true of it. */
	constexpr double RockSlopeDegrees = 45.0;

	struct FSurfacePoint
	{
		FVector2d PositionMeters = FVector2d::ZeroVector;
		FMassifHabitatSample Habitat;
		FMassifSurfaceBlend Blend;
	};

	TArray<FSurfacePoint> ClassifyWholeWorld(const FMassifHabitatField& Habitat)
	{
		const double WorldSizeMeters = Habitat.GetTerrainField().GetWorldSizeMeters();
		const double StepMeters = WorldSizeMeters / SamplesPerSide;

		TArray<FSurfacePoint> Points;
		Points.Reserve(SamplesPerSide * SamplesPerSide);
		for (int32 Index = 0; Index < SamplesPerSide * SamplesPerSide; ++Index)
		{
			FSurfacePoint Point;
			Point.PositionMeters = FVector2d(
				(Index % SamplesPerSide + 0.5) * StepMeters - 0.5 * WorldSizeMeters,
				(Index / SamplesPerSide + 0.5) * StepMeters - 0.5 * WorldSizeMeters);
			Point.Habitat = Habitat.Sample(Point.PositionMeters, SurfaceSampleSpacingMeters);
			Point.Blend = MassifSurfaceMap::Classify(Point.Habitat, Point.PositionMeters, SurfaceTestSeed);
			Points.Add(Point);
		}
		return Points;
	}

	/** The one of the pair that covers most of the point, which is what the eye reads there. */
	int32 DominantLayer(const FMassifSurfaceBlend& Blend)
	{
		if (Blend.UpperWeight >= 0.5)
		{
			return Blend.UpperLayer;
		}
		return Blend.LowerLayer;
	}

	/** Names of the layers no point in the world chose, which is what a mistuned niche looks like. */
	FString UnusedLayerNames(const TArray<FSurfacePoint>& Points)
	{
		TBitArray<> Used(false, MassifSurface::LayerCount);
		for (const FSurfacePoint& Point : Points)
		{
			Used[DominantLayer(Point.Blend)] = true;
		}

		TArray<FString> Names;
		for (int32 Index = 0; Index < MassifSurface::LayerCount; ++Index)
		{
			if (!Used[Index])
			{
				Names.Add(MassifSurface::Layers[Index].Name);
			}
		}
		return FString::Join(Names, TEXT(", "));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSurfaceMapTest,
	"Massif.Presentation.SurfaceMap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSurfaceMapTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifHabitatField> Habitat = MassifTestTerrain::MakeHabitat(SurfaceTestSeed);
	const TArray<FSurfacePoint> Points = ClassifyWholeWorld(*Habitat);

	int32 SnowedPointCount = 0;
	for (const FSurfacePoint& Point : Points)
	{
		const FMassifSurfaceBlend& Blend = Point.Blend;
		if (!TestTrue(TEXT("Layer indices are inside the table"),
			Blend.LowerLayer >= 0 && Blend.LowerLayer < MassifSurface::LayerCount
				&& Blend.UpperLayer >= 0 && Blend.UpperLayer < MassifSurface::LayerCount))
		{
			return false;
		}

		// The shader reads the pair as stored and looks the second one up in the
		// green channel, so a pair the wrong way round draws two different layers
		// either side of a boundary that should have had one running through it.
		if (!TestTrue(TEXT("The pair is stored in index order"), Blend.LowerLayer <= Blend.UpperLayer))
		{
			return false;
		}

		if (!TestTrue(TEXT("The blend between the pair is a fraction"),
			Blend.UpperWeight >= 0.0 && Blend.UpperWeight <= 1.0))
		{
			return false;
		}

		// Ground too steep to hold anything wears a layer written for bare rock.
		const bool bRockFace = Point.Habitat.SlopeDegrees >= RockSlopeDegrees;
		const uint32 DominantMask = MassifSurface::Layers[DominantLayer(Blend)].GroundTypeMask;
		if (bRockFace && !TestTrue(TEXT("A rock face wears a layer that covers rock"),
			(DominantMask & MassifGround::Rock) != 0))
		{
			return false;
		}

		SnowedPointCount += static_cast<int32>(Blend.SnowCover > 0.0);
	}

	// A seed that produced no snow at all would pass every check above while
	// meaning the snow term never fires.
	TestTrue(TEXT("Some of the high ground is under snow"), SnowedPointCount > 0);

	// Every set in the table has to win somewhere, or it is a texture that was
	// fetched, imported into the arrays, and never drawn.
	const FString Unused = UnusedLayerNames(Points);
	TestTrue(FString::Printf(TEXT("Every surface layer covers some ground (unused: %s)"), *Unused),
		Unused.IsEmpty());

	// Classification is a pure function: the same point answers the same twice.
	const FSurfacePoint& Probe = Points[Points.Num() / 3];
	const FMassifSurfaceBlend Repeat =
		MassifSurfaceMap::Classify(Probe.Habitat, Probe.PositionMeters, SurfaceTestSeed);
	TestEqual(TEXT("The same point classifies the same twice"),
		Repeat.LowerLayer, Probe.Blend.LowerLayer);
	TestEqual(TEXT("The same point blends the same twice"),
		Repeat.UpperWeight, Probe.Blend.UpperWeight);

	return true;
}
