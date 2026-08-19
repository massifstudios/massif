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
	 * Lattice the river field is evaluated on before being interpolated up to
	 * the heightfield. The field varies over hundreds of meters, so it does not
	 * need the full grid.
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

	/** How far the ground rises above the valley floor before it levels off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float ValleyReliefMeters = 900.0f;

	/** Horizontal scale of the valley side on a trunk river; smaller is steeper. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float TrunkValleyWidthMeters = 1200.0f;

	/** The same for a headwater stream, which only notches the slope it runs down. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float HeadwaterValleyWidthMeters = 420.0f;

	/**
	 * Ground keeps climbing this much per meter away from a watercourse even
	 * after the valley side has levelled off. Without it the interfluves come
	 * out as plateaus at one fixed height.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float InterfluveRiseSlope = 0.02f;

	/** Highest a watercourse may start, as a fraction of the altitude range. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float RiverMaxSourceAltitudeFraction = 0.6f;

	/** Rounding of the crest where two catchments meet, meters. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float RidgeBlendMeters = 450.0f;

	/** Ridge features per map side. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float RidgeFrequency = 6.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	int32 RidgeOctaveCount = 4;

	/** Ridge crest profile: 1.0 soft and rounded, 2.0 knife edges. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float RidgeSharpness = 1.0f;

	/** How much the ridge noise pushes the crests up and down, meters. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float RidgeAmplitudeMeters = 400.0f;

	/** Medium-scale surface relief features per map side (spurs, gullies, knolls). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float DetailFrequency = 24.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float DetailAmplitudeMeters = 45.0f;

	/** Sideways displacement of the query point, meters; bends the valleys off the graph's straight segments. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float TerrainWarpMeters = 600.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float TerrainWarpFrequency = 5.0f;

	/** Fraction of the map edge over which height falls off to keep borders low. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float EdgeFalloffFraction = 0.12f;

	/** Hydraulic erosion simulation steps; 0 disables erosion entirely. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Erosion")
	int32 HydraulicIterationCount = 1500;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Erosion")
	int32 ThermalIterationCount = 500;

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
