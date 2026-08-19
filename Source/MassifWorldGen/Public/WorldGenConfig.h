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

	/** Broad uplift features per map side; low values give few large massifs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float UpliftFrequency = 2.2f;

	/** Direction the mountain chains run, degrees counter-clockwise from +X. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float RangeAxisDegrees = 25.0f;

	/** Along-axis frequency scale; below 1 stretches uplift into long chains. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float RangeElongation = 0.4f;

	/** Uplift below this level drops to the valley floor, separating chains. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float MassifThreshold = 0.20f;

	/** Regional height variation features per map side. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float RegionFrequency = 1.2f;

	/** How much regional variation lowers the weaker parts of the range. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float RegionContrast = 0.25f;

	/** Ridge features per map side. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float RidgeFrequency = 6.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	int32 RidgeOctaveCount = 4;

	/** Ridge crest profile: 1.0 soft and rounded, 2.0 knife edges. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float RidgeSharpness = 1.0f;

	/** Medium-scale surface relief features per map side (spurs, gullies, knolls). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float DetailFrequency = 24.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float DetailAmplitudeMeters = 45.0f;

	/** How far ridged relief cuts down between crests; 0 keeps massifs flat-topped, 1 drops valleys to the floor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float ValleyDepth = 0.8f;

	/** Main valley threads per map side; these run continuously through the range. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float ValleyNetworkFrequency = 2.6f;

	/** How deeply the valley network cuts into the massifs, 0 to 1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float ValleyNetworkStrength = 0.4f;

	/** Valley thread width; above 1 narrows them to gorges. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float ValleyNetworkSharpness = 1.6f;

	/** Slope profile exponent; above 1 keeps low ground gentle and steepens towards the summits. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float SlopeProfileExponent = 1.4f;

	/** Domain warp displacement as a fraction of map size. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float WarpStrength = 0.08f;

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
