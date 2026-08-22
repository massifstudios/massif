#pragma once

#include "CoreMinimal.h"
#include "MassifRiverNetwork.h"

/**
 * Everything the water solve is tuned by. Iteration limits are counts, never
 * convergence tests: a solve that stops once it has converged stops at a
 * different point on a different machine, and the bake has to be reproducible
 * (ARCHITECTURE §5.6).
 */
struct FMassifEnvironmentBakeParams
{
	double CellSizeMeters = 8.0;

	/**
	 * Drop added per cell as depressions are filled, so a filled lake surface
	 * still descends towards its outflow instead of being a flat that no flow
	 * direction points out of. Small enough that it does not show up as lake
	 * depth: even a thousand-cell flat inflates by a tenth of a millimetre.
	 */
	double FillEpsilonMeters = 0.0000001;

	/**
	 * How sharply flow prefers the steepest of the neighbours below it. 1 spreads
	 * evenly by gradient and washes channels out; large values collapse to single
	 * flow direction and draw drainage as one-cell lines. Freeman's 1.1 keeps
	 * hillsides diffuse and still concentrates in valleys.
	 */
	double FlowConcentrationExponent = 1.1;

	/** Droplets released per bake cell, spread on a jittered lattice. */
	double DropletsPerCell = 1.0;

	int32 MaxDropletSteps = 96;

	/** How much of its old direction a droplet keeps against the gradient under it. */
	double DropletInertia = 0.05;

	/**
	 * Metres of sediment a droplet holds per unit of slope, speed and water.
	 * Capacity is keyed to the slope rather than to the drop across one cell, or
	 * the same terrain would erode differently at a different cell size.
	 */
	double DropletCapacityFactor = 0.05;
	double DropletMinCapacity = 0.001;

	/** Fraction of the shortfall picked up, and of the surplus dropped, per step. */
	double DropletErosionRate = 0.3;
	double DropletDepositionRate = 0.3;

	/** Ceiling on what one droplet takes from one cell, so a single pass cannot cut a hole. */
	double DropletMaxErosionMetersPerStep = 0.02;

	double DropletEvaporationRate = 0.02;
	double DropletGravity = 4.0;
	double DropletMaxSpeed = 4.0;
	double DropletInitialWaterVolume = 1.0;
	double DropletInitialSpeed = 1.0;

	/** Regolith on ground nothing has moved, and the deepest soil gets where it piles up. */
	double BaseSoilDepthMeters = 0.35;
	double MaxSoilDepthMeters = 3.0;

	/**
	 * Net erosion that thins the regolith to a third of itself. Bedrock keeps
	 * weathering into new soil as fast as thin soil exposes it, so ground the
	 * water works at thins towards bare rock instead of reaching it at some
	 * exact amount of scouring — which is also what keeps scree slopes from
	 * collapsing into rock faces the moment they erode at all.
	 */
	double SoilRemovalScaleMeters = 0.6;

	/**
	 * Extra soil held on vegetated ground, as a share of what is there.
	 *
	 * Roots hold the ground that roots need. Reading zero - which is what an
	 * unwritten vegetation channel gives on the first round of the loop - leaves
	 * the soil exactly as the droplets left it.
	 */
	double VegetationSoilRetention = 0.5;

	/** Wetness index readings that count as parched and as saturated. */
	double DryWetnessIndex = 4.0;
	double SaturatedWetnessIndex = 11.0;

	/** Floor on the slope the wetness index divides by, so flat ground is not infinitely wet. */
	double MinimumSlopeTangent = 0.005;

	/** Iterations the GPU relaxations run for; the CPU path solves exactly and ignores these. */
	int32 GpuDepressionFillIterations = 2048;
	int32 GpuFlowRoutingIterations = 2048;

	/** Which of the routed water is a river, and how big. */
	FMassifRiverParams Rivers;
};
