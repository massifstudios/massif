#pragma once

#include "CoreMinimal.h"

/**
 * Everything the river extraction is tuned by. Rivers are read out of the water
 * solve the environment bake already ran (ARCHITECTURE §5.6), so nothing here
 * changes where water goes — only which of it is a river and how big.
 */
struct FMassifRiverParams
{
	/**
	 * Catchment a channel needs before it counts as one. Below it water still
	 * flows, it just flows as a wet hillside rather than as a stream with a bed.
	 * A square kilometre is the scale a permanent alpine brook starts at; halving
	 * it roughly doubles the length of channel in the world.
	 */
	double ChannelInitiationAreaSquareMeters = 1.0e6;

	/**
	 * Metres per second of runoff each square metre of catchment yields, averaged
	 * over the year. The Ötztaler Ache carries about 26 m3/s off 800 km2, which is
	 * this figure; it stands in for precipitation, evaporation and glacier melt
	 * together, none of which the bake models separately.
	 */
	double SpecificRunoffMetersPerSecond = 3.2e-8;

	/**
	 * Downstream hydraulic geometry (Leopold & Maddock 1953): width, depth and
	 * speed are each a power of discharge. The exponents sum to one and the
	 * coefficients multiply to one, which is continuity — width times depth times
	 * speed has to come back to discharge at every point.
	 *
	 * Coefficients are set for a steep alpine gravel-bed river: at the Ache's
	 * 26 m3/s they give a channel 25 m wide, 0.9 m deep, running at 1.2 m/s.
	 */
	double WidthCoefficient = 4.90;
	double WidthExponent = 0.5;
	double DepthCoefficient = 0.241;
	double DepthExponent = 0.4;
	double SpeedCoefficient = 0.847;
	double SpeedExponent = 0.1;

	/** Narrowest and shallowest a channel is drawn, so a headwater brook is still visible. */
	double MinWidthMeters = 0.8;
	double MinDepthMeters = 0.15;

	/**
	 * Shallowest trench the terrain is cut to under a channel.
	 *
	 * The elevation model is an airborne laser scan and water returns no usable
	 * echo, so a river reads in the data as a flat ribbon at its own surface
	 * level: the measured altitude along a channel is the top of the water, not
	 * the bottom. The bed has to be put back, and this is the least of it a
	 * headwater brook gets, so that even the smallest channel has a column of
	 * water in it rather than a sheet lying on the ground.
	 */
	double MinCarveDepthMeters = 0.30;

	/**
	 * How far a node may be moved by dropping it from the polyline, as a share of
	 * the channel's own width. One node per bake cell over a world's worth of
	 * channel is far more geometry than the shape needs, and a fixed tolerance
	 * either strips a brook of its wiggle or leaves the trunk carrying nodes that
	 * sit well inside its own banks.
	 */
	double SimplificationWidthFraction = 0.15;
	double MinSimplificationToleranceMeters = 0.5;
	double MaxSimplificationToleranceMeters = 4.0;

	/**
	 * Spacing the simplified polyline is resampled at, through a spline.
	 *
	 * Routing picks one of eight neighbours per cell, so a raw channel is a
	 * staircase that turns in 45 degree steps, and simplifying it only replaces
	 * the staircase with the long straight chords between its corners. Curving the
	 * line back through those corners is what makes a river read as a river rather
	 * than as a run of fenceposts.
	 */
	double SmoothingSpacingMeters = 6.0;
};

/** One point along a channel, with the channel's shape where it stands. */
struct FMassifRiverNode
{
	FVector2d PositionMeters = FVector2d::ZeroVector;

	/** The elevation model's own altitude here, which along a channel is the top of the water. */
	double GroundAltitudeMeters = 0.0;

	/**
	 * The channel invert: the water surface less the depth the catchment fills it
	 * to. This is what the terrain is cut down to, and it is always far enough
	 * below the water surface to leave a column with something in it.
	 */
	double BedAltitudeMeters = 0.0;

	/** Where the water's top sits. Never rises going downstream. */
	double WaterSurfaceMeters = 0.0;

	double DischargeCubicMetersPerSecond = 0.0;
	double WidthMeters = 0.0;
	double DepthMeters = 0.0;
	double SpeedMetersPerSecond = 0.0;
};

/**
 * A run of channel between two junctions: from a source or a confluence, down
 * to the next confluence or off the edge of the world. Reaches share their end
 * points with the reaches they meet, so the set of them is one connected
 * network rather than a pile of loose lines.
 */
struct FMassifRiverReach
{
	/** Upstream first, downstream last. Always at least two nodes. */
	TArray<FMassifRiverNode> Nodes;

	/** Strahler order: 1 is a headwater, and two equal orders meeting make the next one up. */
	int32 StrahlerOrder = 1;

	/** Whether this reach begins at a channel head rather than where two others meet. */
	bool bStartsAtSource = false;
};

/**
 * The rivers of the world, traced out of the environment bake's own drainage:
 * channel heads where enough catchment has gathered, then the path water takes
 * from each of them down to the world border.
 *
 * Discharge comes from the catchment draining through a point, and width, depth
 * and speed from discharge by hydraulic geometry — so the network is a
 * consequence of the water solve rather than a second guess at it.
 *
 * Never serialized: like the bake it is a pure function of the elevation model
 * and the master seed, and is rebuilt at load.
 *
 * Thread-safe once built: reading touches only immutable state.
 */
class MASSIFWORLDGEN_API FMassifRiverNetwork
{
public:
	/** Takes ownership of extracted reaches and measures the totals over them. */
	static TSharedRef<const FMassifRiverNetwork> Create(TArray<FMassifRiverReach>&& InReaches);

	FMassifRiverNetwork(const FMassifRiverNetwork&) = delete;
	FMassifRiverNetwork& operator=(const FMassifRiverNetwork&) = delete;

	const TArray<FMassifRiverReach>& GetReaches() const
	{
		return Reaches;
	}

	/** Reaches that begin at a channel head: where a river starts. */
	int32 GetSourceCount() const
	{
		return SourceCount;
	}

	double GetTotalLengthMeters() const
	{
		return TotalLengthMeters;
	}

	/** Discharge and width of the largest channel in the world, for logs and debug legends. */
	double GetMaxDischargeCubicMetersPerSecond() const
	{
		return MaxDischargeCubicMetersPerSecond;
	}

	double GetMaxWidthMeters() const
	{
		return MaxWidthMeters;
	}

	int32 GetMaxStrahlerOrder() const
	{
		return MaxStrahlerOrder;
	}

	/** Hash over every node, for the determinism tests. */
	uint64 ComputeContentHash() const;

private:
	FMassifRiverNetwork() = default;

	void MeasureTotals();

	TArray<FMassifRiverReach> Reaches;
	int32 SourceCount = 0;
	int32 MaxStrahlerOrder = 0;
	double TotalLengthMeters = 0.0;
	double MaxDischargeCubicMetersPerSecond = 0.0;
	double MaxWidthMeters = 0.0;
};
