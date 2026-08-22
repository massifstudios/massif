#pragma once

#include "CoreMinimal.h"
#include "MassifEnvironmentChannel.h"

class FMassifEnvironmentState;

/** Buckets a distribution is summarised into. Coarse on purpose: this is a strip in a panel. */
inline constexpr int32 MassifHistogramBucketCount = 16;

/**
 * What a view's numbers look like taken together, which a normalised colour ramp
 * cannot say: a channel holding 0.03 everywhere and a channel spanning its whole
 * range paint the same picture (ARCHITECTURE §5.7).
 */
struct FMassifChannelStatistics
{
	double Minimum = 0.0;
	double Maximum = 0.0;
	double Mean = 0.0;
	int32 SampleCount = 0;

	/** MassifHistogramBucketCount counts spanning Minimum to Maximum. Sums to SampleCount. */
	TArray<int32> BucketCounts;

	int32 GetLargestBucketCount() const;
};

namespace MassifChannelStatistics
{
	FMassifChannelStatistics Measure(TArrayView<const double> Values);

	/** Every cell of one component of one slice, unfiltered. */
	FMassifChannelStatistics MeasureChannel(
		const FMassifEnvironmentState& Environment,
		FMassifChannelHandle Channel,
		int32 ComponentIndex = 0,
		int32 TimeSlice = 0);
}
