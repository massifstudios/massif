#include "MassifElevationSource.h"

#include "MassifHeightmap.h"
#include "Misc/AutomationTest.h"

namespace MassifElevationSourceTest
{
	constexpr double AnalyticWorldSizeMeters = 2000.0;
	constexpr double AnalyticHalfSizeMeters = 1000.0;
	constexpr double AnalyticBaseAltitudeMeters = 1000.0;
	constexpr double AnalyticReliefMeters = 500.0;

	/** The shapes are closed forms in double precision, so only rounding is allowed to differ. */
	constexpr double AnalyticToleranceMeters = 1.0e-9;

	FMassifAnalyticElevationSource::FParams MakeAnalyticParams(EMassifAnalyticTerrain Shape, double BearingDegrees)
	{
		FMassifAnalyticElevationSource::FParams Params;
		Params.Shape = Shape;
		Params.WorldSizeMeters = AnalyticWorldSizeMeters;
		Params.BaseAltitudeMeters = AnalyticBaseAltitudeMeters;
		Params.ReliefMeters = AnalyticReliefMeters;
		Params.BearingDegrees = BearingDegrees;
		Params.FinestSpacingMeters = 1.0;
		return Params;
	}

	struct FAnalyticShapeExpectation
	{
		EMassifAnalyticTerrain Shape;
		FVector2d PositionMeters;
		double AltitudeMeters;
		const TCHAR* Description;
	};

	/** Every shape at the world centre, at a border and at a point in between, worked out from the closed form by hand. */
	const FAnalyticShapeExpectation AnalyticShapeExpectations[] =
	{
		{ EMassifAnalyticTerrain::Flat, FVector2d(0.0, 0.0), 1000.0, TEXT("Flat sits at the base altitude at the centre") },
		{ EMassifAnalyticTerrain::Flat, FVector2d(1000.0, -1000.0), 1000.0, TEXT("Flat sits at the base altitude at the corner") },
		{ EMassifAnalyticTerrain::Flat, FVector2d(300.0, -420.0), 1000.0, TEXT("Flat sits at the base altitude in between") },

		{ EMassifAnalyticTerrain::TiltedPlane, FVector2d(0.0, 0.0), 1250.0, TEXT("The plane is half its relief up at the centre") },
		{ EMassifAnalyticTerrain::TiltedPlane, FVector2d(0.0, 1000.0), 1000.0, TEXT("The plane bottoms out on the edge the bearing points to") },
		{ EMassifAnalyticTerrain::TiltedPlane, FVector2d(0.0, -1000.0), 1500.0, TEXT("The plane tops out on the edge the bearing points away from") },
		{ EMassifAnalyticTerrain::TiltedPlane, FVector2d(250.0, 500.0), 1125.0, TEXT("The plane ignores the across-bearing axis") },

		{ EMassifAnalyticTerrain::Cone, FVector2d(0.0, 0.0), 1500.0, TEXT("The cone peaks at the world centre") },
		{ EMassifAnalyticTerrain::Cone, FVector2d(1000.0, 0.0), 1000.0, TEXT("The cone reaches the base altitude at the border") },
		{ EMassifAnalyticTerrain::Cone, FVector2d(300.0, 400.0), 1250.0, TEXT("The cone falls linearly with radius") },

		{ EMassifAnalyticTerrain::Ridge, FVector2d(0.0, 0.0), 1500.0, TEXT("The ridge crest passes through the world centre") },
		{ EMassifAnalyticTerrain::Ridge, FVector2d(0.0, 900.0), 1500.0, TEXT("The ridge crest runs along the bearing") },
		{ EMassifAnalyticTerrain::Ridge, FVector2d(1000.0, 0.0), 1000.0, TEXT("The ridge reaches the base altitude at the border") },
		{ EMassifAnalyticTerrain::Ridge, FVector2d(250.0, -600.0), 1375.0, TEXT("The ridge falls linearly across its crest") },

		{ EMassifAnalyticTerrain::Pit, FVector2d(0.0, 0.0), 500.0, TEXT("The pit bottoms out at the world centre") },
		{ EMassifAnalyticTerrain::Pit, FVector2d(1000.0, 0.0), 1000.0, TEXT("The pit reaches the base altitude at the border") },
		{ EMassifAnalyticTerrain::Pit, FVector2d(300.0, 400.0), 750.0, TEXT("The pit rises linearly with radius") },

		{ EMassifAnalyticTerrain::Saddle, FVector2d(0.0, 0.0), 1250.0, TEXT("The saddle's pass sits halfway up at the world centre") },
		{ EMassifAnalyticTerrain::Saddle, FVector2d(0.0, 1000.0), 1500.0, TEXT("One saddle peak sits along the bearing") },
		{ EMassifAnalyticTerrain::Saddle, FVector2d(0.0, -1000.0), 1500.0, TEXT("The other saddle peak sits opposite it") },
		{ EMassifAnalyticTerrain::Saddle, FVector2d(1000.0, 0.0), 1000.0, TEXT("The saddle falls away across the bearing") },
		{ EMassifAnalyticTerrain::Saddle, FVector2d(0.0, 500.0), 1375.0, TEXT("The saddle climbs smoothly from the pass to a peak") },
	};

	const EMassifAnalyticTerrain AllAnalyticShapes[] =
	{
		EMassifAnalyticTerrain::Flat,
		EMassifAnalyticTerrain::TiltedPlane,
		EMassifAnalyticTerrain::Cone,
		EMassifAnalyticTerrain::Ridge,
		EMassifAnalyticTerrain::Pit,
		EMassifAnalyticTerrain::Saddle,
	};

	/** Largest gap between samplings of one point at spacings five orders of magnitude apart. */
	double WorstSpacingDifferenceMeters(const IMassifElevationSource& Source, FVector2d PositionMeters)
	{
		const double SampleSpacingsMeters[] = { 0.05, 1.0, 8.0, 64.0, 4000.0 };
		const double ReferenceMeters = Source.SampleAltitudeMeters(PositionMeters, SampleSpacingsMeters[0]);

		double WorstMeters = 0.0;
		for (const double SampleSpacingMeters : SampleSpacingsMeters)
		{
			const double AltitudeMeters = Source.SampleAltitudeMeters(PositionMeters, SampleSpacingMeters);
			WorstMeters = FMath::Max(WorstMeters, FMath::Abs(AltitudeMeters - ReferenceMeters));
		}
		return WorstMeters;
	}

	/** Altitudes out from the world centre along a direction aligned to neither axis. */
	TArray<double> SampleAnalyticRadialMeters(const IMassifElevationSource& Source)
	{
		const FVector2d Direction(0.6, 0.8);
		TArray<double> AltitudesMeters;
		for (int32 Step = 0; Step <= 30; ++Step)
		{
			AltitudesMeters.Add(Source.SampleAltitudeMeters(Direction * (Step * 30.0), 1.0));
		}
		return AltitudesMeters;
	}

	bool IsStrictlyDescending(const TArray<double>& Values)
	{
		for (int32 Index = 1; Index < Values.Num(); ++Index)
		{
			if (Values[Index] >= Values[Index - 1])
			{
				return false;
			}
		}
		return true;
	}

	bool IsStrictlyAscending(const TArray<double>& Values)
	{
		for (int32 Index = 1; Index < Values.Num(); ++Index)
		{
			if (Values[Index] <= Values[Index - 1])
			{
				return false;
			}
		}
		return true;
	}

	/** Turning the bearing a quarter turn east must come out the same as swapping the two axes. */
	double WorstQuarterTurnDifferenceMeters(EMassifAnalyticTerrain Shape)
	{
		const FMassifAnalyticElevationSource FacingNorth(MakeAnalyticParams(Shape, 0.0));
		const FMassifAnalyticElevationSource FacingEast(MakeAnalyticParams(Shape, 90.0));

		double WorstMeters = 0.0;
		for (int32 Step = 0; Step <= 40; ++Step)
		{
			const FVector2d PositionMeters(-900.0 + Step * 45.0, 700.0 - Step * 31.0);
			const double Rotated = FacingEast.SampleAltitudeMeters(PositionMeters, 1.0);
			const double Swapped = FacingNorth.SampleAltitudeMeters(FVector2d(PositionMeters.Y, PositionMeters.X), 1.0);
			WorstMeters = FMath::Max(WorstMeters, FMath::Abs(Rotated - Swapped));
		}
		return WorstMeters;
	}

	constexpr int32 AdapterHeightmapSide = 64;
	constexpr double AdapterHeightmapSpacingMeters = 4.0;

	/** A surface with curvature in both axes, so a swapped or shifted coordinate cannot hide in it. */
	TSharedRef<const FMassifHeightmap> MakeAdapterHeightmap()
	{
		TArray<double> AltitudesMeters;
		AltitudesMeters.Reserve(AdapterHeightmapSide * AdapterHeightmapSide);
		for (int32 Index = 0; Index < AdapterHeightmapSide * AdapterHeightmapSide; ++Index)
		{
			const double EastMeters = (Index % AdapterHeightmapSide) * AdapterHeightmapSpacingMeters;
			const double NorthMeters = (Index / AdapterHeightmapSide) * AdapterHeightmapSpacingMeters;
			AltitudesMeters.Add(1200.0 + 180.0 * FMath::Sin(EastMeters / 61.0) + 90.0 * FMath::Cos(NorthMeters / 43.0));
		}
		return FMassifHeightmap::FromAltitudes(
			AltitudesMeters, AdapterHeightmapSide, AdapterHeightmapSide, AdapterHeightmapSpacingMeters);
	}

	double WorstAdapterDifferenceMeters(
		const IMassifElevationSource& Source, const FMassifHeightmap& Heightmap, double SampleSpacingMeters)
	{
		double WorstMeters = 0.0;
		for (int32 Index = 0; Index < 17 * 17; ++Index)
		{
			const FVector2d PositionMeters(-128.0 + (Index % 17) * 15.0, 128.0 - (Index / 17) * 15.0);
			const double Adapted = Source.SampleAltitudeMeters(PositionMeters, SampleSpacingMeters);
			const double Direct = Heightmap.SampleHeightMeters(PositionMeters, SampleSpacingMeters);
			WorstMeters = FMath::Max(WorstMeters, FMath::Abs(Adapted - Direct));
		}
		return WorstMeters;
	}
}

using namespace MassifElevationSourceTest;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FElevationSourceAnalyticShapesTest,
	"Massif.WorldGen.ElevationSource.AnalyticShapes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FElevationSourceAnalyticShapesTest::RunTest(const FString& Parameters)
{
	// These are the altitudes every environment process is written against, so
	// they are spelled out here rather than recomputed from the same expression.
	for (const FAnalyticShapeExpectation& Expectation : AnalyticShapeExpectations)
	{
		const FMassifAnalyticElevationSource Source(MakeAnalyticParams(Expectation.Shape, 0.0));
		const double AltitudeMeters = Source.SampleAltitudeMeters(Expectation.PositionMeters, 1.0);
		TestEqual(Expectation.Description, AltitudeMeters, Expectation.AltitudeMeters, AnalyticToleranceMeters);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FElevationSourceSpacingIndependenceTest,
	"Massif.WorldGen.ElevationSource.SpacingIndependence",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FElevationSourceSpacingIndependenceTest::RunTest(const FString& Parameters)
{
	// An analytic shape resolves everything at every scale: there is no detail
	// for a coarse sampling to filter away, so the spacing must not change it.
	for (const EMassifAnalyticTerrain Shape : AllAnalyticShapes)
	{
		const FMassifAnalyticElevationSource Source(MakeAnalyticParams(Shape, 33.0));
		const double WorstMeters = FMath::Max(
			WorstSpacingDifferenceMeters(Source, FVector2d(313.0, -207.0)),
			WorstSpacingDifferenceMeters(Source, FVector2d(-861.0, 744.0)));
		TestEqual(FString::Printf(TEXT("Shape %d samples the same at every spacing"), static_cast<int32>(Shape)),
			WorstMeters, 0.0, 0.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FElevationSourceRadialMonotonicityTest,
	"Massif.WorldGen.ElevationSource.RadialMonotonicity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FElevationSourceRadialMonotonicityTest::RunTest(const FString& Parameters)
{
	// The cone exists so drainage can be checked leaving in every direction, and
	// the pit so nothing leaves at all. Both rest on a monotonic radial profile.
	const FMassifAnalyticElevationSource Cone(MakeAnalyticParams(EMassifAnalyticTerrain::Cone, 0.0));
	TestTrue(TEXT("The cone descends the whole way out from its peak"),
		IsStrictlyDescending(SampleAnalyticRadialMeters(Cone)));

	const FMassifAnalyticElevationSource Pit(MakeAnalyticParams(EMassifAnalyticTerrain::Pit, 0.0));
	TestTrue(TEXT("The pit climbs the whole way out from its floor"),
		IsStrictlyAscending(SampleAnalyticRadialMeters(Pit)));

	TestTrue(TEXT("The pit is a closed depression below the surrounding ground"),
		Pit.SampleAltitudeMeters(FVector2d::ZeroVector, 1.0) < AnalyticBaseAltitudeMeters);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FElevationSourceRidgeSymmetryTest,
	"Massif.WorldGen.ElevationSource.RidgeSymmetry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FElevationSourceRidgeSymmetryTest::RunTest(const FString& Parameters)
{
	// Two flanks of the same shape are what makes a windward and a shaded side
	// comparable: any difference a process finds has to come from its own solve.
	const FMassifAnalyticElevationSource Ridge(MakeAnalyticParams(EMassifAnalyticTerrain::Ridge, 0.0));

	double WorstMeters = 0.0;
	for (int32 Step = 0; Step <= 40; ++Step)
	{
		const FVector2d PositionMeters(-800.0 + Step * 40.0, 130.0 + Step * 11.0);
		const double OneFlank = Ridge.SampleAltitudeMeters(PositionMeters, 1.0);
		const double OtherFlank = Ridge.SampleAltitudeMeters(FVector2d(-PositionMeters.X, PositionMeters.Y), 1.0);
		WorstMeters = FMath::Max(WorstMeters, FMath::Abs(OneFlank - OtherFlank));
	}

	TestEqual(TEXT("The ridge is symmetric across its crest"), WorstMeters, 0.0, AnalyticToleranceMeters);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FElevationSourcePlaneGradientTest,
	"Massif.WorldGen.ElevationSource.PlaneGradient",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FElevationSourcePlaneGradientTest::RunTest(const FString& Parameters)
{
	const FMassifAnalyticElevationSource Plane(MakeAnalyticParams(EMassifAnalyticTerrain::TiltedPlane, 0.0));

	// The whole relief is spent over the width of the box, descending towards the
	// bearing, and at the same rate everywhere in between.
	const double ExpectedGradient = -AnalyticReliefMeters / AnalyticWorldSizeMeters;
	const double StepMeters = 50.0;

	double WorstGradientDifference = 0.0;
	for (int32 Step = 0; Step < 20; ++Step)
	{
		const FVector2d PositionMeters(-400.0 + Step * 40.0, -600.0 + Step * 55.0);
		const double Here = Plane.SampleAltitudeMeters(PositionMeters, 1.0);
		const double Ahead = Plane.SampleAltitudeMeters(PositionMeters + FVector2d(0.0, StepMeters), 1.0);
		const double Gradient = (Ahead - Here) / StepMeters;
		WorstGradientDifference = FMath::Max(WorstGradientDifference, FMath::Abs(Gradient - ExpectedGradient));
	}

	TestEqual(FString::Printf(TEXT("The plane descends at a constant %.3f m per metre along the bearing"), ExpectedGradient),
		WorstGradientDifference, 0.0, 1.0e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FElevationSourceBearingTest,
	"Massif.WorldGen.ElevationSource.Bearing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FElevationSourceBearingTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("A quarter turn east rotates the ridge with it"),
		WorstQuarterTurnDifferenceMeters(EMassifAnalyticTerrain::Ridge), 0.0, AnalyticToleranceMeters);
	TestEqual(TEXT("A quarter turn east rotates the tilted plane with it"),
		WorstQuarterTurnDifferenceMeters(EMassifAnalyticTerrain::TiltedPlane), 0.0, AnalyticToleranceMeters);

	// A bearing that did nothing at all would pass the rotation check too, so the
	// two orientations also have to disagree somewhere.
	const FMassifAnalyticElevationSource FacingNorth(MakeAnalyticParams(EMassifAnalyticTerrain::Ridge, 0.0));
	const FMassifAnalyticElevationSource FacingEast(MakeAnalyticParams(EMassifAnalyticTerrain::Ridge, 90.0));
	const FVector2d OffCrestMeters(400.0, 0.0);
	const double SeparationMeters = FMath::Abs(
		FacingNorth.SampleAltitudeMeters(OffCrestMeters, 1.0) - FacingEast.SampleAltitudeMeters(OffCrestMeters, 1.0));
	TestTrue(TEXT("The two ridge orientations are different terrain"), SeparationMeters > 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FElevationSourceWorldBoxClampTest,
	"Massif.WorldGen.ElevationSource.WorldBoxClamp",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FElevationSourceWorldBoxClampTest::RunTest(const FString& Parameters)
{
	// Outside the box there is no world. A shape that kept extrapolating would
	// hand a solve altitudes far outside its own relief at the first stencil that
	// reached past an edge.
	const FMassifAnalyticElevationSource Plane(MakeAnalyticParams(EMassifAnalyticTerrain::TiltedPlane, 0.0));
	TestEqual(TEXT("Far north of the box the plane holds its northern edge"),
		Plane.SampleAltitudeMeters(FVector2d(0.0, 50000.0), 1.0),
		Plane.SampleAltitudeMeters(FVector2d(0.0, AnalyticHalfSizeMeters), 1.0), AnalyticToleranceMeters);
	TestEqual(TEXT("Far south of the box the plane holds its southern edge"),
		Plane.SampleAltitudeMeters(FVector2d(0.0, -50000.0), 1.0),
		Plane.SampleAltitudeMeters(FVector2d(0.0, -AnalyticHalfSizeMeters), 1.0), AnalyticToleranceMeters);
	TestEqual(TEXT("Only the axis that leaves the box is clamped"),
		Plane.SampleAltitudeMeters(FVector2d(9000.0, 300.0), 1.0),
		Plane.SampleAltitudeMeters(FVector2d(AnalyticHalfSizeMeters, 300.0), 1.0), AnalyticToleranceMeters);

	const FMassifAnalyticElevationSource Cone(MakeAnalyticParams(EMassifAnalyticTerrain::Cone, 0.0));
	TestEqual(TEXT("Beyond a corner the cone holds that corner"),
		Cone.SampleAltitudeMeters(FVector2d(4000.0, 4000.0), 1.0),
		Cone.SampleAltitudeMeters(FVector2d(AnalyticHalfSizeMeters, AnalyticHalfSizeMeters), 1.0), AnalyticToleranceMeters);

	TestEqual(TEXT("The world box is as wide as it was asked for"),
		Cone.GetWorldSizeMeters().X, AnalyticWorldSizeMeters, 0.0);
	TestEqual(TEXT("The world box is square"),
		Cone.GetWorldSizeMeters().Y, AnalyticWorldSizeMeters, 0.0);
	TestEqual(TEXT("The finest spacing is the one it was asked for"),
		Cone.GetFinestSpacingMeters(), 1.0, 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FElevationSourceHeightmapAdapterTest,
	"Massif.WorldGen.ElevationSource.HeightmapAdapter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FElevationSourceHeightmapAdapterTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifHeightmap> Heightmap = MakeAdapterHeightmap();
	const FMassifHeightmapElevationSource Adapter(Heightmap);
	const IMassifElevationSource& Source = Adapter;

	// The adapter is the only place the imported model reaches the environment
	// layer, so it must add nothing of its own: sample for sample, at every
	// spacing, the same surface.
	TestEqual(TEXT("The adapter agrees with the heightmap at the finest spacing"),
		WorstAdapterDifferenceMeters(Source, Heightmap.Get(), AdapterHeightmapSpacingMeters), 0.0, 0.0);
	TestEqual(TEXT("The adapter agrees with the heightmap below the finest spacing"),
		WorstAdapterDifferenceMeters(Source, Heightmap.Get(), 0.5), 0.0, 0.0);
	TestEqual(TEXT("The adapter agrees with the heightmap across the mip chain"),
		WorstAdapterDifferenceMeters(Source, Heightmap.Get(), 37.0), 0.0, 0.0);

	TestEqual(TEXT("The adapter reports the heightmap's east-west extent"),
		Source.GetWorldSizeMeters().X, Heightmap->GetWorldSizeXMeters(), 0.0);
	TestEqual(TEXT("The adapter reports the heightmap's north-south extent"),
		Source.GetWorldSizeMeters().Y, Heightmap->GetWorldSizeYMeters(), 0.0);
	TestEqual(TEXT("The adapter reports the heightmap's finest spacing"),
		Source.GetFinestSpacingMeters(), Heightmap->GetFinestSpacingMeters(), 0.0);
	return true;
}
