#pragma once

#include "CoreMinimal.h"
#include "MassifHabitat.h"

/**
 * One ground texture set and the habitat it covers. Presentation data only: the
 * sim never reads which texture a slope wears.
 *
 * A layer is described by the conditions it belongs in rather than by an
 * altitude band, for the same reason a foliage species is
 * (MassifFoliageSpecies.h): temperature carries altitude and how the ground
 * faces the sun with it, so a band written once rides up the sunny flanks and
 * down the shaded ones on its own. Terrain and plants then agree by
 * construction — larch woodland stands on forest floor, not on scree.
 *
 * Niches are meant to overlap. Where several layers fit, the patch noise picks
 * between them over tens of metres, which is what stops a whole hillside
 * wearing one texture.
 *
 * Order is the slice order of the texture arrays Tools/create_terrain_arrays.py
 * builds, which reads this table. Inserting a layer renumbers the ones after
 * it, and the arrays have to be rebuilt; nothing is serialized against these
 * indices, so that costs a content rebuild and no more.
 */
struct FMassifSurfaceLayer
{
	const TCHAR* Name = nullptr;

	/** Poly Haven group folder and asset stem: Content/Textures/PolyHaven/<Group>/T_<Set>_D.jpg. */
	const TCHAR* TextureGroup = nullptr;
	const TCHAR* TextureSet = nullptr;

	/** Metres one repeat of the texture covers. */
	double TileMeters = 4.0;

	/**
	 * Metres of relief the height map stands for, which is what the parallax
	 * raymarch offsets by and what the layer boundary is cut against. Measured,
	 * not a look control: pebbles are a couple of centimetres deep and a rock
	 * face is a third of a metre.
	 */
	double ReliefMeters = 0.03;

	/** Growing-season temperature the layer belongs in, and the fade at each end. */
	double MinTemperatureCelsius = -20.0;
	double MaxTemperatureCelsius = 25.0;
	double TemperatureFadeCelsius = 1.5;

	/** Soil moisture the layer belongs in. */
	double MinHumidity = 0.0;
	double MaxHumidity = 1.2;
	double HumidityFade = 0.12;

	/** Ground angle the layer belongs on, and the fade at each end. */
	double MinSlopeDegrees = 0.0;
	double MaxSlopeDegrees = 90.0;
	double SlopeFadeDegrees = 8.0;

	/** Ground types the layer covers, as EMassifGroundType bits. */
	uint32 GroundTypeMask = MassifGround::Soil;

	/**
	 * Weight against the other layers that fit the same ground. Layers competing
	 * for one niche divide it in this proportion rather than the first one
	 * winning outright.
	 */
	double Abundance = 1.0;

	/**
	 * Size of the patches the layer comes in. Driven by position, so a patch
	 * keeps its shape across bake cells instead of being cut off at them. Every
	 * layer patches: two sets fitting one niche have to alternate over the
	 * ground, or the higher abundance takes all of it.
	 */
	double PatchWavelengthMeters = 60.0;
};

/**
 * The ground of the Ötztal, over the same window the heightmap and the flora
 * come from: 1168 m of valley floor to 3757 m of summit, on acid crystalline
 * bedrock in an inner-alpine dry valley.
 *
 * Read against the temperatures MassifHabitat derives by lapse rate — 11.2 °C
 * at 1200 m, 6.0 °C at 2000 m, 3.4 °C at the 2400 m treeline, 0 °C near 2900 m
 * — with up to 2.5 °C either way from aspect.
 *
 * Permanent snow is not in this table. It goes over the finished ground as its
 * own blend, so that when the sim drives snow depth it can cover any layer
 * without the ground underneath being reclassified.
 */
namespace MassifSurface
{
	inline constexpr FMassifSurfaceLayer Layers[] = {

		// --- Bedrock: faces too steep to hold anything ------------------------

		{
			.Name = TEXT("RockFace"),
			.TextureGroup = TEXT("rock"),
			.TextureSet = TEXT("RockFace03"),
			.TileMeters = 9.0,
			.ReliefMeters = 0.30,
			// The headwalls. No climate term at all: a face this steep is bare
			// whatever the weather does around it.
			.MinSlopeDegrees = 40.0,
			.SlopeFadeDegrees = 9.0,
			.GroundTypeMask = MassifGround::Rock,
			.Abundance = 1.4,
			.PatchWavelengthMeters = 140.0,
		},
		{
			.Name = TEXT("GrayRocks"),
			.TextureGroup = TEXT("rock"),
			.TextureSet = TEXT("GrayRocks"),
			.TileMeters = 7.0,
			.ReliefMeters = 0.22,
			// Frost-shattered rock of the high ground, blockier than the faces below it.
			.MaxTemperatureCelsius = 5.0,
			.TemperatureFadeCelsius = 2.0,
			.MinSlopeDegrees = 30.0,
			.SlopeFadeDegrees = 8.0,
			.GroundTypeMask = MassifGround::Rock | MassifGround::Scree,
			.Abundance = 1.2,
			.PatchWavelengthMeters = 120.0,
		},
		{
			.Name = TEXT("AerialRocks"),
			.TextureGroup = TEXT("rock"),
			.TextureSet = TEXT("AerialRocks02"),
			.TileMeters = 34.0,
			.ReliefMeters = 0.45,
			// Shot from the air, so its features are metres across: the layer that
			// carries the big slabby ground a detail-scale set cannot.
			.MaxTemperatureCelsius = 8.0,
			.TemperatureFadeCelsius = 2.5,
			.MinSlopeDegrees = 22.0,
			.SlopeFadeDegrees = 10.0,
			.GroundTypeMask = MassifGround::Rock | MassifGround::Scree,
			.Abundance = 1.0,
			.PatchWavelengthMeters = 260.0,
		},

		// --- Rocky ground: bedrock that has broken up but not moved -----------

		{
			.Name = TEXT("RockyTerrain"),
			.TextureGroup = TEXT("rock"),
			.TextureSet = TEXT("RockyTerrain"),
			.TileMeters = 6.0,
			.ReliefMeters = 0.14,
			.MaxHumidity = 0.75,
			.MinSlopeDegrees = 12.0,
			.MaxSlopeDegrees = 46.0,
			.SlopeFadeDegrees = 9.0,
			.GroundTypeMask = MassifGround::Rock | MassifGround::Scree,
			.Abundance = 1.0,
			.PatchWavelengthMeters = 90.0,
		},
		{
			.Name = TEXT("RockyTerrainCool"),
			.TextureGroup = TEXT("ground"),
			.TextureSet = TEXT("RockyTerrain02"),
			.TileMeters = 22.0,
			.ReliefMeters = 0.20,
			.MaxTemperatureCelsius = 6.5,
			.TemperatureFadeCelsius = 2.0,
			.MaxSlopeDegrees = 42.0,
			.SlopeFadeDegrees = 9.0,
			.GroundTypeMask = MassifGround::Rock | MassifGround::Scree,
			.Abundance = 1.0,
			.PatchWavelengthMeters = 170.0,
		},
		{
			.Name = TEXT("RockyTerrainDry"),
			.TextureGroup = TEXT("rock"),
			.TextureSet = TEXT("RockyTerrain03"),
			.TileMeters = 8.0,
			.ReliefMeters = 0.16,
			.MaxHumidity = 0.5,
			.HumidityFade = 0.14,
			.MinSlopeDegrees = 16.0,
			.MaxSlopeDegrees = 50.0,
			.SlopeFadeDegrees = 10.0,
			.GroundTypeMask = MassifGround::Rock | MassifGround::Scree,
			.Abundance = 0.9,
			.PatchWavelengthMeters = 110.0,
		},
		{
			.Name = TEXT("RocksGround"),
			.TextureGroup = TEXT("rock"),
			.TextureSet = TEXT("RocksGround03"),
			.TileMeters = 5.0,
			.ReliefMeters = 0.12,
			// The stony flats between outcrops, where blocks sit in whatever fine
			// material has washed between them.
			.MaxTemperatureCelsius = 9.0,
			.TemperatureFadeCelsius = 2.0,
			.MaxSlopeDegrees = 30.0,
			.SlopeFadeDegrees = 8.0,
			.GroundTypeMask = MassifGround::Rock | MassifGround::Scree | MassifGround::Soil,
			.Abundance = 0.8,
			.PatchWavelengthMeters = 80.0,
		},
		{
			.Name = TEXT("GrassyRock"),
			.TextureGroup = TEXT("rock"),
			.TextureSet = TEXT("AerialGrassRock"),
			.TileMeters = 18.0,
			.ReliefMeters = 0.18,
			// Rock with turf taking hold in its cracks: the transition the treeline
			// belt actually looks like, rather than a hard edge from stone to grass.
			.MinTemperatureCelsius = 3.0,
			.MaxTemperatureCelsius = 11.0,
			.TemperatureFadeCelsius = 1.6,
			.MinHumidity = 0.30,
			.HumidityFade = 0.13,
			.MaxSlopeDegrees = 38.0,
			.SlopeFadeDegrees = 9.0,
			.GroundTypeMask = MassifGround::Rock | MassifGround::Scree | MassifGround::Soil,
			.Abundance = 1.1,
			.PatchWavelengthMeters = 150.0,
		},

		// --- Scree and moraine: material the mountain has shed ----------------

		{
			.Name = TEXT("GravellySand"),
			.TextureGroup = TEXT("scree"),
			.TextureSet = TEXT("GravellySand"),
			.TileMeters = 3.0,
			.ReliefMeters = 0.05,
			.MaxHumidity = 0.6,
			.MinSlopeDegrees = 18.0,
			.MaxSlopeDegrees = 42.0,
			.SlopeFadeDegrees = 8.0,
			.GroundTypeMask = MassifGround::Scree,
			.Abundance = 1.2,
			.PatchWavelengthMeters = 70.0,
		},
		{
			.Name = TEXT("MoraineSand"),
			.TextureGroup = TEXT("scree"),
			.TextureSet = TEXT("AerialBeach01"),
			.TileMeters = 16.0,
			.ReliefMeters = 0.06,
			// Glacial outwash: the pale fine ground below a snout, above anything
			// that grows.
			.MaxTemperatureCelsius = 3.5,
			.TemperatureFadeCelsius = 1.8,
			.MaxSlopeDegrees = 26.0,
			.SlopeFadeDegrees = 7.0,
			.GroundTypeMask = MassifGround::Scree | MassifGround::Soil,
			.Abundance = 1.0,
			.PatchWavelengthMeters = 200.0,
		},
		{
			.Name = TEXT("DryRiverPebbles"),
			.TextureGroup = TEXT("scree"),
			.TextureSet = TEXT("DryRiverPebbles"),
			.TileMeters = 2.5,
			.ReliefMeters = 0.04,
			// The bars a braided channel leaves dry between floods, which is most of
			// what a bank is made of.
			.MinTemperatureCelsius = 5.0,
			.TemperatureFadeCelsius = 1.6,
			.MinHumidity = 0.35,
			.MaxHumidity = 1.2,
			.MaxSlopeDegrees = 16.0,
			.SlopeFadeDegrees = 5.0,
			.GroundTypeMask = MassifGround::Scree | MassifGround::Soil | MassifGround::Wetland,
			.Abundance = 1.0,
			.PatchWavelengthMeters = 55.0,
		},
		{
			.Name = TEXT("RiverRocks"),
			.TextureGroup = TEXT("scree"),
			.TextureSet = TEXT("RiverSmallRocks"),
			.TileMeters = 2.5,
			.ReliefMeters = 0.06,
			// The wetted bed itself, which is why the humidity floor is so high and
			// why this is the one layer that covers the bed of a channel. Nothing
			// else in the table takes Water, so the ground under a river would
			// otherwise fall through to whatever the fallback happened to rank first.
			.MinHumidity = 0.72,
			.HumidityFade = 0.10,
			.MaxSlopeDegrees = 14.0,
			.SlopeFadeDegrees = 5.0,
			.GroundTypeMask =
				MassifGround::Water | MassifGround::Scree | MassifGround::Wetland | MassifGround::Soil,
			.Abundance = 1.4,
			.PatchWavelengthMeters = 45.0,
		},
		{
			.Name = TEXT("Sand"),
			.TextureGroup = TEXT("scree"),
			.TextureSet = TEXT("Sand01"),
			.TileMeters = 2.0,
			.ReliefMeters = 0.03,
			.MinTemperatureCelsius = 7.0,
			.TemperatureFadeCelsius = 1.5,
			.MaxHumidity = 0.4,
			.HumidityFade = 0.11,
			.MaxSlopeDegrees = 20.0,
			.SlopeFadeDegrees = 6.0,
			// Soil as well as scree: MassifHabitat only calls ground scree where it
			// is steeper than the angle of repose, so a scree-only layer this flat
			// would describe ground that cannot exist.
			.GroundTypeMask = MassifGround::Scree | MassifGround::Soil,
			.Abundance = 0.7,
			.PatchWavelengthMeters = 60.0,
		},
		{
			.Name = TEXT("RockyTrail"),
			.TextureGroup = TEXT("scree"),
			.TextureSet = TEXT("RockyTrail"),
			.TileMeters = 3.5,
			.ReliefMeters = 0.07,
			.MinTemperatureCelsius = 2.0,
			.MaxTemperatureCelsius = 11.0,
			.TemperatureFadeCelsius = 1.6,
			.MaxHumidity = 0.75,
			.MaxSlopeDegrees = 28.0,
			.SlopeFadeDegrees = 8.0,
			.GroundTypeMask = MassifGround::Scree | MassifGround::Soil,
			.Abundance = 0.9,
			.PatchWavelengthMeters = 75.0,
		},
		{
			.Name = TEXT("StonyGround"),
			.TextureGroup = TEXT("scree"),
			.TextureSet = TEXT("RockyTrail02"),
			.TileMeters = 4.0,
			.ReliefMeters = 0.08,
			.MinTemperatureCelsius = 1.0,
			.MaxTemperatureCelsius = 10.0,
			.TemperatureFadeCelsius = 1.6,
			.MaxHumidity = 0.65,
			.MaxSlopeDegrees = 34.0,
			.SlopeFadeDegrees = 9.0,
			.GroundTypeMask = MassifGround::Scree | MassifGround::Soil,
			.Abundance = 0.9,
			.PatchWavelengthMeters = 85.0,
		},
		{
			.Name = TEXT("SandyRocks"),
			.TextureGroup = TEXT("ground"),
			.TextureSet = TEXT("CoastSandRocks02"),
			.TileMeters = 5.0,
			.ReliefMeters = 0.09,
			.MinTemperatureCelsius = 6.0,
			.TemperatureFadeCelsius = 1.6,
			.MaxHumidity = 0.55,
			.MaxSlopeDegrees = 26.0,
			.SlopeFadeDegrees = 7.0,
			.GroundTypeMask = MassifGround::Scree | MassifGround::Soil,
			.Abundance = 0.7,
			.PatchWavelengthMeters = 90.0,
		},

		// --- Soil: ground a root can hold in ----------------------------------

		{
			.Name = TEXT("Dirt"),
			.TextureGroup = TEXT("ground"),
			.TextureSet = TEXT("Dirt"),
			.TileMeters = 3.0,
			.ReliefMeters = 0.04,
			// Bare earth: the dry sunny banks where the turf has worn through.
			.MinTemperatureCelsius = 5.0,
			.TemperatureFadeCelsius = 1.6,
			.MaxHumidity = 0.5,
			.HumidityFade = 0.12,
			.MaxSlopeDegrees = 36.0,
			.SlopeFadeDegrees = 9.0,
			.GroundTypeMask = MassifGround::Soil,
			.Abundance = 0.8,
			.PatchWavelengthMeters = 70.0,
		},
		{
			.Name = TEXT("ForestMud"),
			.TextureGroup = TEXT("ground"),
			.TextureSet = TEXT("MudForest"),
			.TileMeters = 3.0,
			.ReliefMeters = 0.05,
			// Wet ground under the trees, and the hollows that stay soft all season.
			.MinTemperatureCelsius = 5.0,
			.MaxTemperatureCelsius = 12.0,
			.TemperatureFadeCelsius = 1.4,
			.MinHumidity = 0.62,
			.HumidityFade = 0.11,
			.MaxSlopeDegrees = 24.0,
			.SlopeFadeDegrees = 7.0,
			.GroundTypeMask = MassifGround::Soil | MassifGround::Wetland,
			.Abundance = 1.2,
			.PatchWavelengthMeters = 55.0,
		},
		{
			.Name = TEXT("MudLeaves"),
			.TextureGroup = TEXT("ground"),
			.TextureSet = TEXT("BrownMudLeaves01"),
			.TileMeters = 2.5,
			.ReliefMeters = 0.04,
			.MinTemperatureCelsius = 5.5,
			.MaxTemperatureCelsius = 11.8,
			.TemperatureFadeCelsius = 1.4,
			.MinHumidity = 0.45,
			.HumidityFade = 0.12,
			.MaxSlopeDegrees = 30.0,
			.SlopeFadeDegrees = 8.0,
			.GroundTypeMask = MassifGround::Soil,
			.Abundance = 1.0,
			.PatchWavelengthMeters = 50.0,
		},

		// --- Forest floor, under the montane and subalpine woodland -----------

		{
			.Name = TEXT("ForestFloorMontane"),
			.TextureGroup = TEXT("forest"),
			.TextureSet = TEXT("ForrestGround01"),
			.TileMeters = 3.5,
			.ReliefMeters = 0.05,
			.MinTemperatureCelsius = 5.5,
			.MaxTemperatureCelsius = 11.6,
			.TemperatureFadeCelsius = 1.3,
			.MinHumidity = 0.38,
			.HumidityFade = 0.13,
			.MaxSlopeDegrees = 36.0,
			.SlopeFadeDegrees = 9.0,
			.GroundTypeMask = MassifGround::Soil,
			.Abundance = 1.1,
			.PatchWavelengthMeters = 65.0,
		},
		{
			.Name = TEXT("ForestFloorNeedle"),
			.TextureGroup = TEXT("forest"),
			.TextureSet = TEXT("ForestGround04"),
			.TileMeters = 3.0,
			.ReliefMeters = 0.04,
			.MinTemperatureCelsius = 4.0,
			.MaxTemperatureCelsius = 11.0,
			.TemperatureFadeCelsius = 1.3,
			.MinHumidity = 0.32,
			.MaxHumidity = 0.95,
			.HumidityFade = 0.13,
			.MaxSlopeDegrees = 36.0,
			.SlopeFadeDegrees = 9.0,
			.GroundTypeMask = MassifGround::Soil,
			.Abundance = 1.1,
			.PatchWavelengthMeters = 60.0,
		},
		{
			.Name = TEXT("ForestLeaves"),
			.TextureGroup = TEXT("forest"),
			.TextureSet = TEXT("ForestLeaves02"),
			.TileMeters = 2.5,
			.ReliefMeters = 0.04,
			// Deep leaf litter, so only where the broadleaves are: the warm damp
			// gullies low down.
			.MinTemperatureCelsius = 7.5,
			.MaxTemperatureCelsius = 11.8,
			.TemperatureFadeCelsius = 1.2,
			.MinHumidity = 0.48,
			.HumidityFade = 0.11,
			.MaxSlopeDegrees = 28.0,
			.SlopeFadeDegrees = 8.0,
			.GroundTypeMask = MassifGround::Soil,
			.Abundance = 0.9,
			.PatchWavelengthMeters = 45.0,
		},

		// --- Turf and meadow --------------------------------------------------

		{
			.Name = TEXT("LeafyGrass"),
			.TextureGroup = TEXT("grass"),
			.TextureSet = TEXT("LeafyGrass"),
			.TileMeters = 2.0,
			.ReliefMeters = 0.03,
			// The mown meadows of the valley floor and the pasture above them.
			.MinTemperatureCelsius = 5.5,
			.MaxTemperatureCelsius = 11.8,
			.TemperatureFadeCelsius = 1.3,
			.MinHumidity = 0.40,
			.HumidityFade = 0.12,
			.MaxSlopeDegrees = 30.0,
			.SlopeFadeDegrees = 8.0,
			.GroundTypeMask = MassifGround::Soil,
			.Abundance = 1.3,
			.PatchWavelengthMeters = 80.0,
		},
		{
			.Name = TEXT("SparseGrass"),
			.TextureGroup = TEXT("grass"),
			.TextureSet = TEXT("SparseGrass"),
			.TileMeters = 2.5,
			.ReliefMeters = 0.03,
			// Alpine turf breaking up into the ground under it, which is the whole
			// belt between the treeline and the bare rock.
			.MinTemperatureCelsius = 0.0,
			.MaxTemperatureCelsius = 8.0,
			.TemperatureFadeCelsius = 1.3,
			.MinHumidity = 0.15,
			.MaxHumidity = 0.85,
			.HumidityFade = 0.13,
			.MaxSlopeDegrees = 40.0,
			.SlopeFadeDegrees = 10.0,
			.GroundTypeMask = MassifGround::Soil | MassifGround::Scree,
			.Abundance = 1.4,
			.PatchWavelengthMeters = 95.0,
		},
	};

	inline constexpr int32 LayerCount = UE_ARRAY_COUNT(Layers);

	/**
	 * Snow over the finished ground. Not a layer: it is blended last, so the sim
	 * can drive its coverage without the ground underneath changing.
	 */
	inline constexpr const TCHAR* SnowTextureGroup = TEXT("snow");
	inline constexpr const TCHAR* SnowTextureSet = TEXT("Snow02");
	inline constexpr double SnowTileMeters = 6.0;
	inline constexpr double SnowReliefMeters = 0.06;

	/** Snow is the array slice after the last layer, drawn by the same code. */
	inline constexpr int32 SnowSlice = LayerCount;

	static_assert(LayerCount <= 255, "The surface map stores a layer index in one byte");
}
