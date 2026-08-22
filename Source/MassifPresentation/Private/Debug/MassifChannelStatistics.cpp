#include "MassifChannelStatistics.h"

#include "MassifEnvironmentState.h"

namespace
{
	int32 BucketOfValue(double Value, double Minimum, double Span)
	{
		const double Fraction = (Value - Minimum) / Span;
		return FMath::Clamp(
			FMath::FloorToInt32(Fraction * MassifHistogramBucketCount), 0, MassifHistogramBucketCount - 1);
	}

	void FillBuckets(TArrayView<const double> Values, FMassifChannelStatistics& Statistics)
	{
		const double Span = Statistics.Maximum - Statistics.Minimum;
		if (Span <= 0.0)
		{
			// A flat channel is one bucket tall, which is exactly the reading that matters.
			Statistics.BucketCounts[0] = Values.Num();
			return;
		}

		for (double Value : Values)
		{
			++Statistics.BucketCounts[BucketOfValue(Value, Statistics.Minimum, Span)];
		}
	}

	void MeasureExtentAndMean(TArrayView<const double> Values, FMassifChannelStatistics& Statistics)
	{
		Statistics.Minimum = TNumericLimits<double>::Max();
		Statistics.Maximum = TNumericLimits<double>::Lowest();

		double Total = 0.0;
		for (double Value : Values)
		{
			Statistics.Minimum = FMath::Min(Statistics.Minimum, Value);
			Statistics.Maximum = FMath::Max(Statistics.Maximum, Value);
			Total += Value;
		}
		Statistics.Mean = Total / Values.Num();
	}

	void ReadChannelCells(
		const FMassifEnvironmentState& Environment,
		FMassifChannelHandle Channel,
		int32 ComponentIndex,
		int32 TimeSlice,
		TArray<double>& OutValues)
	{
		const FMassifChannelGrid& Grid = Environment.GetGrid(Channel);
		OutValues.SetNumUninitialized(Grid.CellCount());
		for (int32 CellIndex = 0; CellIndex < Grid.CellCount(); ++CellIndex)
		{
			OutValues[CellIndex] = Environment.ReadCell(
				Channel, CellIndex % Grid.Width, CellIndex / Grid.Width, ComponentIndex, TimeSlice);
		}
	}
}

int32 FMassifChannelStatistics::GetLargestBucketCount() const
{
	int32 Largest = 0;
	for (int32 Count : BucketCounts)
	{
		Largest = FMath::Max(Largest, Count);
	}
	return Largest;
}

FMassifChannelStatistics MassifChannelStatistics::Measure(TArrayView<const double> Values)
{
	FMassifChannelStatistics Statistics;
	Statistics.BucketCounts.SetNumZeroed(MassifHistogramBucketCount);
	Statistics.SampleCount = Values.Num();
	if (Values.IsEmpty())
	{
		return Statistics;
	}

	MeasureExtentAndMean(Values, Statistics);
	FillBuckets(Values, Statistics);
	return Statistics;
}

FMassifChannelStatistics MassifChannelStatistics::MeasureChannel(
	const FMassifEnvironmentState& Environment,
	FMassifChannelHandle Channel,
	int32 ComponentIndex,
	int32 TimeSlice)
{
	TArray<double> Values;
	ReadChannelCells(Environment, Channel, ComponentIndex, TimeSlice, Values);
	return Measure(Values);
}
