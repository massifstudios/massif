#include "MassifVegetationProcesses.h"

#include "MassifClimateProcesses.h"
#include "MassifEnvironmentGraph.h"
#include "MassifSnowProcesses.h"
#include "MassifWindProcesses.h"

namespace MassifEnvironmentChannels
{
	const FName& VegetationCover()
	{
		static const FName Name(TEXT("Vegetation.Cover"));
		return Name;
	}

	const FName& VegetationCanopy()
	{
		static const FName Name(TEXT("Vegetation.Canopy"));
		return Name;
	}

	const FName& VegetationHeight()
	{
		static const FName Name(TEXT("Vegetation.Height"));
		return Name;
	}
}

namespace MassifEnvironmentProcesses
{
	const FName& Vegetation()
	{
		static const FName Name(TEXT("Vegetation.Cover"));
		return Name;
	}
}

namespace
{
	/** What a point offers something trying to grow on it, each term 0 hostile to 1 unlimiting. */
	struct FGrowingConditions
	{
		double TemperatureCelsius = 0.0;
		double Wetness = 0.0;
		double SoilDepthMeters = 0.0;
		double WindExposure = 1.0;
		double SnowCoverDuration = 0.0;
	};

	double WarmthFactor(double TemperatureCelsius, const FMassifVegetationParams& Params)
	{
		const double Span = FMath::Max(
			Params.ComfortableTemperatureCelsius - Params.MinimumGrowingTemperatureCelsius,
			UE_DOUBLE_KINDA_SMALL_NUMBER);
		return FMath::Clamp((TemperatureCelsius - Params.MinimumGrowingTemperatureCelsius) / Span, 0.0, 1.0);
	}

	/** Falls away either side of the optimum: parched ground and waterlogged ground both refuse. */
	double MoistureFactor(double Wetness, const FMassifVegetationParams& Params)
	{
		const double Tolerance = FMath::Max(Params.WetnessTolerance, UE_DOUBLE_KINDA_SMALL_NUMBER);
		const double Distance = FMath::Abs(Wetness - Params.OptimalWetness) / Tolerance;
		return FMath::Clamp(1.0 - Distance, 0.0, 1.0);
	}

	double RootingFactor(double SoilDepthMeters, double FullDepthMeters)
	{
		const double Depth = FMath::Max(FullDepthMeters, UE_DOUBLE_KINDA_SMALL_NUMBER);
		return FMath::Clamp(SoilDepthMeters / Depth, 0.0, 1.0);
	}

	/** Exposure at or below neutral costs nothing; above it, the wind shears growth back. */
	double ShelterFactor(double WindExposure, const FMassifVegetationParams& Params)
	{
		const double Excess = FMath::Max(0.0, WindExposure - 1.0);
		return FMath::Clamp(1.0 - Params.WindDamageSensitivity * Excess, 0.0, 1.0);
	}

	/** Ground under snow most of the year has no season left to grow in. */
	double SeasonFactor(double SnowCoverDuration, const FMassifVegetationParams& Params)
	{
		const double Smothering = FMath::Max(Params.SmotheringCoverDuration, UE_DOUBLE_KINDA_SMALL_NUMBER);
		return FMath::Clamp(1.0 - SnowCoverDuration / Smothering, 0.0, 1.0);
	}

	double CoverAt(const FGrowingConditions& Conditions, const FMassifVegetationParams& Params)
	{
		return WarmthFactor(Conditions.TemperatureCelsius, Params)
			* MoistureFactor(Conditions.Wetness, Params)
			* RootingFactor(Conditions.SoilDepthMeters, Params.FullCoverSoilDepthMeters)
			* ShelterFactor(Conditions.WindExposure, Params)
			* SeasonFactor(Conditions.SnowCoverDuration, Params);
	}

	/** A tree asks for more warmth and more ground to stand in than the cover under it does. */
	double CanopyAt(double Cover, const FGrowingConditions& Conditions, const FMassifVegetationParams& Params)
	{
		const double Band = FMath::Max(Params.TreelineBandCelsius, UE_DOUBLE_KINDA_SMALL_NUMBER);
		const double Warmth = FMath::Clamp(
			(Conditions.TemperatureCelsius - Params.TreelineTemperatureCelsius) / Band, 0.0, 1.0);
		return Cover * Warmth * RootingFactor(Conditions.SoilDepthMeters, Params.TreeRootingDepthMeters);
	}

	FMassifChannelDescriptor MakeVegetationChannel(
		const FName& Name, const TCHAR* Unit, double CellSizeMeters, TOptional<FVector2d> DisplayRange)
	{
		FMassifChannelDescriptor Descriptor;
		Descriptor.Name = Name;
		Descriptor.Type = EMassifChannelType::Scalar;
		Descriptor.Encoding = EMassifChannelEncoding::Uint16;
		Descriptor.CellSizeMeters = CellSizeMeters;
		Descriptor.Unit = Unit;
		Descriptor.ColorMap = EMassifChannelColorMap::Viridis;
		Descriptor.DisplayRange = DisplayRange;
		return Descriptor;
	}
}

/**
 * What grows where, and what it does back to the world it grows in.
 *
 * This is the process that closes the environment's loops. Everything before it
 * runs once, in order; vegetation holds the soil that vegetation needs, and its
 * canopy slows the wind that moves the snow that decides where canopy grows. The
 * scheduler finds those cycles from the declarations and relaxes them a fixed
 * number of times (ARCHITECTURE §5.6.3).
 */
class FVegetationProcess final : public IMassifEnvironmentProcess
{
public:
	explicit FVegetationProcess(const FMassifVegetationParams& InParams)
		: Params(InParams)
	{
	}

	FMassifProcessDeclaration Declare() const override;
	void Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const override;

private:
	void WriteBare(FMassifEnvironmentContext& Context) const;

	FGrowingConditions ConditionsAt(
		const FMassifEnvironmentState& State, const struct FVegetationInputs& Inputs, FVector2d PositionMeters,
		EMassifFidelity Fidelity) const;

	FMassifVegetationParams Params;
};

/** The channels the solve reads, resolved once. */
struct FVegetationInputs
{
	FMassifChannelHandle Temperature;
	FMassifChannelHandle Wetness;
	FMassifChannelHandle SoilDepth;
	FMassifChannelHandle WindExposure;
	FMassifChannelHandle SnowCoverDuration;
};

FMassifProcessDeclaration FVegetationProcess::Declare() const
{
	FMassifProcessDeclaration Declaration;
	Declaration.Name = MassifEnvironmentProcesses::Vegetation();
	Declaration.InputChannels = {
		MassifEnvironmentChannels::MeanTemperature(), MassifEnvironmentChannels::Wetness(),
		MassifEnvironmentChannels::SoilDepth()};
	Declaration.OptionalInputChannels = {
		MassifEnvironmentChannels::WindExposure(), MassifEnvironmentChannels::SnowCoverDuration()};
	Declaration.SupportedFidelities = {EMassifFidelity::Cheap, EMassifFidelity::Standard};

	// Soil and wind read what this writes, so the scheduler will find a cycle here
	// and must be told it is meant.
	Declaration.bParticipatesInFeedback = true;

	Declaration.OutputChannels.Add(MakeVegetationChannel(
		MassifEnvironmentChannels::VegetationCover(), TEXT("0-1"), Params.CellSizeMeters,
		FVector2d(0.0, 1.0)));
	Declaration.OutputChannels.Add(MakeVegetationChannel(
		MassifEnvironmentChannels::VegetationCanopy(), TEXT("0-1"), Params.CellSizeMeters,
		FVector2d(0.0, 1.0)));
	Declaration.OutputChannels.Add(MakeVegetationChannel(
		MassifEnvironmentChannels::VegetationHeight(), TEXT("m"), Params.CellSizeMeters, {}));
	return Declaration;
}

void FVegetationProcess::WriteBare(FMassifEnvironmentContext& Context) const
{
	const FName Names[] = {
		MassifEnvironmentChannels::VegetationCover(), MassifEnvironmentChannels::VegetationCanopy(),
		MassifEnvironmentChannels::VegetationHeight()};
	for (const FName& Name : Names)
	{
		const TUniquePtr<FMassifChannelWriteScope> Scope = Context.OpenOutput(Name);
		TArrayView<double> Values = Scope->GetComponent(0);
		for (int32 CellIndex = 0; CellIndex < Values.Num(); ++CellIndex)
		{
			Values[CellIndex] = 0.0;
		}
		Scope->Commit();
	}
}

FGrowingConditions FVegetationProcess::ConditionsAt(
	const FMassifEnvironmentState& State, const FVegetationInputs& Inputs, FVector2d PositionMeters,
	EMassifFidelity Fidelity) const
{
	FGrowingConditions Conditions;
	Conditions.TemperatureCelsius = State.SampleScalar(Inputs.Temperature, PositionMeters);
	Conditions.Wetness = State.SampleScalar(Inputs.Wetness, PositionMeters);
	Conditions.SoilDepthMeters = State.SampleScalar(Inputs.SoilDepth, PositionMeters);
	if (Fidelity == EMassifFidelity::Cheap)
	{
		return Conditions;
	}

	// Wind and snow are the two the loop runs through. Both read as neutral until
	// the round that writes them has happened, and stay neutral in a graph that
	// holds neither.
	if (Inputs.WindExposure.IsValid())
	{
		Conditions.WindExposure = State.SampleScalar(Inputs.WindExposure, PositionMeters);
	}
	if (Inputs.SnowCoverDuration.IsValid())
	{
		Conditions.SnowCoverDuration = State.SampleScalar(Inputs.SnowCoverDuration, PositionMeters);
	}
	return Conditions;
}

void FVegetationProcess::Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const
{
	if (Fidelity == EMassifFidelity::Off)
	{
		WriteBare(Context);
		return;
	}

	FVegetationInputs Inputs;
	Inputs.Temperature = Context.GetInput(MassifEnvironmentChannels::MeanTemperature());
	Inputs.Wetness = Context.GetInput(MassifEnvironmentChannels::Wetness());
	Inputs.SoilDepth = Context.GetInput(MassifEnvironmentChannels::SoilDepth());
	Inputs.WindExposure = Context.FindOptionalInput(MassifEnvironmentChannels::WindExposure());
	Inputs.SnowCoverDuration = Context.FindOptionalInput(MassifEnvironmentChannels::SnowCoverDuration());

	const TUniquePtr<FMassifChannelWriteScope> Cover =
		Context.OpenOutput(MassifEnvironmentChannels::VegetationCover());
	const TUniquePtr<FMassifChannelWriteScope> Canopy =
		Context.OpenOutput(MassifEnvironmentChannels::VegetationCanopy());
	const TUniquePtr<FMassifChannelWriteScope> Height =
		Context.OpenOutput(MassifEnvironmentChannels::VegetationHeight());

	const FMassifChannelGrid& Grid = Cover->GetGrid();
	TArrayView<double> CoverValues = Cover->GetComponent(0);
	TArrayView<double> CanopyValues = Canopy->GetComponent(0);
	TArrayView<double> HeightValues = Height->GetComponent(0);

	for (int32 CellIndex = 0; CellIndex < CoverValues.Num(); ++CellIndex)
	{
		const FVector2d Position = Grid.CellCentreMeters(CellIndex % Grid.Width, CellIndex / Grid.Width);
		const FGrowingConditions Conditions =
			ConditionsAt(Context.GetState(), Inputs, Position, Fidelity);

		CoverValues[CellIndex] = CoverAt(Conditions, Params);
		CanopyValues[CellIndex] = CanopyAt(CoverValues[CellIndex], Conditions, Params);
		HeightValues[CellIndex] = Params.MaxCanopyHeightMeters * CanopyValues[CellIndex];
	}

	Cover->Commit();
	Canopy->Commit();
	Height->Commit();
}

void MassifRegisterVegetationProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifVegetationParams& Params)
{
	Scheduler.RegisterProcess(MakeShared<FVegetationProcess>(Params));
}
