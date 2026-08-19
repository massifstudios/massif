#pragma once

#include "CoreMinimal.h"
#include "WorldGenConfig.generated.h"

/** Input parameters for world generation. All values in meters unless noted. */
USTRUCT(BlueprintType)
struct MASSIFWORLDGEN_API FWorldGenConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen")
	int32 MasterSeed = 1337;

	/** Heightfield samples per side. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen")
	int32 Resolution = 4096;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen")
	float WorldSizeMeters = 8192.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen")
	float MinAltitudeMeters = 1200.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen")
	float MaxAltitudeMeters = 3000.0f;

	/**
	 * Lattice the macro surface is evaluated on before being interpolated up to
	 * the heightfield. It varies over hundreds of meters, so it does not need
	 * the full grid; its central differences also supply the surface slope the
	 * erosion filter orients its gullies by.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	int32 RiverFieldResolution = 513;

	/** Watercourses reaching the map border, each the outlet of one catchment. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	int32 RiverMouthCount = 7;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float RiverMouthSpacingMeters = 1600.0f;

	/** Distance between successive nodes along a watercourse. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float RiverSegmentLengthMeters = 520.0f;

	/** Watercourses may not come closer to each other than this, which sets valley spacing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float RiverMinNodeSpacingMeters = 360.0f;

	/** Chance that a node forks into two tributaries instead of continuing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float RiverBranchProbability = 0.6f;

	/** Heading change allowed between successive nodes, degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float RiverTurnDegrees = 28.0f;

	/** Half-angle between the two branches of a fork, degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float RiverForkDegrees = 40.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	int32 RiverMaxNodeCount = 1200;

	/** Valley floor gradient just above a mouth. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float RiverSlopeNearMouth = 0.03f;

	/** Valley floor gradient in the headwaters. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float RiverSlopeHeadwater = 0.14f;

	/** How far the ground rises above a trunk valley's floor before it levels off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float ValleyReliefMeters = 1500.0f;

	/**
	 * The same for a headwater stream. Well below the trunk figure: a headwater
	 * cannot stand its own crest higher than the ground the trunk it drains into
	 * allows around it, or the stream ends up floating above the surface.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float HeadwaterReliefMeters = 300.0f;

	/** Horizontal scale of the valley side on a trunk river; smaller is steeper. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float TrunkValleyWidthMeters = 1600.0f;

	/** The same for a headwater stream, which only notches the slope it runs down. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float HeadwaterValleyWidthMeters = 500.0f;

	/** Flat ground either side of the water before the valley side starts to climb. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float TrunkFloorHalfWidthMeters = 120.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float HeadwaterFloorHalfWidthMeters = 25.0f;

	/**
	 * Ground keeps climbing this much per meter away from a watercourse even
	 * after the valley side has levelled off. Without it the interfluves come
	 * out as plateaus at one fixed height.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float InterfluveRiseSlope = 0.02f;

	/**
	 * Highest a watercourse may start, as a fraction of the altitude range. Has
	 * to stay under what the trunk valleys' relief lifts the ground to, or the
	 * headwaters end up above their own terrain.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float RiverMaxSourceAltitudeFraction = 0.6f;

	/** Rounding of the crest where two catchments meet, meters. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float RidgeBlendMeters = 450.0f;

	/**
	 * Ridge features per map side. Low and few: this layer only varies how high
	 * the crests stand, and the erosion filter shapes everything below that. Run
	 * at a fine enough scale it out-steepens the valley sides, and the filter
	 * then orients its gullies to the noise instead of to the mountain.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float RidgeFrequency = 2.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	int32 RidgeOctaveCount = 3;

	/** Ridge crest profile: 1.0 soft and rounded, 2.0 knife edges. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float RidgeSharpness = 1.0f;

	/** How much the ridge noise pushes the crests up and down, meters. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float RidgeAmplitudeMeters = 220.0f;

	/** Sideways displacement of the query point, meters; bends the valleys off the graph's straight segments. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float TerrainWarpMeters = 600.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float TerrainWarpFrequency = 5.0f;

	/** Fraction of the map edge over which height falls off to keep borders low. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float EdgeFalloffFraction = 0.12f;

	/** Gully scales the erosion filter cuts, coarsest first; 0 leaves the macro surface bare. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|ErosionFilter")
	int32 GullyOctaveCount = 4;

	/** Spacing of the coarsest gullies. Keep it under the valley spacing or gullies compete with valleys. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|ErosionFilter")
	float LargestGullyWavelengthMeters = 220.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|ErosionFilter")
	float LargestGullyDepthMeters = 18.0f;

	/** Wavelength and depth left per octave. Equal values keep gully steepness the same at every scale. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|ErosionFilter")
	float GullyWavelengthGain = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|ErosionFilter")
	float GullyDepthGain = 0.55f;

	/**
	 * Spacing of the stripe pivot lattice as a multiple of the wavelength. Sets
	 * how long a gully runs before it dies out: at 1 a gully is about as long as
	 * it is wide and the slope comes out dimpled rather than grooved.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|ErosionFilter")
	float GullyCellsPerWavelength = 3.0f;

	/** Surface gradient at which gullies cut to full depth; gentler ground gets proportionally less. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|ErosionFilter")
	float GullySlopeReference = 0.35f;

	/** Above 1, fine gullies keep cutting on ground where the coarse ones had already faded out. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|ErosionFilter")
	float GullyDetailPersistence = 1.0f;

	/**
	 * How strongly a gully bends the flow the finer gullies follow, as a
	 * fraction of the slope it is cut into. This is what makes them branch, and
	 * the one dial trading branching against a smooth surface: past roughly 0.3
	 * the bending outruns the pattern and tears it into cliffs.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|ErosionFilter")
	float GullyBranchStrength = 0.25f;

	/** 1 keeps a rounded cosine profile at that end of the gully, 0 pinches it to a straight-sided edge. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|ErosionFilter")
	float GullyRidgeRounding = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|ErosionFilter")
	float GullyCreaseRounding = 0.15f;

	/**
	 * Hydraulic erosion simulation steps; 0 disables the simulated pass
	 * entirely. The erosion filter above does the incising analytically, so the
	 * simulation is off by default and only worth enabling to add deposition.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Erosion")
	int32 HydraulicIterationCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Erosion")
	int32 ThermalIterationCount = 0;

	/** Simulation step in seconds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Erosion")
	float ErosionDeltaTime = 0.15f;

	/** Rainfall in meters of water per cell per second. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Erosion")
	float RainRate = 0.0015f;

	/**
	 * Water depth below which a cell only deposits. Thin sheet flow off bare
	 * rain erodes every cell equally and combs the slopes into rills; requiring
	 * a channel's worth of water confines cutting to real watercourses.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Erosion")
	float MinErosionWaterDepth = 0.02f;

	/** Radius in cells of the erosion brush that spreads each cell's cut. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Erosion")
	int32 ErosionBrushRadiusCells = 3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Erosion")
	float EvaporationRate = 0.015f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Erosion")
	float SedimentCapacityFactor = 3.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Erosion")
	float DissolveRate = 0.3f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Erosion")
	float DepositRate = 0.3f;

	/** Angle of repose for thermal erosion, degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Erosion")
	float TalusAngleDegrees = 34.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Erosion")
	float ThermalRate = 0.5f;

	/** Spring sources feeding rivers; 0 leaves erosion rain-fed only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Rivers")
	int32 RiverSourceCount = 48;

	/** Water injected at each source cell in meters per second. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Rivers")
	float RiverSourceRate = 8.0f;

	/** Radius in cells over which a source spreads its inflow. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Rivers")
	int32 RiverSourceRadiusCells = 3;

	/** Altitude band, as a fraction of the generated relief, where sources may sit. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Rivers")
	float RiverSourceMinAltitudeFraction = 0.55f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Rivers")
	float RiverSourceMaxAltitudeFraction = 0.92f;
};
