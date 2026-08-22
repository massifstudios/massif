#include "MassifEnvironmentProbe.h"

#include "HAL/IConsoleManager.h"
#include "MassifDebugCamera.h"
#include "MassifDebugChannels.h"
#include "MassifDebugChannelView.h"
#include "MassifEnvironmentInspector.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifEnvironmentState.h"
#include "MassifHabitat.h"

namespace
{
	TAutoConsoleVariable<int32> CVarProbePanel(
		TEXT("massif.Env.ProbePanel"), 1,
		TEXT("Draw the last massif.Env.Probe reading as a panel."),
		ECVF_Cheat);

	/** Game-thread only, and diagnostic: nothing downstream of a solve ever reads it. */
	FMassifProbeResult& LastProbe()
	{
		static FMassifProbeResult Result;
		return Result;
	}

	double SampleChannelValue(
		const FMassifEnvironmentState& Environment, FMassifChannelHandle Channel, FVector2d PositionMeters)
	{
		const EMassifChannelType Type = Environment.GetDescriptor(Channel).Type;
		if (Type == EMassifChannelType::Vector2)
		{
			return Environment.SampleVector(Channel, PositionMeters).Length();
		}
		if (Type == EMassifChannelType::Classified)
		{
			return static_cast<double>(Environment.SampleClass(Channel, PositionMeters));
		}
		return Environment.SampleScalar(Channel, PositionMeters);
	}

	void FillReadingValue(
		const FMassifEnvironmentState& Environment,
		FMassifChannelHandle Channel,
		FVector2d PositionMeters,
		FMassifProbeReading& Reading)
	{
		Reading.ScalarValue = SampleChannelValue(Environment, Channel, PositionMeters);
		if (Reading.Type == EMassifChannelType::Vector2)
		{
			Reading.VectorValue = Environment.SampleVector(Channel, PositionMeters);
			return;
		}
		if (Reading.Type == EMassifChannelType::Classified)
		{
			Reading.ClassValue = Environment.SampleClass(Channel, PositionMeters);
		}
	}

	FMassifProbeReading MakeReading(
		const FMassifEnvironmentState& Environment, FMassifChannelHandle Channel, FVector2d PositionMeters)
	{
		const FMassifChannelDescriptor& Descriptor = Environment.GetDescriptor(Channel);

		FMassifProbeReading Reading;
		Reading.Name = Descriptor.Name;
		Reading.Unit = Descriptor.Unit;
		Reading.Type = Descriptor.Type;
		FillReadingValue(Environment, Channel, PositionMeters, Reading);
		return Reading;
	}

	/** The ends are the ends exactly: a transect is read off at the points that were asked for. */
	FVector2d PositionAtFraction(FVector2d StartMeters, FVector2d EndMeters, double Fraction)
	{
		if (Fraction >= 1.0)
		{
			return EndMeters;
		}
		return StartMeters + (EndMeters - StartMeters) * Fraction;
	}

	FMassifTransectSample MakeTransectSample(
		const FMassifEnvironmentState& Environment,
		const TArray<FMassifChannelHandle>& Channels,
		FVector2d StartMeters,
		FVector2d EndMeters,
		double Fraction)
	{
		FMassifTransectSample Sample;
		Sample.PositionMeters = PositionAtFraction(StartMeters, EndMeters, Fraction);
		Sample.DistanceMeters = (EndMeters - StartMeters).Length() * Fraction;
		Sample.Values.Reserve(Channels.Num());
		for (FMassifChannelHandle Channel : Channels)
		{
			Sample.Values.Add(SampleChannelValue(Environment, Channel, Sample.PositionMeters));
		}
		return Sample;
	}

	FString FormatHabitat(const FMassifHabitatSample& Sample)
	{
		return FString::Printf(
			TEXT("  habitat  %s, %.0f m, %.1f deg slope, aspect %.0f deg, %.1f C, humidity %.2f, ")
			TEXT("soil %.2f m, standing water %.2f m"),
			MassifDebugChannels::GetGroundTypeLabel(Sample.GroundType), Sample.AltitudeMeters, Sample.SlopeDegrees,
			Sample.AspectDegrees, Sample.TemperatureCelsius, Sample.Humidity, Sample.SoilDepthMeters,
			Sample.StandingWaterDepthMeters);
	}

	FString FormatTransectHeader(const FMassifTransect& Result)
	{
		FString Header = TEXT("distance_m\tx_m\ty_m");
		for (FName ChannelName : Result.ChannelNames)
		{
			Header += FString::Printf(TEXT("\t%s"), *ChannelName.ToString());
		}
		return Header;
	}

	FString FormatTransectRow(const FMassifTransectSample& Sample)
	{
		FString Row = FString::Printf(
			TEXT("%.1f\t%.1f\t%.1f"), Sample.DistanceMeters, Sample.PositionMeters.X, Sample.PositionMeters.Y);
		for (double Value : Sample.Values)
		{
			Row += FString::Printf(TEXT("\t%.6g"), Value);
		}
		return Row;
	}
}

FMassifProbeResult MassifEnvironmentProbe::Probe(
	const FMassifEnvironmentState& Environment, FVector2d PositionMeters)
{
	FMassifProbeResult Result;
	Result.PositionMeters = PositionMeters;
	for (FMassifChannelHandle Channel : Environment.GetChannelsByName())
	{
		Result.Readings.Add(MakeReading(Environment, Channel, PositionMeters));
	}
	return Result;
}

FMassifProbeResult MassifEnvironmentProbe::ProbeHabitat(
	const FMassifHabitatField& Habitat, FVector2d PositionMeters)
{
	FMassifProbeResult Result = Probe(Habitat.GetEnvironment(), PositionMeters);
	Result.Habitat = Habitat.Sample(PositionMeters, HabitatSampleSpacingMeters);
	return Result;
}

FMassifTransect MassifEnvironmentProbe::Transect(
	const FMassifEnvironmentState& Environment, FVector2d StartMeters, FVector2d EndMeters, int32 SampleCount)
{
	const TArray<FMassifChannelHandle> Channels = Environment.GetChannelsByName();

	FMassifTransect Result;
	for (FMassifChannelHandle Channel : Channels)
	{
		Result.ChannelNames.Add(Environment.GetDescriptor(Channel).Name);
		Result.ChannelTypes.Add(Environment.GetDescriptor(Channel).Type);
	}

	const int32 ClampedCount = FMath::Max(SampleCount, MinimumTransectSampleCount);
	Result.Samples.Reserve(ClampedCount);
	for (int32 SampleIndex = 0; SampleIndex < ClampedCount; ++SampleIndex)
	{
		const double Fraction = SampleIndex / static_cast<double>(ClampedCount - 1);
		Result.Samples.Add(MakeTransectSample(Environment, Channels, StartMeters, EndMeters, Fraction));
	}
	return Result;
}

FString MassifEnvironmentProbe::FormatReading(const FMassifProbeReading& Reading)
{
	if (Reading.Type == EMassifChannelType::Vector2)
	{
		return FString::Printf(TEXT("vector  %8.3f %-4s  (%.3f, %.3f)"),
			Reading.ScalarValue, *Reading.Unit, Reading.VectorValue.X, Reading.VectorValue.Y);
	}
	if (Reading.Type == EMassifChannelType::Classified)
	{
		return FString::Printf(TEXT("class   %8d"), Reading.ClassValue);
	}
	return FString::Printf(TEXT("scalar  %8.3f %-4s"), Reading.ScalarValue, *Reading.Unit);
}

void MassifEnvironmentProbe::LogProbe(const FMassifProbeResult& Result)
{
	UE_LOG(LogTemp, Display, TEXT("Probe at %.1f, %.1f m"),
		Result.PositionMeters.X, Result.PositionMeters.Y);
	for (const FMassifProbeReading& Reading : Result.Readings)
	{
		UE_LOG(LogTemp, Display, TEXT("  %-34s %s"), *Reading.Name.ToString(), *FormatReading(Reading));
	}

	if (Result.Habitat.IsSet())
	{
		UE_LOG(LogTemp, Display, TEXT("%s"), *FormatHabitat(*Result.Habitat));
	}
}

void MassifEnvironmentProbe::LogTransect(const FMassifTransect& Result)
{
	UE_LOG(LogTemp, Display, TEXT("%s"), *FormatTransectHeader(Result));
	for (const FMassifTransectSample& Sample : Result.Samples)
	{
		UE_LOG(LogTemp, Display, TEXT("%s"), *FormatTransectRow(Sample));
	}
}

bool MassifEnvironmentProbe::IsPanelVisible()
{
	return CVarProbePanel.GetValueOnGameThread() != 0;
}

const FMassifProbeResult& MassifEnvironmentProbe::GetLastProbe()
{
	return LastProbe();
}

namespace
{
	int32 ResolveTransectSampleCount(const TArray<FString>& Arguments)
	{
		if (Arguments.Num() < 5)
		{
			return MassifEnvironmentProbe::DefaultTransectSampleCount;
		}
		return FCString::Atoi(*Arguments[4]);
	}

	FVector2d ResolveProbePosition(const TArray<FString>& Arguments, UWorld* World)
	{
		if (Arguments.Num() < 2)
		{
			return MassifDebugCamera::GetPositionMeters(World);
		}
		return FVector2d(FCString::Atod(*Arguments[0]), FCString::Atod(*Arguments[1]));
	}

	FMassifProbeResult TakeProbe(
		const FMassifEnvironmentScheduler& Scheduler, UWorld* World, FVector2d PositionMeters)
	{
		const AMassifDebugChannelView* View = AMassifDebugChannelView::Find(World);
		if (View != nullptr && View->GetHabitat().IsValid())
		{
			return MassifEnvironmentProbe::ProbeHabitat(*View->GetHabitat(), PositionMeters);
		}
		return MassifEnvironmentProbe::Probe(Scheduler.GetState(), PositionMeters);
	}

	void HandleProbeCommand(const TArray<FString>& Arguments, UWorld* World)
	{
		const FMassifEnvironmentScheduler* Scheduler = MassifEnvironmentInspector::FindScheduler(World);
		if (Scheduler == nullptr)
		{
			UE_LOG(LogTemp, Warning, TEXT("massif.Env.Probe: no environment has been solved yet"));
			return;
		}

		LastProbe() = TakeProbe(*Scheduler, World, ResolveProbePosition(Arguments, World));
		MassifEnvironmentProbe::LogProbe(LastProbe());
	}

	void HandleTransectCommand(const TArray<FString>& Arguments, UWorld* World)
	{
		const FMassifEnvironmentScheduler* Scheduler = MassifEnvironmentInspector::FindScheduler(World);
		if (Scheduler == nullptr || Arguments.Num() < 4)
		{
			UE_LOG(LogTemp, Display, TEXT("massif.Env.Transect <X0> <Y0> <X1> <Y1> [SampleCount]"));
			return;
		}

		const FVector2d StartMeters(FCString::Atod(*Arguments[0]), FCString::Atod(*Arguments[1]));
		const FVector2d EndMeters(FCString::Atod(*Arguments[2]), FCString::Atod(*Arguments[3]));
		MassifEnvironmentProbe::LogTransect(MassifEnvironmentProbe::Transect(
			Scheduler->GetState(), StartMeters, EndMeters, ResolveTransectSampleCount(Arguments)));
	}

	FAutoConsoleCommandWithWorldAndArgs GEnvironmentProbeCommand(
		TEXT("massif.Env.Probe"),
		TEXT("Read every channel at a world position in metres, or under the camera with no arguments."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&HandleProbeCommand));

	FAutoConsoleCommandWithWorldAndArgs GEnvironmentTransectCommand(
		TEXT("massif.Env.Transect"),
		TEXT("Sample every channel along a line in world metres, as a tab-separated table."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&HandleTransectCommand));
}
