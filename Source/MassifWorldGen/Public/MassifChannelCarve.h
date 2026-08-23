#pragma once

#include "CoreMinimal.h"
#include "MassifEnvironmentProcess.h"

struct FMassifRiverReach;

/**
 * What a channel is cutting into, wherever it is cutting.
 *
 * Its own interface for the same reason terrain has one: the carve must work on
 * a world with no lithology solved for it, and must not learn where hardness
 * comes from when there is. Pure function of position — read from many threads
 * at once.
 */
class MASSIFWORLDGEN_API IMassifBedMaterialSource
{
public:
	virtual ~IMassifBedMaterialSource() = default;

	/** 0 is loose valley fill a river reworks in a season, 1 is rock it barely touches. */
	virtual double SampleRockHardness(FVector2d PositionMeters) const = 0;
};

/** One hardness everywhere: what a world with no lithology in it is cut through. */
class MASSIFWORLDGEN_API FMassifUniformBedMaterial final : public IMassifBedMaterialSource
{
public:
	explicit FMassifUniformBedMaterial(double InRockHardness = 0.0);

	double SampleRockHardness(FVector2d PositionMeters) const override;

private:
	double RockHardness = 0.0;
};

/** How the bed under a channel is worked, and by what. */
struct FMassifChannelCarveParams
{
	/**
	 * `Off` leaves the trench the normal-depth solve cut, which is a channel on any
	 * surface and so is a usable level rather than an absence. `Standard` evolves
	 * the bed under it until what the flow carries in and out of each station
	 * balances.
	 */
	EMassifFidelity Fidelity = EMassifFidelity::Standard;

	/** Median grain on the bed, and what it is made of. Together: the shear a grain first moves at. */
	double GrainDiameterMeters = 0.04;
	double SedimentDensityKgPerCubicMeter = 2650.0;

	/** Shields' threshold and Meyer-Peter & Müller's coefficient: when gravel moves, and how much of it. */
	double CriticalShieldsNumber = 0.047;
	double TransportCoefficient = 8.0;

	/** Share of the bed that is void, so a cubic metre carried off lowers it by more than a metre. */
	double BedPorosity = 0.3;

	/**
	 * Sweeps of the reach, and what one of them stands for.
	 *
	 * A count rather than a settling test (ARCHITECTURE §4). Minutes rather than
	 * years because that is the timescale: gravel moving at these rates rebuilds the
	 * bed between two stations a few metres apart in about that long.
	 */
	int32 StepCount = 100;
	double StepSeconds = 60.0;

	/**
	 * Passes of a three-point smoothing over what one sweep moves each bed by.
	 *
	 * Bed altitudes are floats over a thousand metres of relief, so two stations of
	 * one shape do not measure exactly the same load, and the difference between
	 * neighbours is the one place that last digit lands. Left alone it grows into a
	 * sawtooth of alternating scour and fill. What tells it apart from a real
	 * imbalance is that it alternates, which is exactly what this removes.
	 */
	int32 ChangeSmoothingPasses = 2;

	/** Bisection steps for the water level inside one sweep. */
	int32 SurfaceIterations = 24;

	/**
	 * Stations either side the energy slope is remeasured over between sweeps, and
	 * the floor under it.
	 *
	 * Remeasured rather than kept: a reach that cuts into its own bed loses the
	 * gradient it was cutting with, and that is what stops it. Held at the gradient
	 * it started with, a deepening trench gains shear as it goes — the water in it
	 * gets deeper faster than the channel narrows — and cuts until it hits a limit
	 * rather than until it is graded.
	 */
	int32 SlopeWindowNodes = 4;
	double MinEnergySlope = 1.0e-3;

	/**
	 * Ceiling on one sweep's bed movement, applied smoothly rather than as a clip,
	 * so a station near balance still settles instead of chattering across it.
	 *
	 * Small, because a station far from balance is paced by this rather than by what
	 * it is carrying, and a bed moved by a fixed amount in whichever direction it is
	 * out overshoots and comes back. The reach settles into an alternation of scour
	 * and fill about this deep, so it is set below what anything draws and the
	 * distance is bought with sweeps instead.
	 */
	double MaxBedChangeMetersPerStep = 0.04;

	/**
	 * Deepest the whole carve may cut below the ground it started from.
	 *
	 * A backstop rather than a model. A reach is fed only at its own head, so a long
	 * one whose upper stations settle their load out starves everything below them
	 * and those stations take the difference out of their own beds. Real channels are
	 * fed along their length by the hillsides beside them, which nothing here
	 * supplies.
	 */
	double MaxIncisionBelowGroundMeters = 4.0;

	/** How much of the cut rate survives on solid rock, so hardness slows incision without stopping it. */
	double MinErodibility = 0.05;

	/**
	 * Secondary circulation on a bend: how steeply the bed leans across the channel
	 * per unit of curvature and depth (Engelund 1974, about 7 for gravel). This is
	 * what digs the outer bank out and lays the point bar against the inner one,
	 * and it is most of what makes a channel read as a channel rather than a ditch.
	 */
	double TransverseSlopeCoefficient = 7.0;
	double TransverseRelaxation = 0.35;

	/**
	 * Ceiling on the fall the bend may put across the channel, as a multiple of the
	 * depth the discharge alone predicts.
	 *
	 * A guard, not a model. The lean and the slumping work against each other: the
	 * lean oversteepens the outer bank, the bank comes down into the water, and the
	 * lean takes the fallen material away again. Left to run, the two of them between
	 * them quarry the whole section, and each metre they take makes room for the
	 * next.
	 */
	double MaxBendReliefDepthFactor = 2.0;

	/** Steepest a bank of loose material stands before it slumps into the water. */
	double BankFrictionAngleDegrees = 34.0;
	int32 BankSweepCount = 4;

	/** Shallowest water left in a channel, matching the normal-depth solve's own floor. */
	double MinDepthMeters = 0.30;
};

/**
 * Works the bed of every channel until what the flow carries into a station
 * balances what it carries out.
 *
 * Sediment transport is what makes a channel: water over an un-incised valley
 * floor spreads into a wide shallow sheet and stays there, which is the right
 * answer to the question a fluid solver asks on its own. Shear above a threshold
 * entrains, falling capacity deposits, and the bed follows by Exner — incision,
 * banks, scour pools and point bars are all consequences of that coupling.
 *
 * Each station's supply is what the station above it carries, so a reach grades
 * itself from its own head down. A reach's head is fed at its own capacity: no
 * sediment is routed between reaches, so a confluence neither starves nor buries
 * the channel below it. That is a simplification, and it is what makes the
 * reaches independent enough to solve at once.
 *
 * Deterministic and thread-safe over reaches, by fixed sweep count.
 */
MASSIFWORLDGEN_API void MassifCarveChannels(
	const IMassifBedMaterialSource& BedMaterial,
	const FMassifChannelCarveParams& Params,
	TArray<FMassifRiverReach>& InOutReaches);
