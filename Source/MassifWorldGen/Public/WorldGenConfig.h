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

	/** Spacing of the fluvial bake grid; 128 m over 150 km is ~1.4 M cells and tens of MB. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|FluvialBake")
	float BakeCellSizeMeters = 128.0f;

	/** Solve iterations before the surface is taken as quasi-steady. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|FluvialBake")
	int32 BakeMaxIterationCount = 60;

	/** Largest height change per iteration below which the solve stops early. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|FluvialBake")
	float BakeConvergenceMeters = 0.5f;

	/**
	 * Erodibility and step size folded into one coefficient: the implicit
	 * update uses F = factor * area^exponent / distance. Higher cuts channels
	 * flatter and reaches steady state in fewer iterations.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|FluvialBake")
	float BakeStreamPowerFactor = 0.5f;

	/** Exponent on drained area in the stream power law. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|FluvialBake")
	float BakeDrainageExponent = 0.5f;

	/** Uplift added per iteration on a range axis; basins get none. This is what erosion works against. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|FluvialBake")
	float BakeUpliftMetersPerStep = 30.0f;

	/** 0..1 blend towards the four-neighbour mean each iteration, for softer interfluves. 0 disables. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|FluvialBake")
	float BakeHillslopeSmoothing = 0.15f;

	/**
	 * Fractal relief the solve starts from, as a share of the height between
	 * the uplift ceiling and base level. Zero starts on the bare ceiling,
	 * where flow has nothing to follow but the grid axes and drainage comes
	 * out as an axis-aligned maze.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|FluvialBake")
	float BakeInitialReliefFraction = 0.5f;

	/** Wavelength of the coarsest initial relief octave. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|FluvialBake")
	float BakeInitialReliefWavelengthMeters = 16000.0f;

	/**
	 * Smoothing passes over the solved surface before sampling products are
	 * derived. Takes the D8 zigzag out of the cell-scale channel walls; the
	 * channel carve puts sub-cell sharpness back along the smoothed polylines.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|FluvialBake")
	int32 BakePostSmoothPasses = 2;

	/**
	 * Smooth domain warp on every bake sample, a couple of cell sizes strong.
	 * The solve's channels run along the lattice's eight headings; the warp
	 * bends those straight lines at render time. Zero disables.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|FluvialBake")
	float BakeSampleWarpMeters = 260.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|FluvialBake")
	float BakeSampleWarpWavelengthMeters = 2600.0f;

	/** Drained area above which a cell counts as a channel and joins the river polylines. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|FluvialBake")
	float RiverDrainageAreaThresholdSquareMeters = 4.0e6f;

	/**
	 * Channel cross-section from drained area, hydraulic-geometry style: floor
	 * half width, side fade width and cut depth each scale with sqrt(area)
	 * under a hard cap. The caps bound how far a carve query has to look.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Channels")
	float ChannelWidthPerSqrtArea = 0.004f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Channels")
	float ChannelMaxFloorHalfWidthMeters = 120.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Channels")
	float ChannelSideWidthPerSqrtArea = 0.008f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Channels")
	float ChannelMaxSideWidthMeters = 200.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Channels")
	float ChannelDepthPerSqrtArea = 0.003f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WorldGen|Channels")
	float ChannelNotchDepthCapMeters = 40.0f;

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
