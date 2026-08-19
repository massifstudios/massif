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
	float WorldSizeMeters = 20000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen")
	float MinAltitudeMeters = 500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen")
	float MaxAltitudeMeters = 3400.0f;

	/** Distance between range axes. The largest thing in the world. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Uplift")
	float RangeSpacingMeters = 14000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Uplift")
	int32 RangeOctaveCount = 4;

	/** Range axis profile: 1.0 broad and rounded, 2.0 narrow and sharp. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Uplift")
	float RangeSharpness = 1.1f;

	/** Sideways displacement of the range pattern, so ranges bend and throw off spurs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Uplift")
	float RangeWarpWavelengthMeters = 9000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Uplift")
	float RangeWarpMeters = 2200.0f;

	/** Altitude the crests reach on a basin floor, and on a range axis. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Uplift")
	float BasinCrestAltitudeMeters = 800.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Uplift")
	float PeakCrestAltitudeMeters = 3000.0f;

	/**
	 * Ridged relief added to the crest altitude inside the ranges, so summits
	 * are peaks and cols instead of the smooth domes the range-scale noise
	 * makes on its own.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Uplift")
	float PeakReliefMeters = 300.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Uplift")
	float PeakWavelengthMeters = 4500.0f;

	/**
	 * Coarsest wavelength of the fractal base relief under the erosion filter.
	 * This is where the valleys between the uplift ranges come from; the
	 * filter cuts everything below it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|BaseRelief")
	float BaseReliefWavelengthMeters = 6000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|BaseRelief")
	int32 BaseReliefOctaveCount = 4;

	/** Gully scales the erosion filter cuts, coarsest first; 0 leaves the base surface bare. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|ErosionFilter")
	int32 GullyOctaveCount = 10;

	/**
	 * Spacing of the coarsest gullies. The filter is the terrain below the base
	 * relief, so this starts near the base wavelength and the octave stack
	 * carries the surface down to walking scale.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|ErosionFilter")
	float LargestGullyWavelengthMeters = 1800.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|ErosionFilter")
	float LargestGullyDepthMeters = 140.0f;

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
};
