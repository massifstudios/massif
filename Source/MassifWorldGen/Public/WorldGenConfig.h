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

	/** Side of the world square. Finite, and playable throughout. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen")
	float WorldSizeMeters = 150000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen")
	float MinAltitudeMeters = 300.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen")
	float MaxAltitudeMeters = 4800.0f;

	/** Distance between range axes. The largest thing in the world. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Uplift")
	float RangeSpacingMeters = 42000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Uplift")
	int32 RangeOctaveCount = 4;

	/** Range axis profile: 1.0 broad and rounded, 2.0 narrow and sharp. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Uplift")
	float RangeSharpness = 1.1f;

	/** Sideways displacement of the range pattern, so ranges bend and throw off spurs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Uplift")
	float RangeWarpWavelengthMeters = 60000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Uplift")
	float RangeWarpMeters = 12000.0f;

	/** Altitude the crests reach on a basin floor, and on a range axis. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Uplift")
	float BasinCrestAltitudeMeters = 900.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Uplift")
	float PeakCrestAltitudeMeters = 4300.0f;

	/**
	 * Ridged relief added to the crest altitude inside the ranges, so summits
	 * are peaks and cols instead of the smooth domes the range-scale noise
	 * makes on its own. Ground reaches the crest altitude wherever it stands
	 * far from every big watercourse — without structure at peak scale, every
	 * such place tops off flat.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Uplift")
	float PeakReliefMeters = 300.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Uplift")
	float PeakWavelengthMeters = 4500.0f;

	/**
	 * Ground a watercourse must keep above it. Headwaters may not climb closer
	 * than this to the local crest, which is what keeps drainage inside the
	 * ranges that contain it instead of running over the top of them.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Uplift")
	float MinReliefAboveWaterMeters = 500.0f;

	/** Watercourses reaching the world border, each the outlet of one catchment. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	int32 RiverMouthCount = 28;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float RiverMouthSpacingMeters = 9000.0f;

	/**
	 * Distance between nodes on a trunk, and up in the headwaters. Segments
	 * shorten as a watercourse climbs, so one graph carries both the main
	 * valleys and the streams that feed them.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float TrunkSegmentLengthMeters = 3200.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float HeadwaterSegmentLengthMeters = 380.0f;

	/**
	 * Strength a main stem keeps per step upstream, and the share of it a
	 * tributary starts with. The stem holds together over tens of kilometres
	 * while every tributary drops straight to a smaller, steeper watercourse.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float MainStemStrengthRetention = 0.988f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float TributaryStrengthFactor = 0.55f;

	/**
	 * Share of the remaining headroom under the local crest a watercourse may
	 * take in one step. Below 1 it eases in towards that ceiling instead of
	 * running into it, which is what lets rivers cross low basins flat and still
	 * climb hard through ranges.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float RiverHeadroomClimbFraction = 0.25f;

	/** Watercourses may not come closer to each other than this, which sets valley spacing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float RiverMinNodeSpacingMeters = 300.0f;

	/** Chance a tributary joins at a node. The stem always continues regardless. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float TributaryProbability = 0.42f;

	/** Heading change allowed between successive nodes, degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float RiverTurnDegrees = 26.0f;

	/** Angle a tributary leaves the stem at, degrees. Wide, so junctions read as joins rather than forks. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float TributaryJoinDegrees = 52.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	int32 RiverMaxNodeCount = 220000;

	/** Valley floor gradient just above a mouth. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float RiverSlopeNearMouth = 0.004f;

	/** Valley floor gradient in the headwaters. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float RiverSlopeHeadwater = 0.13f;

	/**
	 * Horizontal scale of the valley side on a trunk river, and on a headwater
	 * stream. Relief itself is not configured here: a valley side climbs towards
	 * the crest altitude the uplift field sets overhead, so the same river cuts
	 * a shallow trough in a basin and a deep valley through a range.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float TrunkValleyWidthMeters = 2200.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float HeadwaterValleyWidthMeters = 950.0f;

	/** Flat ground either side of the water before the valley side starts to climb. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float TrunkFloorHalfWidthMeters = 260.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float HeadwaterFloorHalfWidthMeters = 15.0f;

	/**
	 * Discharge fraction above which a watercourse defines the mountainside,
	 * proposing a valley side that climbs to the crest. Everything below it
	 * only cuts a notch into that surface. Letting every stream claim the full
	 * relief dissects the ground uniformly at the node spacing: the ridge
	 * between two adjacent headwaters is barely above the streams, and the
	 * whole massif reads as knife-edges.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float BackboneDischargeFraction = 0.35f;

	/** Deepest cut a small stream makes below the mountainside it sits on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float StreamNotchDepthMeters = 100.0f;

	/**
	 * Ground keeps climbing this much per meter away from a watercourse even
	 * after the valley side has levelled off. Without it the interfluves come
	 * out as plateaus at one fixed height.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float InterfluveRiseSlope = 0.01f;

	/** Rounding of the crest where two catchments meet, meters. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Hydrology")
	float RidgeBlendMeters = 400.0f;

	/** Gully scales the erosion filter cuts, coarsest first; 0 leaves the macro surface bare. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|ErosionFilter")
	int32 GullyOctaveCount = 6;

	/** Spacing of the coarsest gullies. Keep it under the valley spacing or gullies compete with valleys. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|ErosionFilter")
	float LargestGullyWavelengthMeters = 220.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|ErosionFilter")
	float LargestGullyDepthMeters = 9.0f;

	/** Wavelength and depth left per octave. Equal values keep gully steepness the same at every scale. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|ErosionFilter")
	float GullyWavelengthGain = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|ErosionFilter")
	float GullyDepthGain = 0.5f;

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
	 * How far a gully drags the finer gullies sideways, as a fraction of its own
	 * wavelength. This is what makes them branch off it instead of running
	 * parallel to it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|ErosionFilter")
	float GullyBranchStrength = 0.25f;

	/** 1 keeps a rounded cosine profile at that end of the gully, 0 pinches it to a straight-sided edge. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|ErosionFilter")
	float GullyRidgeRounding = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|ErosionFilter")
	float GullyCreaseRounding = 0.15f;

	/**
	 * Window the whole-array path materialises out of the terrain field, centred
	 * on the world origin. Scaffolding until M5 replaces it with clipmap rings
	 * sampled directly; the field itself has no resolution.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Preview")
	float PreviewWindowSizeMeters = 20000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Preview")
	int32 Resolution = 4096;

	/**
	 * Hydraulic erosion simulation steps; 0 disables the simulated pass
	 * entirely. Incising is analytic now, and a pass that needs the whole grid
	 * and its neighbours cannot be part of a field evaluated point by point, so
	 * this stays off and nothing depends on it.
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

	/** Water injected at each spring cell feeding the simulated pass, meters per second. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Erosion")
	float RiverSourceRate = 8.0f;

	/** Radius in cells over which a source spreads its inflow. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Erosion")
	int32 RiverSourceRadiusCells = 3;
};
