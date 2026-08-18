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
	float MaxAltitudeMeters = 3400.0f;

	/** Broad uplift features per map side; low values give few large massifs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float UpliftFrequency = 1.5f;

	/** Ridge features per map side. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float RidgeFrequency = 3.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	int32 RidgeOctaveCount = 4;

	/** Ridge crest profile: 1.0 soft and rounded, 2.0 knife edges. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float RidgeSharpness = 1.0f;

	/** Medium-scale surface relief features per map side (spurs, gullies, knolls). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float DetailFrequency = 24.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float DetailAmplitudeMeters = 70.0f;

	/** Domain warp displacement as a fraction of map size. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float WarpStrength = 0.08f;

	/** Fraction of the map edge over which height falls off to keep borders low. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Noise")
	float EdgeFalloffFraction = 0.12f;
};
