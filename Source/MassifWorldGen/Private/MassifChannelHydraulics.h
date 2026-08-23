#pragma once

#include "CoreMinimal.h"
#include "MassifChannelProfile.h"
#include "MassifRiverNetwork.h"

/**
 * What one cross-section carries with its water at a given level.
 *
 * Measured over the connected run of offsets around the deepest point of the
 * bed rather than over every offset below the surface: a section wide enough to
 * find both banks often clips a ditch or a side channel beyond one of them, and
 * a width spanning both would be the distance between two separate waters.
 */
struct FMassifSectionFlow
{
	double AreaSquareMeters = 0.0;
	double WettedPerimeterMeters = 0.0;
	double TopWidthMeters = 0.0;
	double MaxDepthMeters = 0.0;

	/** The run of offsets under water, or INDEX_NONE where the section holds none. */
	int32 FirstWetOffset = INDEX_NONE;
	int32 LastWetOffset = INDEX_NONE;

	bool IsWet() const
	{
		return FirstWetOffset != INDEX_NONE && AreaSquareMeters > UE_DOUBLE_SMALL_NUMBER;
	}

	/** Area over the bed it rubs against: what Manning's resistance is felt over. */
	double HydraulicRadiusMeters() const;

	/** Area over the surface it presents: the depth a wave on this section travels at. */
	double HydraulicDepthMeters() const;
};

/**
 * The open-channel hydraulics every channel milestone measures with.
 *
 * One definition of what a section is and what it carries, shared by the
 * normal-depth solve that first fits a discharge into the ground, by the carve
 * that evolves the bed under it, and by the backwater sweep that puts the
 * surface back on top. Three answers to that question would disagree, and the
 * three of them run over the same profiles.
 */
namespace MassifChannelHydraulics
{
	constexpr double GravityMetersPerSecondSquared = 9.80665;
	constexpr double WaterDensityKgPerCubicMeter = 1000.0;

	/**
	 * Distance from each station to the one above it. The head takes the distance to
	 * the one below instead, so every entry is a real span and no caller has to
	 * special-case the first.
	 */
	void MeasureStationSpans(const TArray<FMassifRiverNode>& Nodes, TArray<double>& OutSpanMeters);

	/** Deepest offset of the bed: the thalweg the wetted run is grown out from. */
	int32 FindThalwegOffset(const FMassifChannelProfile& Profile);

	/**
	 * The lowest of a section's offsets, and on a tie the one nearest the
	 * centreline.
	 *
	 * The tie-break is what a section across level ground turns on. A valley floor,
	 * or the flat ribbon a laser scan returns over water, is level at every offset:
	 * taking the first of them puts the channel against the edge of its own profile
	 * instead of under the line it was traced along.
	 */
	int32 FindLowestOffset(const float* AltitudeMeters);

	FMassifSectionFlow MeasureSection(const FMassifChannelProfile& Profile, double SurfaceAltitudeMeters);

	/**
	 * Manning's discharge through a section. Rises with the water level, which is
	 * what lets a level be bisected out of a discharge.
	 */
	double ManningDischarge(const FMassifSectionFlow& Section, double Roughness, double EnergySlope);

	/**
	 * The level at which the section carries the discharge — normal depth, solved
	 * on the section's own shape rather than on a prismatic idealisation of it.
	 *
	 * Bisected over a fixed count rather than to a tolerance (ARCHITECTURE §4),
	 * between the thalweg and the ceiling below.
	 */
	double SolveSurfaceAltitudeMeters(
		const FMassifChannelProfile& Profile, double Roughness, double EnergySlope,
		double DischargeCubicMetersPerSecond, double MinDepthMeters, int32 IterationCount);

	/**
	 * The lower of the two crests the water would spill over.
	 *
	 * Taken as the highest bed the profile reaches on each side of the thalweg, so
	 * a section cut into a slope is bounded by the low bank and not by the hillside
	 * standing above the other one.
	 */
	double BankCrestAltitudeMeters(const FMassifChannelProfile& Profile);

	/**
	 * As high as the water is allowed to stand: its own bank, or the minimum column
	 * over the thalweg where the section has no bank to speak of.
	 *
	 * The bank is where the section stops being a channel — above it the water is on
	 * a floodplain, and a floodplain is not what a cross-section can answer for. The
	 * floor under that is what keeps a station on flat ground from having nowhere at
	 * all to put its water: a section with no relief across it has its crest at its
	 * own thalweg, and a solve bracketed between those two carries nothing.
	 */
	double SurfaceCeilingMeters(const FMassifChannelProfile& Profile, double MinDepthMeters);

	double MeanSpeedMetersPerSecond(double DischargeCubicMetersPerSecond, double AreaSquareMeters);

	/** Speed against the speed of a wave on water this deep on average across the section. */
	double FroudeNumber(double SpeedMetersPerSecond, const FMassifSectionFlow& Section);

	/** Bed shear under a column this deep running down a slope this steep. */
	double ShearStressPascals(double DepthMeters, double EnergySlope);

	/**
	 * Measures the node's section at the surface it carries and hands the answer to
	 * the node itself.
	 *
	 * Width, depth and speed leave as what fits the ground here, so everything that
	 * already reads a node — the field, the terrain carve, the water mesh, the
	 * masks — picks the section up without being taught about it. Every solve that
	 * moves a bed or a surface ends by calling this, and so all of them agree.
	 */
	void ApplySectionToNode(FMassifRiverNode& InOutNode, double MinDepthMeters);
}
