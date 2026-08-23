#pragma once

#include "CoreMinimal.h"
#include "MassifChannelProfile.h"

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

	/** Deepest offset of the bed: the thalweg the wetted run is grown out from. */
	int32 FindThalwegOffset(const FMassifChannelProfile& Profile);

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
	 * Bisected over a fixed count rather than to a tolerance (ARCHITECTURE §4).
	 * The ceiling is where the section stops being a channel: above it the water
	 * is over the bank and the answer is a floodplain's, not this section's.
	 */
	double SolveSurfaceAltitudeMeters(
		const FMassifChannelProfile& Profile, double Roughness, double EnergySlope,
		double DischargeCubicMetersPerSecond, double CeilingAltitudeMeters, int32 IterationCount);

	/**
	 * The lower of the two crests the water would spill over.
	 *
	 * Taken as the highest bed the profile reaches on each side of the thalweg, so
	 * a section cut into a slope is bounded by the low bank and not by the hillside
	 * standing above the other one.
	 */
	double BankCrestAltitudeMeters(const FMassifChannelProfile& Profile);

	double MeanSpeedMetersPerSecond(double DischargeCubicMetersPerSecond, double AreaSquareMeters);

	/** Speed against the speed of a wave on water this deep on average across the section. */
	double FroudeNumber(double SpeedMetersPerSecond, const FMassifSectionFlow& Section);

	/** Bed shear under a column this deep running down a slope this steep. */
	double ShearStressPascals(double DepthMeters, double EnergySlope);
}
