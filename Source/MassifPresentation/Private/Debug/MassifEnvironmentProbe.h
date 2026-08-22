#pragma once

#include "CoreMinimal.h"
#include "MassifEnvironmentChannel.h"
#include "MassifHabitat.h"

class FMassifEnvironmentState;
class FMassifHabitatField;

/** One channel's reading at one point. */
struct FMassifProbeReading
{
	FName Name;
	FString Unit;
	EMassifChannelType Type = EMassifChannelType::Scalar;

	/** The magnitude for a vector channel, the class index for a classified one. */
	double ScalarValue = 0.0;

	/** Meaningful for a Vector2 channel: metres per second east and north. */
	FVector2d VectorValue = FVector2d::ZeroVector;

	/** Meaningful for a Classified channel. Never interpolated. */
	int32 ClassValue = 0;
};

/**
 * What the world thinks is true at one point.
 *
 * The single most useful reading in the debug set: every channel is a
 * simulation whose output is otherwise only visible as a second-order effect on
 * where trees ended up (ARCHITECTURE §5.7), and a heatmap cannot tell you the
 * number under the cursor.
 */
struct FMassifProbeResult
{
	FVector2d PositionMeters = FVector2d::ZeroVector;
	TArray<FMassifProbeReading> Readings;

	/** Set where the probe was taken through the habitat field rather than the bare channels. */
	TOptional<FMassifHabitatSample> Habitat;

	bool IsEmpty() const
	{
		return Readings.IsEmpty();
	}
};

/** Every channel along a line, one row per sample. */
struct FMassifTransectSample
{
	FVector2d PositionMeters = FVector2d::ZeroVector;
	double DistanceMeters = 0.0;

	/** One per entry of FMassifTransect::ChannelNames, in that order. */
	TArray<double> Values;
};

struct FMassifTransect
{
	TArray<FName> ChannelNames;
	TArray<EMassifChannelType> ChannelTypes;
	TArray<FMassifTransectSample> Samples;
};

namespace MassifEnvironmentProbe
{
	/** Samples the point at 8 m spacing. Habitat is where the sample spacing matters, not here. */
	constexpr double HabitatSampleSpacingMeters = 8.0;

	/** Ends of a transect count as samples, so the smallest one that means anything is two. */
	constexpr int32 MinimumTransectSampleCount = 2;
	constexpr int32 DefaultTransectSampleCount = 64;

	/** Every registered channel's value at the point, in name order. */
	FMassifProbeResult Probe(const FMassifEnvironmentState& Environment, FVector2d PositionMeters);

	/** The same, plus what habitat derives there. */
	FMassifProbeResult ProbeHabitat(const FMassifHabitatField& Habitat, FVector2d PositionMeters);

	/**
	 * Every registered channel sampled at SampleCount points from Start to End
	 * inclusive, which is how a lapse rate or a valley wetness profile is read
	 * off rather than guessed at.
	 */
	FMassifTransect Transect(
		const FMassifEnvironmentState& Environment,
		FVector2d StartMeters,
		FVector2d EndMeters,
		int32 SampleCount = DefaultTransectSampleCount);

	void LogProbe(const FMassifProbeResult& Result);

	/** Tab separated, one row per sample, so it pastes into a spreadsheet. */
	void LogTransect(const FMassifTransect& Result);

	/** The reading as the panel and the log write it: value, unit and type. */
	FString FormatReading(const FMassifProbeReading& Reading);

	/** Whether the probe panel is drawn. Toggled by massif.Env.ProbePanel. */
	bool IsPanelVisible();

	/** The last probe taken, for the HUD panel. Empty until massif.Env.Probe has been run. */
	const FMassifProbeResult& GetLastProbe();
}
