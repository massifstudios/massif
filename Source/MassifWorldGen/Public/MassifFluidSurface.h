#pragma once

#include "CoreMinimal.h"
#include "MassifFluidBake.h"

/**
 * How the particles are turned into a surface.
 *
 * The particles are the water; a surface is what can be drawn. Each one is read
 * as a blob, the blobs are summed into a field, and the sheet where that field
 * crosses zero is the water's skin. Built at the solve's own cell size, so the
 * mesh resolves what the solve resolved and no finer.
 */
struct FMassifFluidSurfaceParams
{
	/** How far a particle reaches into the field, as a multiple of the cell. */
	double KernelCells = 2.0;

	/** Spacing the particles were seeded at, which is what sets how big a blob one is. */
	double ParticleSpacingMeters = 0.025;

	/**
	 * Radius a particle stands for, as a multiple of that spacing.
	 *
	 * At one the blobs of neighbouring particles just meet, which closes a body of
	 * water without inflating it. Below that a full channel reads as gravel; well
	 * above it, a thin sheet reads as a slab.
	 */
	double ParticleRadiusFactor = 1.0;

	/**
	 * Passes of smoothing over the field before it is meshed.
	 *
	 * A surface built from blobs carries the lattice the particles were seeded on,
	 * which draws as a regular bumpiness that is not water. Two passes take that out
	 * and leave the shape the flow actually made.
	 */
	int32 SmoothingPasses = 2;
};

/** The water's skin, as triangles in world metres. */
struct FMassifFluidSurfaceMesh
{
	TArray<FVector3f> PositionsMeters;
	TArray<FVector3f> Normals;
	TArray<FIntVector> Triangles;
	double BuildSeconds = 0.0;
};

/**
 * Builds the surface of the water a region's solve settled into.
 *
 * A field is gathered at every grid node from the particles near it, taken as the
 * distance from the node to the mean of those particles less the radius they
 * stand for, and the zero crossing of that field is meshed.
 *
 * Threaded and not order-independent: this is geometry derived from the
 * particles rather than state anything else reads, so the determinism the solve
 * holds to (ARCHITECTURE §4) does not bind it and the passes are free to run
 * however they finish fastest.
 */
MASSIFWORLDGEN_API FMassifFluidSurfaceMesh MassifBuildFluidSurface(
	const FMassifFluidRegion& Region,
	const FMassifFluidSurfaceParams& Params,
	TArrayView<const FVector3f> ParticlePositionsMeters);
