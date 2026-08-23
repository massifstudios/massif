#pragma once

#include "CoreMinimal.h"

class IMassifElevationSource;
struct FMassifRiverReach;

/**
 * Offsets one cross-section is sampled at, both banks and the centreline
 * between them.
 *
 * Odd, so one sample is always midstream. Fixed rather than chosen per channel
 * because a node carries its profile inline: a world's worth of channel is tens
 * of thousands of stations, and a container each would be tens of thousands of
 * allocations holding thirty-three floats.
 */
inline constexpr int32 MassifChannelProfileOffsetCount = 33;

/**
 * One cross-section through a channel, and what the discharge does in it.
 *
 * The shape is the ground's, sampled from the elevation model across the flow.
 * Width and depth then come from fitting the water into that shape instead of
 * from a power law of the discharge alone — two reaches carrying the same water
 * run deep and narrow through a gorge and shallow and wide over a fan, and only
 * the cross-section knows which one this is.
 */
struct FMassifChannelProfile
{
	/** Half the span the offsets cover, which is wider than the water. */
	double HalfSpanMeters = 0.0;

	/** Ground across the flow, left bank first, as the elevation model has it. */
	float GroundAltitudeMeters[MassifChannelProfileOffsetCount] = {};

	/** Where the water reaches across the section, as offset indices with a fraction either side. */
	double WettedWidthMeters = 0.0;

	/** Deepest point of the water over the bed cut for it. */
	double DepthMeters = 0.0;

	/** Cross-sectional area the discharge passes through, and how fast it has to move to fit. */
	double FlowAreaSquareMeters = 0.0;
	double SpeedMetersPerSecond = 0.0;

	/** Speed against wave speed. Over one the flow is supercritical, which is what breaks white. */
	double FroudeNumber = 0.0;

	/** The energy slope the depth was solved against, floored where the surface is level. */
	double EnergySlope = 0.0;

	double OffsetMeters(int32 OffsetIndex) const
	{
		const double Fraction = 2.0 * OffsetIndex / (MassifChannelProfileOffsetCount - 1) - 1.0;
		return Fraction * HalfSpanMeters;
	}

	double OffsetSpacingMeters() const
	{
		return 2.0 * HalfSpanMeters / (MassifChannelProfileOffsetCount - 1);
	}
};

/** How the cross-sections are taken, and what the water does in them. */
struct FMassifChannelSolveParams
{
	/**
	 * How far across the profile reaches, as a multiple of the width the discharge
	 * alone predicts. Wider than the water, or the solve has no bank to stop at and
	 * the wetted extent is whatever the section happened to be cut off at.
	 */
	double ProfileSpanFactor = 4.0;

	/** Narrowest span a profile covers, so a headwater brook still resolves its own banks. */
	double MinProfileSpanMeters = 6.0;

	/**
	 * Manning's n: a steep alpine gravel bed with boulders standing in it.
	 *
	 * One figure for every channel in the world for now. It belongs on the bed
	 * material, which `Geology.RockHardness` and the scree production are already
	 * solving somewhere else, and wiring the two together is its own change.
	 */
	double Roughness = 0.045;

	/**
	 * Nodes either side the energy slope is measured over.
	 *
	 * The surface is clamped monotone downstream and stored to four centimetres, so
	 * the drop between two adjacent stations is mostly quantisation. A window long
	 * enough to leave that behind is what stops the depth solve reading a staircase
	 * as a run of alternating pools and rapids.
	 */
	int32 SlopeWindowNodes = 4;

	/**
	 * Floor on the energy slope. A reach the monotone clamp left exactly level has
	 * no slope at all, and Manning at zero slope carries no water at any depth.
	 */
	double MinEnergySlope = 1.0e-3;

	/**
	 * Band the wetted width is held inside, as a multiple of the hydraulic-geometry
	 * width. A guard rather than a model: a section with no channel in it reads as
	 * either the whole valley floor or a single cell, and neither should be allowed
	 * to become a river. Cutting the channel that such a section is missing is a
	 * later milestone's job.
	 */
	double MinWettedWidthFactor = 0.25;
	double MaxWettedWidthFactor = 4.0;

	/** Deepest the bed is cut below the section, as a multiple of the hydraulic-geometry depth. */
	double MaxDepthFactor = 6.0;

	/**
	 * Bisection steps for the depth. A count rather than a tolerance: a solve that
	 * stops once it is satisfied stops somewhere else on another machine
	 * (ARCHITECTURE §4).
	 */
	int32 DepthIterations = 40;

	/**
	 * Shallowest water drawn.
	 *
	 * The terrain is cut to this depth whatever the section says, so even a channel
	 * the ground could carry dry has a column of water in it rather than a sheet
	 * lying on the ground with nothing underneath for the shading to integrate
	 * through.
	 */
	double MinDepthMeters = 0.30;
};

/**
 * Fills in every node's cross-section, and the depth and width the discharge
 * makes of it.
 *
 * Reads the elevation model directly rather than the bake's 8 m altitude
 * channel: a channel five metres wide is one cell there, and a section across it
 * would be three samples of the valley it sits in.
 *
 * The water surface each node already carries is left alone. Along a channel the
 * elevation model's own altitude is the top of the water, so the surface is
 * measured and the depth is what has to be put back underneath it — solving a new
 * surface on top of the measured one would stack one column on another.
 *
 * Deterministic and thread-safe over reaches: a reach's section depends on the
 * elevation model and on that reach alone.
 */
MASSIFWORLDGEN_API void MassifSolveChannelProfiles(
	const IMassifElevationSource& Elevation,
	const FMassifChannelSolveParams& Params,
	TArray<FMassifRiverReach>& InOutReaches);
