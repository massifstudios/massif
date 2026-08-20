#pragma once

#include "CoreMinimal.h"
#include "MassifHabitat.h"

/**
 * One kind of plant and the habitat it grows in. Presentation data only:
 * nothing here is read by the sim, and vegetation that ever becomes
 * gameplay-relevant comes from worldgen masks instead (ARCHITECTURE §5.5).
 *
 * A species is described by the conditions it tolerates rather than by an
 * altitude band: temperature carries the altitude, and because it also carries
 * how the ground faces the sun, the treeline rises on south slopes and drops
 * into shaded north bowls the way a real one does.
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

	/** Candidates per hectare before the habitat weights reject any. */
	double DensityPerHectare = 90.0;

	/** Growing-season temperature the species tolerates, and the fade at each end. */
	double MinTemperatureCelsius = 5.0;
	double MaxTemperatureCelsius = 15.0;
	double TemperatureFadeCelsius = 1.5;

	/** Soil moisture the species tolerates. A maximum above 1 means it never drowns. */
	double MinHumidity = 0.3;
	double MaxHumidity = 1.2;
	double HumidityFade = 0.12;

	/** Steepest ground the species holds, and the fade below it. */
	double MaxSlopeDegrees = 38.0;
	double SlopeFadeDegrees = 8.0;

	/** Ground types the species roots in, as EMassifGroundType bits. */
	uint32 GroundTypeMask = MassifGround::Soil;

	float MinScale = 0.8f;
	float MaxScale = 1.35f;
};

/**
 * The planted world. Temperatures suit the 500-3400 m massif under the lapse
 * rate in MassifHabitat: conifers thin out into saplings towards the treeline
 * near 6 °C, ground cover carries on past it.
 */
namespace MassifFoliage
{
	inline constexpr FMassifFoliageSpecies Species[] = {
		{
			.Name = TEXT("FirTree01"),
			.MeshAssetPath = TEXT("/Game/Models/PolyHaven/tree/SM_FirTree01"),
			.TileSizeMeters = 256.0,
			.VisibleRadiusMeters = 2600.0,
			.DensityPerHectare = 55.0,
			// Firs take the cool damp side of the forest: shaded slopes and hollows.
			.MinTemperatureCelsius = 5.5,
			.MaxTemperatureCelsius = 14.5,
			.TemperatureFadeCelsius = 1.7,
			.MinHumidity = 0.40,
			.MaxHumidity = 1.2,
			.HumidityFade = 0.14,
			.MaxSlopeDegrees = 38.0,
			.SlopeFadeDegrees = 8.0,
			.GroundTypeMask = MassifGround::Soil,
			.MinScale = 0.80f,
			.MaxScale = 1.40f,
		},
		{
			.Name = TEXT("PineTree01"),
			.MeshAssetPath = TEXT("/Game/Models/PolyHaven/tree/SM_PineTree01"),
			.TileSizeMeters = 256.0,
			.VisibleRadiusMeters = 2600.0,
			.DensityPerHectare = 35.0,
			// Pines take the dry sunny side, and give way where the ground stays wet.
			.MinTemperatureCelsius = 6.6,
			.MaxTemperatureCelsius = 14.8,
			.TemperatureFadeCelsius = 1.6,
			.MinHumidity = 0.18,
			.MaxHumidity = 0.72,
			.HumidityFade = 0.12,
			.MaxSlopeDegrees = 36.0,
			.SlopeFadeDegrees = 8.0,
			.GroundTypeMask = MassifGround::Soil,
			.MinScale = 0.85f,
			.MaxScale = 1.30f,
		},
		{
			.Name = TEXT("FirSaplingMedium"),
			.MeshAssetPath = TEXT("/Game/Models/PolyHaven/sapling/SM_FirSaplingMedium"),
			.TileSizeMeters = 256.0,
			.VisibleRadiusMeters = 1200.0,
			.DensityPerHectare = 60.0,
			.MinTemperatureCelsius = 4.4,
			.MaxTemperatureCelsius = 13.2,
			.TemperatureFadeCelsius = 1.3,
			.MinHumidity = 0.34,
			.MaxHumidity = 1.2,
			.HumidityFade = 0.14,
			.MaxSlopeDegrees = 42.0,
			.SlopeFadeDegrees = 10.0,
			.GroundTypeMask = MassifGround::Soil,
			.MinScale = 0.70f,
			.MaxScale = 1.25f,
		},
		{
			.Name = TEXT("PineSaplingMedium"),
			.MeshAssetPath = TEXT("/Game/Models/PolyHaven/sapling/SM_PineSaplingMedium"),
			.TileSizeMeters = 256.0,
			.VisibleRadiusMeters = 1200.0,
			.DensityPerHectare = 45.0,
			.MinTemperatureCelsius = 4.7,
			.MaxTemperatureCelsius = 13.5,
			.TemperatureFadeCelsius = 1.3,
			.MinHumidity = 0.16,
			.MaxHumidity = 0.78,
			.HumidityFade = 0.12,
			.MaxSlopeDegrees = 42.0,
			.SlopeFadeDegrees = 10.0,
			.GroundTypeMask = MassifGround::Soil,
			.MinScale = 0.70f,
			.MaxScale = 1.25f,
		},
		{
			.Name = TEXT("Shrub01"),
			.MeshAssetPath = TEXT("/Game/Models/PolyHaven/groundcover/SM_Shrub01"),
			.TileSizeMeters = 128.0,
			.VisibleRadiusMeters = 450.0,
			.DensityPerHectare = 220.0,
			// Dwarf shrub: carries on above the treeline and takes hold in loose debris.
			.MinTemperatureCelsius = 4.0,
			.MaxTemperatureCelsius = 15.1,
			.TemperatureFadeCelsius = 1.4,
			.MinHumidity = 0.22,
			.MaxHumidity = 0.95,
			.HumidityFade = 0.12,
			.MaxSlopeDegrees = 40.0,
			.SlopeFadeDegrees = 10.0,
			.GroundTypeMask = MassifGround::Soil | MassifGround::Scree,
			.MinScale = 0.65f,
			.MaxScale = 1.35f,
		},
		{
			.Name = TEXT("GrassMedium01"),
			.MeshAssetPath = TEXT("/Game/Models/PolyHaven/groundcover/SM_GrassMedium01"),
			.TileSizeMeters = 32.0,
			.VisibleRadiusMeters = 140.0,
			.DensityPerHectare = 4200.0,
			// Damp meadow grass: thickest in the hollows, and the only thing in the bogs.
			.MinTemperatureCelsius = 5.7,
			.MaxTemperatureCelsius = 15.8,
			.TemperatureFadeCelsius = 1.4,
			.MinHumidity = 0.38,
			.MaxHumidity = 1.2,
			.HumidityFade = 0.12,
			.MaxSlopeDegrees = 35.0,
			.SlopeFadeDegrees = 9.0,
			.GroundTypeMask = MassifGround::Soil | MassifGround::Wetland,
			.MinScale = 0.75f,
			.MaxScale = 1.30f,
		},
		{
			.Name = TEXT("GrassMedium02"),
			.MeshAssetPath = TEXT("/Game/Models/PolyHaven/groundcover/SM_GrassMedium02"),
			.TileSizeMeters = 32.0,
			.VisibleRadiusMeters = 140.0,
			.DensityPerHectare = 3000.0,
			// Dry meadow grass: the sunny slopes the damp one leaves bare.
			.MinTemperatureCelsius = 5.4,
			.MaxTemperatureCelsius = 15.8,
			.TemperatureFadeCelsius = 1.4,
			.MinHumidity = 0.12,
			.MaxHumidity = 0.62,
			.HumidityFade = 0.12,
			.MaxSlopeDegrees = 35.0,
			.SlopeFadeDegrees = 9.0,
			.GroundTypeMask = MassifGround::Soil | MassifGround::Scree,
			.MinScale = 0.75f,
			.MaxScale = 1.30f,
		},
	};
}
