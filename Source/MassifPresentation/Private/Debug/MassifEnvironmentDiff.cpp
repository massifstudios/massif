#include "MassifEnvironmentDiff.h"

#include "HAL/IConsoleManager.h"
#include "MassifEnvironmentInspector.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifEnvironmentState.h"

namespace
{
	/** One channel as it stood after the first solve, to measure the second against. */
	struct FChannelCapture
	{
		FMassifChannelHandle Channel;
		FName Name;
		int32 ComponentCount = 0;
		int32 SliceCount = 0;
		int32 CellCount = 0;

		/** Slice-major, then component, then cell. */
		TArray<double> Values;

		int32 EntryIndex(int32 TimeSlice, int32 ComponentIndex, int32 CellIndex) const
		{
			return (TimeSlice * ComponentCount + ComponentIndex) * CellCount + CellIndex;
		}
	};

	struct FDifferenceAccumulator
	{
		double Total = 0.0;
		double Largest = 0.0;
		int32 LargestCellIndex = INDEX_NONE;
		int32 ChangedCount = 0;
		int32 SampleCount = 0;

		void Add(double Difference, int32 CellIndex)
		{
			Total += Difference;
			++SampleCount;
			if (Difference > 0.0)
			{
				++ChangedCount;
			}
			if (Difference > Largest)
			{
				Largest = Difference;
				LargestCellIndex = CellIndex;
			}
		}
	};

	void CaptureComponentSlice(
		const FMassifEnvironmentState& Environment,
		int32 TimeSlice,
		int32 ComponentIndex,
		FChannelCapture& Capture)
	{
		const FMassifChannelGrid& Grid = Environment.GetGrid(Capture.Channel);
		for (int32 CellIndex = 0; CellIndex < Capture.CellCount; ++CellIndex)
		{
			Capture.Values[Capture.EntryIndex(TimeSlice, ComponentIndex, CellIndex)] = Environment.ReadCell(
				Capture.Channel, CellIndex % Grid.Width, CellIndex / Grid.Width, ComponentIndex, TimeSlice);
		}
	}

	FChannelCapture CaptureChannel(const FMassifEnvironmentState& Environment, FMassifChannelHandle Channel)
	{
		const FMassifChannelDescriptor& Descriptor = Environment.GetDescriptor(Channel);

		FChannelCapture Capture;
		Capture.Channel = Channel;
		Capture.Name = Descriptor.Name;
		Capture.ComponentCount = MassifChannelComponentCount(Descriptor.Type);
		Capture.SliceCount = Descriptor.TimeSliceCount;
		Capture.CellCount = Environment.GetGrid(Channel).CellCount();
		Capture.Values.SetNumUninitialized(Capture.SliceCount * Capture.ComponentCount * Capture.CellCount);

		for (int32 EntryIndex = 0; EntryIndex < Capture.SliceCount * Capture.ComponentCount; ++EntryIndex)
		{
			CaptureComponentSlice(
				Environment, EntryIndex / Capture.ComponentCount, EntryIndex % Capture.ComponentCount, Capture);
		}
		return Capture;
	}

	TArray<FChannelCapture> CaptureState(const FMassifEnvironmentState& Environment)
	{
		TArray<FChannelCapture> Captures;
		for (FMassifChannelHandle Channel : Environment.GetChannelsByName())
		{
			Captures.Add(CaptureChannel(Environment, Channel));
		}
		return Captures;
	}

	/** Distance between the two readings of one cell, which is the absolute difference for a scalar. */
	double CellDifference(
		const FMassifEnvironmentState& Environment,
		const FChannelCapture& Capture,
		const FMassifChannelGrid& Grid,
		int32 TimeSlice,
		int32 CellIndex)
	{
		double SumOfSquares = 0.0;
		for (int32 ComponentIndex = 0; ComponentIndex < Capture.ComponentCount; ++ComponentIndex)
		{
			const double Before = Capture.Values[Capture.EntryIndex(TimeSlice, ComponentIndex, CellIndex)];
			const double After = Environment.ReadCell(
				Capture.Channel, CellIndex % Grid.Width, CellIndex / Grid.Width, ComponentIndex, TimeSlice);
			SumOfSquares += FMath::Square(After - Before);
		}
		return FMath::Sqrt(SumOfSquares);
	}

	void AccumulateSlice(
		const FMassifEnvironmentState& Environment,
		const FChannelCapture& Capture,
		const FMassifChannelGrid& Grid,
		int32 TimeSlice,
		FDifferenceAccumulator& Accumulator)
	{
		for (int32 CellIndex = 0; CellIndex < Capture.CellCount; ++CellIndex)
		{
			Accumulator.Add(CellDifference(Environment, Capture, Grid, TimeSlice, CellIndex), CellIndex);
		}
	}

	FVector2d LargestDifferencePosition(const FMassifChannelGrid& Grid, const FDifferenceAccumulator& Accumulator)
	{
		if (Accumulator.LargestCellIndex == INDEX_NONE)
		{
			return FVector2d::ZeroVector;
		}
		return Grid.CellCentreMeters(
			Accumulator.LargestCellIndex % Grid.Width, Accumulator.LargestCellIndex / Grid.Width);
	}

	FMassifChannelDifference MakeDifference(
		const FChannelCapture& Capture, const FMassifChannelGrid& Grid, const FDifferenceAccumulator& Accumulator)
	{
		FMassifChannelDifference Difference;
		Difference.Name = Capture.Name;
		Difference.CellCount = Capture.CellCount;
		Difference.LargestAbsoluteDifference = Accumulator.Largest;
		Difference.LargestDifferencePositionMeters = LargestDifferencePosition(Grid, Accumulator);
		if (Accumulator.SampleCount == 0)
		{
			return Difference;
		}

		Difference.MeanAbsoluteDifference = Accumulator.Total / Accumulator.SampleCount;
		Difference.ChangedCellFraction = Accumulator.ChangedCount / static_cast<double>(Accumulator.SampleCount);
		return Difference;
	}

	FMassifChannelDifference CompareChannel(
		const FMassifEnvironmentState& Environment, const FChannelCapture& Capture)
	{
		const FMassifChannelGrid& Grid = Environment.GetGrid(Capture.Channel);

		FDifferenceAccumulator Accumulator;
		for (int32 TimeSlice = 0; TimeSlice < Capture.SliceCount; ++TimeSlice)
		{
			AccumulateSlice(Environment, Capture, Grid, TimeSlice, Accumulator);
		}
		return MakeDifference(Capture, Grid, Accumulator);
	}

	bool SolveAtFidelity(
		FMassifEnvironmentScheduler& Scheduler, FName ProcessName, EMassifFidelity Fidelity)
	{
		if (!Scheduler.SetFidelity(ProcessName, Fidelity))
		{
			return false;
		}

		Scheduler.Execute();
		return true;
	}
}

const FMassifChannelDifference* FMassifFidelityDifference::FindChannel(FName ChannelName) const
{
	return Channels.FindByPredicate([ChannelName](const FMassifChannelDifference& Difference)
	{
		return Difference.Name == ChannelName;
	});
}

bool FMassifFidelityDifference::IsIdentical() const
{
	for (const FMassifChannelDifference& Difference : Channels)
	{
		if (Difference.LargestAbsoluteDifference > 0.0)
		{
			return false;
		}
	}
	return true;
}

FMassifFidelityDifference MassifEnvironmentDiff::Compare(
	FMassifEnvironmentScheduler& Scheduler,
	FName ProcessName,
	EMassifFidelity FidelityA,
	EMassifFidelity FidelityB)
{
	FMassifFidelityDifference Result;
	Result.ProcessName = ProcessName;
	Result.FidelityA = FidelityA;
	Result.FidelityB = FidelityB;

	const EMassifFidelity StartingFidelity = Scheduler.GetFidelity(ProcessName);
	if (!SolveAtFidelity(Scheduler, ProcessName, FidelityA))
	{
		return Result;
	}

	const TArray<FChannelCapture> Captures = CaptureState(Scheduler.GetState());
	if (!SolveAtFidelity(Scheduler, ProcessName, FidelityB))
	{
		SolveAtFidelity(Scheduler, ProcessName, StartingFidelity);
		return Result;
	}

	for (const FChannelCapture& Capture : Captures)
	{
		Result.Channels.Add(CompareChannel(Scheduler.GetState(), Capture));
	}
	Result.bComparable = true;

	SolveAtFidelity(Scheduler, ProcessName, StartingFidelity);
	return Result;
}

void MassifEnvironmentDiff::Log(const FMassifFidelityDifference& Difference)
{
	if (!Difference.bComparable)
	{
		UE_LOG(LogTemp, Warning, TEXT("%s does not implement both %s and %s"),
			*Difference.ProcessName.ToString(), MassifFidelityName(Difference.FidelityA),
			MassifFidelityName(Difference.FidelityB));
		return;
	}

	UE_LOG(LogTemp, Display, TEXT("%s: %s against %s"), *Difference.ProcessName.ToString(),
		MassifFidelityName(Difference.FidelityA), MassifFidelityName(Difference.FidelityB));
	for (const FMassifChannelDifference& Channel : Difference.Channels)
	{
		UE_LOG(LogTemp, Display, TEXT("  %-34s mean %10.4g  largest %10.4g at %.0f, %.0f m  changed %5.1f%%"),
			*Channel.Name.ToString(), Channel.MeanAbsoluteDifference, Channel.LargestAbsoluteDifference,
			Channel.LargestDifferencePositionMeters.X, Channel.LargestDifferencePositionMeters.Y,
			Channel.ChangedCellFraction * 100.0);
	}
}

namespace
{
	void HandleDiffCommand(const TArray<FString>& Arguments, UWorld* World)
	{
		FMassifEnvironmentScheduler* Scheduler = MassifEnvironmentInspector::FindScheduler(World);
		if (Scheduler == nullptr || Arguments.Num() < 3)
		{
			UE_LOG(LogTemp, Display,
				TEXT("massif.Env.Diff <Process> <Off|Cheap|Standard|Reference> <Off|Cheap|Standard|Reference>"));
			return;
		}

		const TOptional<EMassifFidelity> FidelityA = MassifEnvironmentInspector::ParseFidelity(Arguments[1]);
		const TOptional<EMassifFidelity> FidelityB = MassifEnvironmentInspector::ParseFidelity(Arguments[2]);
		if (!FidelityA.IsSet() || !FidelityB.IsSet())
		{
			UE_LOG(LogTemp, Warning, TEXT("massif.Env.Diff: %s and %s are not both levels"),
				*Arguments[1], *Arguments[2]);
			return;
		}

		MassifEnvironmentDiff::Log(
			MassifEnvironmentDiff::Compare(*Scheduler, FName(*Arguments[0]), *FidelityA, *FidelityB));
	}

	FAutoConsoleCommandWithWorldAndArgs GEnvironmentDiffCommand(
		TEXT("massif.Env.Diff"),
		TEXT("Solve one process at two fidelities and report what moved in every channel."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&HandleDiffCommand));
}
