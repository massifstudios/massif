#pragma once

#include "CoreMinimal.h"

/**
 * One kind of plant and the ground it grows on. Presentation data only:
 * nothing here is read by the sim, and vegetation that ever becomes
 * gameplay-relevant comes from worldgen masks instead (ARCHITECTURE §5.5).
 *
 * A species owns its own scatter tiling: conifers are placed over wide tiles
 * out to kilometres, ground cover over small ones that only exist within a
 * short walk of the camera.
 */
struct FMassifFoliageSpecies
{
	const TCHAR* Name = nullptr;
	const TCHAR* MeshAssetPath = nullptr;

	/** Side of one scatter tile, and how far from the camera tiles are kept. */
	double TileSizeMeters = 256.0;
	double VisibleRadiusMeters = 1500.0;

	/** Candidates per hectare before the altitude and slope weights reject any. */
	double DensityPerHectare = 90.0;

	/** Altitude band the species occupies, and the distance it fades over at each end. */
	double MinAltitudeMeters = 700.0;
	double MaxAltitudeMeters = 2100.0;
	double AltitudeFadeMeters = 250.0;

	/** Steepest ground the species holds, and the fade below it. */
	double MaxSlopeDegrees = 38.0;
	double SlopeFadeDegrees = 8.0;

	float MinScale = 0.8f;
	float MaxScale = 1.35f;
};

/**
 * The planted world. Altitudes suit the 500-3400 m massif: conifers thin out
 * into saplings towards the treeline, ground cover stops well below it.
 */
namespace MassifFoliage
{
	inline constexpr FMassifFoliageSpecies Species[] = {
		{ TEXT("FirTree01"), TEXT("/Game/Models/PolyHaven/tree/SM_FirTree01"),
		  256.0, 1500.0, 55.0, 700.0, 2050.0, 260.0, 38.0, 8.0, 0.80f, 1.40f },
		{ TEXT("PineTree01"), TEXT("/Game/Models/PolyHaven/tree/SM_PineTree01"),
		  256.0, 1500.0, 35.0, 650.0, 1900.0, 240.0, 36.0, 8.0, 0.85f, 1.30f },
		{ TEXT("FirSaplingMedium"), TEXT("/Game/Models/PolyHaven/sapling/SM_FirSaplingMedium"),
		  256.0, 800.0, 60.0, 900.0, 2250.0, 200.0, 42.0, 10.0, 0.70f, 1.25f },
		{ TEXT("PineSaplingMedium"), TEXT("/Game/Models/PolyHaven/sapling/SM_PineSaplingMedium"),
		  256.0, 800.0, 45.0, 850.0, 2200.0, 200.0, 42.0, 10.0, 0.70f, 1.25f },
		{ TEXT("Shrub01"), TEXT("/Game/Models/PolyHaven/groundcover/SM_Shrub01"),
		  128.0, 450.0, 220.0, 600.0, 2300.0, 220.0, 40.0, 10.0, 0.65f, 1.35f },
		{ TEXT("GrassMedium01"), TEXT("/Game/Models/PolyHaven/groundcover/SM_GrassMedium01"),
		  32.0, 140.0, 4200.0, 500.0, 2050.0, 220.0, 35.0, 9.0, 0.75f, 1.30f },
		{ TEXT("GrassMedium02"), TEXT("/Game/Models/PolyHaven/groundcover/SM_GrassMedium02"),
		  32.0, 140.0, 3000.0, 500.0, 2100.0, 220.0, 35.0, 9.0, 0.75f, 1.30f },
	};
}
