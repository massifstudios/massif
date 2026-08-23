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

	const FName& Interception()
	{
		static const FName Name(TEXT("Vegetation.Interception"));
		return Name;
	}

	const FName& VegetationClass()
	{
		static const FName Name(TEXT("Vegetation.Class"));
		return Name;
	}

	const FName& Disturbance()
	{
		static const FName Name(TEXT("Vegetation.Disturbance"));
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

const TCHAR* MassifVegetationClassName(EMassifVegetationClass Class)
{
	switch (Class)
	{
		case EMassifVegetationClass::Bare: return TEXT("Bare");
		case EMassifVegetationClass::Snowbed: return TEXT("Snowbed");
		case EMassifVegetationClass::AlpineMeadow: return TEXT("AlpineMeadow");
		case EMassifVegetationClass::DwarfShrub: return TEXT("DwarfShrub");
		case EMassifVegetationClass::ConiferForest: return TEXT("ConiferForest");
		case EMassifVegetationClass::BroadleafForest: return TEXT("BroadleafForest");
		case EMassifVegetationClass::Wetland: return TEXT("Wetland");
		default: return TEXT("Unknown");
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

	/** What one cell came to: what is left standing, what took it down, and what it is. */
	struct FVegetationStand
	{
		double Cover = 0.0;
		double Canopy = 0.0;
		double Disturbance = 0.0;
		EMassifVegetationClass Class = EMassifVegetationClass::Bare;
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

	/**
	 * What has been knocked down of what could have grown.
	 *
	 * The wind throws trees where it gets at them, so that term carries the canopy
	 * with it — open ground has nothing to throw. Snow lying past the season a
	 * snowbed clears in scours the ground whatever stood on it.
	 */
	double DisturbanceAt(double Canopy, const FGrowingConditions& Conditions, const FMassifVegetationParams& Params)
	{
		const double Windthrow =
			Params.WindthrowSensitivity * FMath::Max(0.0, Conditions.WindExposure - 1.0) * Canopy;
		const double Scour = Params.SnowScourSensitivity
			* FMath::Max(0.0, Conditions.SnowCoverDuration - Params.SnowbedCoverDuration);
		return FMath::Clamp(Windthrow + Scour, 0.0, 1.0);
	}

	/** Broadleaf wants the warm damp ground low down; the belt above it is conifer. */
	EMassifVegetationClass ForestClass(
		const FGrowingConditions& Conditions, const FMassifVegetationParams& Params)
	{
		if (Conditions.TemperatureCelsius > Params.BroadleafTemperatureCelsius)
		{
			return EMassifVegetationClass::BroadleafForest;
		}
		return EMassifVegetationClass::ConiferForest;
	}

	/** A key over what the stand came to and what it grew in, read in order: the first match holds. */
	EMassifVegetationClass ClassifyStand(
		const FVegetationStand& Stand, const FGrowingConditions& Conditions,
		const FMassifVegetationParams& Params)
	{
		if (Conditions.Wetness > Params.WetlandWetness)
		{
			return EMassifVegetationClass::Wetland;
		}
		if (Stand.Cover < Params.BareCoverThreshold)
		{
			return EMassifVegetationClass::Bare;
		}
		if (Conditions.SnowCoverDuration > Params.SnowbedCoverDuration)
		{
			return EMassifVegetationClass::Snowbed;
		}
		if (Stand.Canopy > Params.ForestCanopyThreshold)
		{
			return ForestClass(Conditions, Params);
		}
		if (Conditions.WindExposure > Params.ShrubExposure || Stand.Canopy > 0.0)
		{
			return EMassifVegetationClass::DwarfShrub;
		}
		return EMassifVegetationClass::AlpineMeadow;
	}

	FVegetationStand StandAt(const FGrowingConditions& Conditions, const FMassifVegetationParams& Params)
	{
		const double PotentialCover = CoverAt(Conditions, Params);
		const double PotentialCanopy = CanopyAt(PotentialCover, Conditions, Params);

		FVegetationStand Stand;
		Stand.Disturbance = DisturbanceAt(PotentialCanopy, Conditions, Params);

		// What is left standing is what everything downstream sees: the soil it
		// holds, the wind it slows and the rain it catches are all the reduced stand.
		const double Standing = 1.0 - Stand.Disturbance;
		Stand.Cover = PotentialCover * Standing;
		Stand.Canopy = PotentialCanopy * Standing;
		Stand.Class = ClassifyStand(Stand, Conditions, Params);
		return Stand;
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

	/** Class indices, so the channel is sampled by nearest cell and never blended. */
	FMassifChannelDescriptor MakeVegetationClassChannel(double CellSizeMeters)
	{
		FMassifChannelDescriptor Descriptor;
		Descriptor.Name = MassifEnvironmentChannels::VegetationClass();
		Descriptor.Type = EMassifChannelType::Classified;
		Descriptor.Encoding = EMassifChannelEncoding::Uint8;
		Descriptor.CellSizeMeters = CellSizeMeters;
		Descriptor.Unit = TEXT("class");
		Descriptor.ColorMap = EMassifChannelColorMap::Categorical;
		return Descriptor;
	}

	/** Every channel one run writes, opened together so a cell is written in one place. */
	class FVegetationOutputs
	{
	public:
		explicit FVegetationOutputs(const FMassifEnvironmentContext& Context)
			: CoverScope(Context.OpenOutput(MassifEnvironmentChannels::VegetationCover()))
			, CanopyScope(Context.OpenOutput(MassifEnvironmentChannels::VegetationCanopy()))
			, HeightScope(Context.OpenOutput(MassifEnvironmentChannels::VegetationHeight()))
			, InterceptionScope(Context.OpenOutput(MassifEnvironmentChannels::Interception()))
			, DisturbanceScope(Context.OpenOutput(MassifEnvironmentChannels::Disturbance()))
			, ClassScope(Context.OpenOutput(MassifEnvironmentChannels::VegetationClass()))
			, CoverValues(CoverScope->GetComponent(0))
			, CanopyValues(CanopyScope->GetComponent(0))
			, HeightValues(HeightScope->GetComponent(0))
			, InterceptionValues(InterceptionScope->GetComponent(0))
			, DisturbanceValues(DisturbanceScope->GetComponent(0))
			, ClassValues(ClassScope->GetComponent(0))
		{
		}

		const FMassifChannelGrid& GetGrid() const
		{
			return CoverScope->GetGrid();
		}

		int32 CellCount() const
		{
			return CoverValues.Num();
		}

		void Write(int32 CellIndex, const FVegetationStand& Stand, const FMassifVegetationParams& Params)
		{
			CoverValues[CellIndex] = Stand.Cover;
			CanopyValues[CellIndex] = Stand.Canopy;
			HeightValues[CellIndex] = Params.MaxCanopyHeightMeters * Stand.Canopy;
			InterceptionValues[CellIndex] = Params.MaxInterceptionFraction * Stand.Canopy;
			DisturbanceValues[CellIndex] = Stand.Disturbance;
			ClassValues[CellIndex] = static_cast<double>(Stand.Class);
		}

		void Commit()
		{
			CoverScope->Commit();
			CanopyScope->Commit();
			HeightScope->Commit();
			InterceptionScope->Commit();
			DisturbanceScope->Commit();
			ClassScope->Commit();
		}

	private:
		TUniquePtr<FMassifChannelWriteScope> CoverScope;
		TUniquePtr<FMassifChannelWriteScope> CanopyScope;
		TUniquePtr<FMassifChannelWriteScope> HeightScope;
		TUniquePtr<FMassifChannelWriteScope> InterceptionScope;
		TUniquePtr<FMassifChannelWriteScope> DisturbanceScope;
		TUniquePtr<FMassifChannelWriteScope> ClassScope;

		TArrayView<double> CoverValues;
		TArrayView<double> CanopyValues;
		TArrayView<double> HeightValues;
		TArrayView<double> InterceptionValues;
		TArrayView<double> DisturbanceValues;
		TArrayView<double> ClassValues;
	};
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
	Declaration.DisplayName = TEXT("Vegetation");
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
	Declaration.OutputChannels.Add(MakeVegetationChannel(
		MassifEnvironmentChannels::Interception(), TEXT("0-1"), Params.CellSizeMeters, FVector2d(0.0, 1.0)));
	Declaration.OutputChannels.Add(MakeVegetationChannel(
		MassifEnvironmentChannels::Disturbance(), TEXT("0-1"), Params.CellSizeMeters, FVector2d(0.0, 1.0)));
	Declaration.OutputChannels.Add(MakeVegetationClassChannel(Params.CellSizeMeters));
	return Declaration;
}

void FVegetationProcess::WriteBare(FMassifEnvironmentContext& Context) const
{
	// Bare is class zero, so the same fill covers the class channel as covers the
	// fractions: nothing grows, nothing is intercepted, and nothing was thrown.
	const FName Names[] = {
		MassifEnvironmentChannels::VegetationCover(), MassifEnvironmentChannels::VegetationCanopy(),
		MassifEnvironmentChannels::VegetationHeight(), MassifEnvironmentChannels::Interception(),
		MassifEnvironmentChannels::Disturbance(), MassifEnvironmentChannels::VegetationClass()};
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
	// holds neither — which is also what leaves the disturbance at nothing.
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

	FVegetationOutputs Outputs(Context);
	const FMassifChannelGrid& Grid = Outputs.GetGrid();
	for (int32 CellIndex = 0; CellIndex < Outputs.CellCount(); ++CellIndex)
	{
		const FVector2d Position = Grid.CellCentreMeters(CellIndex % Grid.Width, CellIndex / Grid.Width);
		const FGrowingConditions Conditions =
			ConditionsAt(Context.GetState(), Inputs, Position, Fidelity);
		Outputs.Write(CellIndex, StandAt(Conditions, Params), Params);
	}

	Outputs.Commit();
}

void MassifRegisterVegetationProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifVegetationParams& Params)
{
	Scheduler.RegisterProcess(MakeShared<FVegetationProcess>(Params));
}
