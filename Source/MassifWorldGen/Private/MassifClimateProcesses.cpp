#include "MassifClimateProcesses.h"

#include "MassifEnvironmentGraph.h"
#include "MassifSnowProcesses.h"
#include "MassifSunProcesses.h"

namespace MassifEnvironmentChannels
{
	const FName& MeanTemperature()
	{
		static const FName Name(TEXT("Climate.MeanTemperature"));
		return Name;
	}

	const FName& SeasonalTemperature()
	{
		static const FName Name(TEXT("Climate.SeasonalTemperature"));
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

	const FName& SeasonalClimate()
	{
		static const FName Name(TEXT("Climate.Seasonal"));
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
			, AnnualInsolation(Context.GetInput(MassifEnvironmentChannels::AnnualInsolation()))
		{
		}

		double AltitudeAt(FVector2d PositionMeters) const
		{
			return State.SampleScalar(Altitude, PositionMeters);
		}

		double AnnualInsolationAt(FVector2d PositionMeters) const
		{
			return State.SampleScalar(AnnualInsolation, PositionMeters);
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
		FMassifChannelHandle AnnualInsolation;
	};

	/**
	 * What the sun adds or takes away against level open ground at the same
	 * altitude. The insolation channel is already a ratio against that ground, so
	 * one is no anomaly at all and zero is the full depression.
	 *
	 * The growing-season mean rather than a half-month, because the temperature
	 * this feeds is itself a growing-season mean.
	 */
	double SolarAnomalyCelsius(const FMassifClimateParams& Params, double AnnualInsolation)
	{
		return Params.InsolationGainCelsius * (AnnualInsolation - 1.0);
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

	/**
	 * How far one half-month stands from the annual mean.
	 *
	 * One cosine over the year, the same everywhere: what makes one slope colder
	 * than another is already in the mean this is added to, and the swing itself is
	 * a property of where on Earth the world sits rather than of its terrain.
	 */
	double SeasonalAnomalyCelsius(const FMassifClimateParams& Params, int32 TimeSlice)
	{
		const double PhaseRadians =
			UE_DOUBLE_TWO_PI * (TimeSlice - Params.WarmestSlice) / MassifSeasonalSliceCount;
		return 0.5 * Params.AnnualTemperatureRangeCelsius * FMath::Cos(PhaseRadians);
	}

	/**
	 * How far a half-month is pulled down by the snow lying on it.
	 *
	 * Measured from bare ground rather than from zero: what cools a slope is the
	 * sunlight the pack throws back over what the ground it covers would have thrown
	 * back anyway. A settled spring pack reflects far less than a fresh fall, so the
	 * cooling weakens as the pack ages — which is part of why it then goes quickly.
	 */
	double AlbedoAnomalyCelsius(const FMassifClimateParams& Params, double Albedo)
	{
		return -Params.SnowAlbedoCoolingCelsius * FMath::Max(0.0, Albedo - Params.BareGroundAlbedo);
	}

	/** Off is a year without seasons: the mean in every slice, so the channel still exists. */
	double SeasonalAnomalyForFidelity(
		const FMassifClimateParams& Params, int32 TimeSlice, EMassifFidelity Fidelity)
	{
		if (Fidelity == EMassifFidelity::Off)
		{
			return 0.0;
		}
		return SeasonalAnomalyCelsius(Params, TimeSlice);
	}

	/** Everything one half-month of the seasonal solve is written from. */
	struct FSeasonalInputs
	{
		FMassifChannelHandle MeanTemperature;

		/** Invalid where no snowpack is in the graph, or where the seasons are switched off. */
		FMassifChannelHandle SnowAlbedo;

		int32 TimeSlice = 0;
		double AnomalyCelsius = 0.0;
	};

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
		MassifEnvironmentChannels::AnnualInsolation()};
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
		Celsius + SolarAnomalyCelsius(Params, Inputs.AnnualInsolationAt(PositionMeters));
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

/**
 * The mean temperature carried round a representative year.
 *
 * Per-slice, not sequential: a slice is a closed form of its own index over the
 * mean, so nothing here carries state forward the way the snowpack that reads it
 * does (ARCHITECTURE §5.6.4). The swing is uniform because everything that makes
 * one slope colder than another is already in the mean it is added to — except
 * the snow, which cools the ground it lies on by throwing back the sunlight that
 * would have warmed it, and which is itself decided by the temperature this
 * writes.
 */
class FSeasonalTemperatureProcess final : public IMassifEnvironmentProcess
{
public:
	explicit FSeasonalTemperatureProcess(const FMassifClimateParams& InParams)
		: Params(InParams)
	{
	}

	FMassifProcessDeclaration Declare() const override;
	void Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const override;

private:
	void WriteSlice(FMassifEnvironmentContext& Context, const FSeasonalInputs& Inputs) const;

	double CelsiusAt(
		const FMassifEnvironmentState& State, const FSeasonalInputs& Inputs, FVector2d PositionMeters) const;

	void SolveRow(
		const FMassifEnvironmentState& State, const FSeasonalInputs& Inputs, const FMassifChannelGrid& Grid,
		int32 Row, TArrayView<double> OutCelsius) const;

	FMassifClimateParams Params;
};

FMassifProcessDeclaration FSeasonalTemperatureProcess::Declare() const
{
	FMassifProcessDeclaration Declaration;
	Declaration.Name = MassifEnvironmentProcesses::SeasonalClimate();
	Declaration.InputChannels = {MassifEnvironmentChannels::MeanTemperature()};

	// Optional, so the temperature still solves in a graph with no snowpack in it:
	// there is simply nothing white anywhere to keep its own ground cold.
	Declaration.OptionalInputChannels = {MassifEnvironmentChannels::SnowAlbedo()};
	Declaration.SupportedFidelities = {
		EMassifFidelity::Cheap, EMassifFidelity::Standard, EMassifFidelity::Reference};

	// The pack reads this temperature and writes the albedo read back above, so the
	// two of them are one cycle and the scheduler has to be told it is meant.
	Declaration.bParticipatesInFeedback = true;

	FMassifChannelDescriptor Descriptor;
	Descriptor.Name = MassifEnvironmentChannels::SeasonalTemperature();
	Descriptor.Type = EMassifChannelType::Scalar;
	Descriptor.Encoding = EMassifChannelEncoding::Uint16;
	Descriptor.CellSizeMeters = Params.CellSizeMeters;
	Descriptor.TimeSliceCount = MassifSeasonalSliceCount;
	Descriptor.Unit = TEXT("C");
	Descriptor.Description = TEXT("Half-monthly air temperature at the ground");
	Descriptor.ColorMap = EMassifChannelColorMap::Magma;
	Declaration.OutputChannels.Add(Descriptor);
	return Declaration;
}

double FSeasonalTemperatureProcess::CelsiusAt(
	const FMassifEnvironmentState& State, const FSeasonalInputs& Inputs, FVector2d PositionMeters) const
{
	const double SeasonalCelsius =
		State.SampleScalar(Inputs.MeanTemperature, PositionMeters) + Inputs.AnomalyCelsius;
	if (!Inputs.SnowAlbedo.IsValid())
	{
		return SeasonalCelsius;
	}

	// Reads as zero until the round of the loop that writes it has run, which is the
	// same thing as no snow lying and needs no special case for the first iteration.
	const double Albedo = State.SampleScalar(Inputs.SnowAlbedo, PositionMeters, Inputs.TimeSlice);
	return SeasonalCelsius + AlbedoAnomalyCelsius(Params, Albedo);
}

void FSeasonalTemperatureProcess::SolveRow(
	const FMassifEnvironmentState& State, const FSeasonalInputs& Inputs, const FMassifChannelGrid& Grid,
	int32 Row, TArrayView<double> OutCelsius) const
{
	for (int32 Column = 0; Column < Grid.Width; ++Column)
	{
		OutCelsius[Row * Grid.Width + Column] = CelsiusAt(State, Inputs, Grid.CellCentreMeters(Column, Row));
	}
}

void FSeasonalTemperatureProcess::WriteSlice(
	FMassifEnvironmentContext& Context, const FSeasonalInputs& Inputs) const
{
	const TUniquePtr<FMassifChannelWriteScope> Scope =
		Context.OpenOutput(MassifEnvironmentChannels::SeasonalTemperature(), Inputs.TimeSlice);

	const FMassifChannelGrid& Grid = Scope->GetGrid();
	TArrayView<double> Celsius = Scope->GetComponent(0);
	for (int32 Row = 0; Row < Grid.Height; ++Row)
	{
		SolveRow(Context.GetState(), Inputs, Grid, Row, Celsius);
	}

	Scope->Commit();
}

void FSeasonalTemperatureProcess::Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const
{
	FSeasonalInputs Inputs;
	Inputs.MeanTemperature = Context.GetInput(MassifEnvironmentChannels::MeanTemperature());
	if (Fidelity != EMassifFidelity::Off)
	{
		Inputs.SnowAlbedo = Context.FindOptionalInput(MassifEnvironmentChannels::SnowAlbedo());
	}

	for (int32 TimeSlice = 0; TimeSlice < MassifSeasonalSliceCount; ++TimeSlice)
	{
		Inputs.TimeSlice = TimeSlice;
		Inputs.AnomalyCelsius = SeasonalAnomalyForFidelity(Params, TimeSlice, Fidelity);
		WriteSlice(Context, Inputs);
	}
}

void MassifRegisterClimateProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifClimateParams& Params)
{
	Scheduler.RegisterProcess(MakeShared<FTemperatureProcess>(Params));
	Scheduler.RegisterProcess(MakeShared<FSeasonalTemperatureProcess>(Params));
}
