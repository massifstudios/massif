#pragma once

#include "CoreMinimal.h"

class FMassifHeightmap;

/** One simulated quantity over the whole world. See ARCHITECTURE §5.6. */
enum class EMassifBakeChannel : uint8
{
	/** Metres of water standing in a hollow: the filled surface less the measured one. */
	StandingWaterDepth,

	/** Square metres of ground draining through the cell, including its own. */
	FlowAccumulation,

	/** Soil moisture, 0 parched to 1 saturated, from the topographic wetness index. */
	Wetness,

	/** Metres of loose material over bedrock, from where droplets dropped what they carried. */
	SoilDepth,

	Count
};

MASSIFWORLDGEN_API const TCHAR* MassifBakeChannelName(EMassifBakeChannel Channel);

/** Unit the channel is measured in, for debug legends. */
MASSIFWORLDGEN_API const TCHAR* MassifBakeChannelUnit(EMassifBakeChannel Channel);

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

	/** Sediment a droplet can hold, per unit of speed, water and downhill drop. */
	double DropletCapacityFactor = 6.0;
	double DropletMinCapacity = 0.005;

	/** Fraction of the shortfall picked up, and of the surplus dropped, per step. */
	double DropletErosionRate = 0.3;
	double DropletDepositionRate = 0.3;

	double DropletEvaporationRate = 0.02;
	double DropletGravity = 10.0;
	double DropletInitialWaterVolume = 1.0;
	double DropletInitialSpeed = 1.0;

	/** Regolith on the ground before any droplet moved anything, and the deepest it gets. */
	double BaseSoilDepthMeters = 0.35;
	double MaxSoilDepthMeters = 3.0;

	/** Wetness index readings that count as parched and as saturated. */
	double DryWetnessIndex = 4.0;
	double SaturatedWetnessIndex = 11.0;

	/** Floor on the slope the wetness index divides by, so flat ground is not infinitely wet. */
	double MinimumSlopeTangent = 0.005;

	/** Iterations the GPU relaxations run for; the CPU path solves exactly and ignores these. */
	int32 GpuDepressionFillIterations = 2048;
	int32 GpuFlowRoutingIterations = 2048;
};

/**
 * Water, and what it left behind, over the whole world:
 *
 *     Sample(Channel, Position) → metres, square metres, or a fraction
 *
 * Solved once at world gen from the imported elevation model and the master
 * seed, on a grid coarser than the model (ARCHITECTURE §5.6). Unlike the
 * terrain field this is not a per-point function — the whole point of it is
 * that water arrives from the entire catchment above a point, which no local
 * evaluation can see. It sits beside the terrain field rather than under it:
 * `Height()` does not read the bake and is unaffected by it.
 *
 * Channels are quantised to uint16 over their own observed range, which holds
 * a 20 km world at 8 m cells in about 50 MB. Flow accumulation spans six
 * orders of magnitude between a hillside and a river, so it is stored
 * logarithmically; the getters still speak square metres.
 *
 * Never serialized: a pure function of the elevation model and the seed, so it
 * is rebuilt at load like the heightmap's mip chain.
 *
 * Thread-safe once built: sampling touches only immutable state.
 */
class MASSIFWORLDGEN_API FMassifEnvironmentBake
{
public:
	/** Solves the whole grid. Uses the GPU path where one is available, the CPU reference otherwise. */
	static TSharedRef<const FMassifEnvironmentBake> Build(
		const FMassifHeightmap& Heightmap, uint32 MasterSeed, const FMassifEnvironmentBakeParams& Params);

	/** Solves on the CPU whatever the hardware offers. The reference the GPU path is held against. */
	static TSharedRef<const FMassifEnvironmentBake> BuildOnCpu(
		const FMassifHeightmap& Heightmap, uint32 MasterSeed, const FMassifEnvironmentBakeParams& Params);

	FMassifEnvironmentBake(const FMassifEnvironmentBake&) = delete;
	FMassifEnvironmentBake& operator=(const FMassifEnvironmentBake&) = delete;

	/** Bilinear across cell centres. Positions outside the world clamp to its border. */
	double Sample(EMassifBakeChannel Channel, FVector2d PositionMeters) const;

	/** One cell, unfiltered. For debug textures and whole-grid passes. */
	double ReadCell(EMassifBakeChannel Channel, int32 Column, int32 Row) const;

	/** Lowest and highest value the solve produced, for normalising debug views. */
	double GetChannelMinimum(EMassifBakeChannel Channel) const;
	double GetChannelMaximum(EMassifBakeChannel Channel) const;

	int32 GetWidth() const
	{
		return Width;
	}

	int32 GetHeight() const
	{
		return Height;
	}

	double GetCellSizeMeters() const
	{
		return CellSizeMeters;
	}

	/** Whether the GPU compute path produced this bake, rather than the CPU reference. */
	bool WasBuiltOnGpu() const
	{
		return bBuiltOnGpu;
	}

	/** Hash over every channel, for the determinism tests. */
	uint64 ComputeContentHash() const;

private:
	FMassifEnvironmentBake() = default;

	/** One channel, quantised over its own observed range. */
	struct FChannel
	{
		TArray<uint16> Samples;
		double MinValue = 0.0;
		double MaxValue = 0.0;
		bool bLogarithmic = false;
	};

	/** Fills one channel from solved floats, measuring the range as it goes. */
	void StoreChannel(EMassifBakeChannel Channel, TArrayView<const float> Values, bool bLogarithmic);

	const FChannel& GetChannel(EMassifBakeChannel Channel) const;
	double DecodeCell(const FChannel& Channel, int32 Column, int32 Row) const;

	FChannel Channels[static_cast<int32>(EMassifBakeChannel::Count)];
	int32 Width = 0;
	int32 Height = 0;
	double CellSizeMeters = 0.0;
	double WorldSizeXMeters = 0.0;
	double WorldSizeYMeters = 0.0;
	bool bBuiltOnGpu = false;
};
