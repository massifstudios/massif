#pragma once

#include "CoreMinimal.h"

class FMassifRiverField;
class FMassifTerrainField;

/**
 * A patch of world the water is solved over, and how finely.
 *
 * Two dimensional, and that is what makes it a patch of a river rather than a
 * puddle. A three dimensional solve fine enough to hold a standing wave costs
 * the volume over the cube of the cell, which buys a box a few metres across; a
 * depth per column at the same spacing costs the area over its square, and buys
 * a couple of hundred metres of channel. What the shallow water equations give
 * up is exactly what a river surface does not do — overturn, break, and enclose
 * air — and what they keep is what it does: run faster where it is squeezed,
 * stand up over a bar, pile against the outside of a bend, and pull down into
 * the eddy behind a rock.
 */
struct FMassifShallowWaterRegion
{
	/** Lowest corner of the patch, in world metres. */
	FVector2d OriginMeters = FVector2d::ZeroVector;

	FVector2d ExtentMeters = FVector2d(160.0, 80.0);

	/** Edge of one cell. A quarter metre is the scale a standing wave has shape at. */
	double CellSizeMeters = 0.25;
};

/** How the water over that patch is solved. */
struct FMassifShallowWaterParams
{
	/**
	 * Steps of the solve, and how long one of them is.
	 *
	 * A count rather than a settling test (ARCHITECTURE §4), and a fixed step
	 * rather than one taken from the fastest cell, so the same patch takes the
	 * same steps whatever the water in it happens to be doing.
	 *
	 * The patch is seeded at the surface the channel solve already put there, so
	 * these steps are spent developing the detail that solve cannot hold rather
	 * than filling a dry bed.
	 */
	int32 StepCount = 1200;
	double StepSeconds = 0.02;

	double GravityMetersPerSecondSquared = 9.81;

	/** Manning's n for the bed, matching the roughness the channel solve fitted against. */
	double Roughness = 0.045;

	/**
	 * Shallowest water that counts as wet.
	 *
	 * Below it a cell is a damp patch of gravel rather than part of the river, and
	 * drawing it puts a sheet of surface over ground that has nothing on it.
	 */
	double MinWetDepthMeters = 0.02;

	/**
	 * Ceiling on how fast one face may run.
	 *
	 * A guard, not a model. The scheme is explicit, so a cell that is momentarily
	 * a centimetre deep under a metre of head solves for a speed no water reaches
	 * and takes its neighbours with it on the step after.
	 */
	double MaxSpeedMetersPerSecond = 12.0;
};

/** Depth and velocity over the patch, as the solve left them. */
struct FMassifShallowWaterField
{
	FIntPoint CellCounts = FIntPoint::ZeroValue;
	double CellSizeMeters = 0.25;
	FVector2d OriginMeters = FVector2d::ZeroVector;

	/** The ground under each cell: the terrain with the channel already cut into it. */
	TArray<float> BedAltitudeMeters;

	TArray<float> DepthMeters;
	TArray<FVector2f> VelocityMetersPerSecond;

	int32 CellIndex(int32 Column, int32 Row) const
	{
		return Row * CellCounts.X + Column;
	}

	bool Contains(int32 Column, int32 Row) const
	{
		return Column >= 0 && Row >= 0 && Column < CellCounts.X && Row < CellCounts.Y;
	}

	/** Low corner of a cell, which is also the lattice node the mesh is built on. */
	FVector2d NodePositionMeters(int32 Column, int32 Row) const
	{
		return OriginMeters + FVector2d(Column, Row) * CellSizeMeters;
	}

	double SurfaceAltitudeMeters(int32 CellIndex) const
	{
		return static_cast<double>(BedAltitudeMeters[CellIndex]) + DepthMeters[CellIndex];
	}
};

/** What one patch settled into, as measures rather than as geometry. */
struct FMassifShallowWaterSummary
{
	FIntPoint CellCounts = FIntPoint::ZeroValue;
	int32 WetCellCount = 0;
	double WetAreaSquareMeters = 0.0;
	double MeanDepthMeters = 0.0;
	double MaxDepthMeters = 0.0;
	double MeanSpeedMetersPerSecond = 0.0;
	double MaxSpeedMetersPerSecond = 0.0;
	double SolveSeconds = 0.0;
};

/**
 * Runs water over one patch of terrain until it settles, and keeps the depth and
 * velocity it settled to.
 *
 * Shallow water on a staggered grid, as a network of pipes between cells: a face
 * carries what the difference in water surface either side of it drives through
 * it, no cell may send away more than it holds, and the bed's roughness takes
 * its share back. Depth follows from what arrives less what leaves, which is
 * conservation rather than an approximation of it — a patch neither gains nor
 * loses water except across its own boundary.
 *
 * Seeded from the channel solve's own surface, and held at it one cell in from
 * the boundary. That is what makes the patch a window onto a river rather than a
 * tank: water arrives at the upstream edge at the level the reach delivers it,
 * leaves at the downstream edge at the level the reach below expects, and what
 * happens between them is the solve's own.
 *
 * Deterministic: fixed step count, fixed step length, and every pass writes only
 * the cell it is reading for, so the grid may be walked in any order and split
 * across any number of threads.
 */
MASSIFWORLDGEN_API FMassifShallowWaterSummary MassifBakeShallowWater(
	const FMassifTerrainField& Terrain,
	const FMassifRiverField& Rivers,
	int32 TimeSlice,
	const FMassifShallowWaterRegion& Region,
	const FMassifShallowWaterParams& Params,
	FMassifShallowWaterField& OutField);

/** A water surface as triangles, in world metres. */
struct FMassifShallowWaterMesh
{
	TArray<FVector3f> PositionsMeters;
	TArray<FVector3f> Normals;

	/** Where across the channel and how fast, for the material: depth and speed as fractions. */
	TArray<FVector2f> DepthAndSpeed;

	TArray<FIntVector> Triangles;
	double BuildSeconds = 0.0;
};

/**
 * Meshes the surface as a height over the patch's own lattice.
 *
 * A height field, because that is what the solve produced. Nothing here has to
 * find an isosurface or decide which side of it a corner is on: a node's
 * altitude is the bed under it plus the water over it, and the sheet ends where
 * the water runs out.
 *
 * A node takes the mean of the cells around it counting dry ones as nothing, so
 * the sheet thins to the ground at its own edge instead of standing a wall of
 * water there.
 */
MASSIFWORLDGEN_API FMassifShallowWaterMesh MassifBuildShallowWaterSurface(
	const FMassifShallowWaterField& Field, const FMassifShallowWaterParams& Params);
