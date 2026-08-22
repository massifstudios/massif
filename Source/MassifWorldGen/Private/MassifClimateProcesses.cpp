#include "MassifClimateProcesses.h"

#include "MassifEnvironmentGraph.h"
#include "MassifSunProcesses.h"

namespace MassifEnvironmentChannels
{
	const FName& MeanTemperature()
	{
		static const FName Name(TEXT("Climate.MeanTemperature"));
		return Name;
	}
}

namespace MassifEnvironmentProcesses
{
	const FName& Temperature()
	{
		static const FName Name(TEXT("Climate.Temperature"));
		return Name;
	}
}

namespace
{
	/** The surfaces the temperature is read off, sampled at whatever grid is being solved. */
	class FClimateInputs
	{
	public:
		explicit FClimateInputs(const FMassifEnvironmentContext& Context)
			: State(Context.GetState())
			, Altitude(Context.GetInput(MassifEnvironmentChannels::Altitude()))
			, FilledAltitude(Context.GetInput(MassifEnvironmentChannels::FilledAltitude()))
			, DirectInsolation(Context.GetInput(MassifEnvironmentChannels::DirectInsolation()))
		{
		}

		double AltitudeAt(FVector2d PositionMeters) const
		{
			return State.SampleScalar(Altitude, PositionMeters);
		}

		double DirectInsolationAt(FVector2d PositionMeters) const
		{
			return State.SampleScalar(DirectInsolation, PositionMeters);
		}

		/** How deep a hollow the depression fill had to flood before water could leave here. */
		double BasinDepthAt(FVector2d PositionMeters) const
		{
			return State.SampleScalar(FilledAltitude, PositionMeters) - AltitudeAt(PositionMeters);
		}

	private:
		const FMassifEnvironmentState& State;
		FMassifChannelHandle Altitude;
		FMassifChannelHandle FilledAltitude;
		FMassifChannelHandle DirectInsolation;
	};

	/**
	 * What the sun adds or takes away against level open ground at the same
	 * altitude. The insolation channel is already a ratio against that ground, so
	 * one is no anomaly at all and zero is the full depression.
	 */
	double SolarAnomalyCelsius(const FMassifClimateParams& Params, double DirectInsolation)
	{
		return Params.InsolationGainCelsius * (DirectInsolation - 1.0);
	}

	/**
	 * How far cold air drained into a hollow overnight pulls it below the slope
	 * above it.
	 *
	 * Keyed to the depth the depression fill already computed: the ground that
	 * cannot let water out is the ground that cannot let dense air out either. Deep
	 * enough and this beats the lapse rate, which is the point — a frost hollow is
	 * why a treeline dips into a basin instead of following a contour.
	 */
	double PoolingAnomalyCelsius(const FMassifClimateParams& Params, double BasinDepthMeters)
	{
		const double DepthScaleMeters = FMath::Max(Params.PoolingDepthScaleMeters, UE_DOUBLE_KINDA_SMALL_NUMBER);
		const double Depth = FMath::Max(BasinDepthMeters, 0.0);
		return -Params.PoolingDepressionCelsius * (1.0 - FMath::Exp(-Depth / DepthScaleMeters));
	}

	void WriteConstant(FMassifChannelWriteScope& Scope, double Value)
	{
		TArrayView<double> Scratch = Scope.GetComponent(0);
		for (int32 CellIndex = 0; CellIndex < Scratch.Num(); ++CellIndex)
		{
			Scratch[CellIndex] = Value;
		}
		Scope.Commit();
	}
}

/**
 * Mean growing-season air temperature at the ground.
 *
 * One term per fidelity over the datum the georeference carries: the lapse rate
 * over altitude, then what a day of sun does to a slope, then the cold air that
 * settles in a basin at night. Each is signed against the level open ground the
 * datum describes, so the terms add without any of them needing to know about
 * the others.
 */
class FTemperatureProcess final : public IMassifEnvironmentProcess
{
public:
	explicit FTemperatureProcess(const FMassifClimateParams& InParams)
		: Params(InParams)
	{
	}

	FMassifProcessDeclaration Declare() const override;
	void Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const override;

private:
	double TemperatureAt(
		const FClimateInputs& Inputs, FVector2d PositionMeters, double DatumCelsius,
		EMassifFidelity Fidelity) const;

	void SolveRow(
		const FClimateInputs& Inputs, const FMassifChannelGrid& Grid, int32 Row, double DatumCelsius,
		EMassifFidelity Fidelity, TArrayView<double> OutCelsius) const;

	FMassifClimateParams Params;
};

FMassifProcessDeclaration FTemperatureProcess::Declare() const
{
	FMassifProcessDeclaration Declaration;
	Declaration.Name = MassifEnvironmentProcesses::Temperature();
	Declaration.InputChannels = {
		MassifEnvironmentChannels::Altitude(), MassifEnvironmentChannels::FilledAltitude(),
		MassifEnvironmentChannels::DirectInsolation()};
	Declaration.SupportedFidelities = {
		EMassifFidelity::Cheap, EMassifFidelity::Standard, EMassifFidelity::Reference};

	FMassifChannelDescriptor Descriptor;
	Descriptor.Name = MassifEnvironmentChannels::MeanTemperature();
	Descriptor.Type = EMassifChannelType::Scalar;
	Descriptor.Encoding = EMassifChannelEncoding::Uint16;
	Descriptor.CellSizeMeters = Params.CellSizeMeters;
	Descriptor.Unit = TEXT("C");
	Descriptor.Description = TEXT("Mean growing-season air temperature at the ground");
	Descriptor.ColorMap = EMassifChannelColorMap::Magma;
	Declaration.OutputChannels.Add(Descriptor);
	return Declaration;
}

double FTemperatureProcess::TemperatureAt(
	const FClimateInputs& Inputs, FVector2d PositionMeters, double DatumCelsius,
	EMassifFidelity Fidelity) const
{
	const double Celsius = DatumCelsius - Params.LapseRateCelsiusPerMeter * Inputs.AltitudeAt(PositionMeters);
	if (Fidelity == EMassifFidelity::Cheap)
	{
		return Celsius;
	}

	const double SunlitCelsius =
		Celsius + SolarAnomalyCelsius(Params, Inputs.DirectInsolationAt(PositionMeters));
	if (Fidelity != EMassifFidelity::Reference)
	{
		return SunlitCelsius;
	}

	return SunlitCelsius + PoolingAnomalyCelsius(Params, Inputs.BasinDepthAt(PositionMeters));
}

void FTemperatureProcess::SolveRow(
	const FClimateInputs& Inputs, const FMassifChannelGrid& Grid, int32 Row, double DatumCelsius,
	EMassifFidelity Fidelity, TArrayView<double> OutCelsius) const
{
	for (int32 Column = 0; Column < Grid.Width; ++Column)
	{
		const FVector2d Position = Grid.CellCentreMeters(Column, Row);
		OutCelsius[Row * Grid.Width + Column] = TemperatureAt(Inputs, Position, DatumCelsius, Fidelity);
	}
}

void FTemperatureProcess::Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const
{
	const double DatumCelsius = Context.GetGeoreference().SeaLevelMeanTemperatureCelsius;
	const TUniquePtr<FMassifChannelWriteScope> Scope =
		Context.OpenOutput(MassifEnvironmentChannels::MeanTemperature());

	if (Fidelity == EMassifFidelity::Off)
	{
		// The datum everywhere, altitude and all: the channel still exists, so
		// nothing downstream has to ask whether the climate was solved.
		WriteConstant(*Scope, DatumCelsius);
		return;
	}

	const FClimateInputs Inputs(Context);
	const FMassifChannelGrid& Grid = Scope->GetGrid();
	TArrayView<double> Celsius = Scope->GetComponent(0);
	for (int32 Row = 0; Row < Grid.Height; ++Row)
	{
		SolveRow(Inputs, Grid, Row, DatumCelsius, Fidelity, Celsius);
	}

	Scope->Commit();
}

void MassifRegisterClimateProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifClimateParams& Params)
{
	Scheduler.RegisterProcess(MakeShared<FTemperatureProcess>(Params));
}
