#pragma once

#include "CoreMinimal.h"

class FMassifTerrainField;

/**
 * A box of world the fluid is solved in, and how finely.
 *
 * Bounded rather than global on purpose. Centimetre water over twenty kilometres
 * is not a grid anything can hold; centimetre water over a hundred metres of one
 * river is, and what it produces is geometry that can be stored and drawn. The
 * region is the unit of the bake, and a world's rivers are covered by running
 * many of them.
 */
struct FMassifFluidRegion
{
	/** Lowest corner of the box, in world metres. */
	FVector OriginMeters = FVector::ZeroVector;

	/** How far the box runs from that corner along each axis. */
	FVector ExtentMeters = FVector(12.0, 6.0, 3.0);

	/** Edge of one cell. Five centimetres is the scale a standing wave has shape at. */
	double CellSizeMeters = 0.05;

	/**
	 * Level the region is filled to before the first step.
	 *
	 * Started full rather than filled from the inflow. What is wanted is the shape a
	 * river running through here settles into, and waiting for the water to arrive
	 * spends most of the steps on a wavefront crossing dry ground.
	 */
	double InitialSurfaceAltitudeMeters = 0.0;

	FIntVector CellCounts() const
	{
		return FIntVector(
			FMath::Max(FMath::FloorToInt32(ExtentMeters.X / CellSizeMeters), 1),
			FMath::Max(FMath::FloorToInt32(ExtentMeters.Y / CellSizeMeters), 1),
			FMath::Max(FMath::FloorToInt32(ExtentMeters.Z / CellSizeMeters), 1));
	}
};

/** How the water in a region is solved, and what feeds it. */
struct FMassifFluidBakeParams
{
	/**
	 * Steps of the solve, and how long one of them is.
	 *
	 * A count rather than a settling test (ARCHITECTURE §4). The step is fixed
	 * rather than taken from the fastest particle, so the same region takes the same
	 * steps whatever the water in it happens to be doing.
	 */
	int32 StepCount = 400;
	double StepSeconds = 0.008;

	/** Sweeps of the pressure solve inside one step. Fixed, for the same reason. */
	int32 PressureIterations = 40;

	/**
	 * How much of a particle's new velocity is taken as the grid's change rather
	 * than the grid's value.
	 *
	 * One is pure FLIP, which keeps every eddy and every bit of noise with it; zero
	 * is pure PIC, which is smooth and loses the flow's energy within a few steps.
	 * Near one with a little PIC in it is what keeps the surface alive without the
	 * particles shaking themselves apart.
	 */
	double FlipRatio = 0.95;

	/**
	 * Cells the velocity is carried out past the water into the air around it.
	 *
	 * A face with no particle either side holds nothing, and the pressure solve
	 * reads nothing as a wall, so a free surface with no band around it is held in
	 * by the air it should be free to move into. Three layers is enough for the
	 * stencil a particle at the surface reads across.
	 */
	int32 ExtrapolationLayers = 3;

	/** Particles seeded per cell of water, along each axis of that cell. */
	int32 ParticlesPerCellAxis = 2;

	/** Water fed in across the upstream face of the region, and the depth it arrives at. */
	double InflowSpeedMetersPerSecond = 2.0;
	double InflowDepthMeters = 0.5;

	/** Grains of the bed, as the share of tangential speed a solid face keeps. */
	double BedSlip = 0.4;

	double GravityMetersPerSecondSquared = 9.81;
};

/** What one region's solve settled into, as measures rather than as geometry. */
struct FMassifFluidBakeSummary
{
	int32 ParticleCount = 0;
	FIntVector CellCounts = FIntVector::ZeroValue;
	double WetVolumeCubicMeters = 0.0;
	double MeanDepthMeters = 0.0;
	double MaxDepthMeters = 0.0;
	double MeanSpeedMetersPerSecond = 0.0;
	double MaxSpeedMetersPerSecond = 0.0;
	double SolveSeconds = 0.0;

	/** Where the solve's time went, so a port to the GPU can be aimed rather than guessed. */
	double ScatterSeconds = 0.0;
	double PressureSeconds = 0.0;
	double AdvectSeconds = 0.0;
};

/**
 * Runs water through one region of terrain as particles and keeps where they end
 * up.
 *
 * FLIP on a staggered grid: the particles carry the water and its velocity, the
 * grid is borrowed each step to make that velocity divergence-free, and the
 * change is handed back. That pairing is what gives an incompressible free
 * surface — a body of water that fills a channel, stands against a rock, and
 * breaks over a step — rather than a height per column, which is all a
 * two-dimensional solve can produce however fine it is run.
 *
 * The terrain is a heightfield, so a cell is solid exactly when its centre is
 * under the ground, and a particle is put back on the surface it crossed rather
 * than tested against a mesh.
 *
 * Deterministic: fixed step count, fixed step length, fixed pressure sweeps, and
 * particles seeded on the grid's own lattice rather than scattered.
 */
MASSIFWORLDGEN_API FMassifFluidBakeSummary MassifBakeFluidRegion(
	const FMassifTerrainField& Terrain,
	const FMassifFluidRegion& Region,
	const FMassifFluidBakeParams& Params,
	TArray<FVector3f>& OutParticlePositionsMeters,
	TArray<FVector3f>& OutParticleVelocitiesMetersPerSecond);
