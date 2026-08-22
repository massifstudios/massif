#include "MassifPrecipitationProcesses.h"

#include "MassifClimateProcesses.h"
#include "MassifEnvironmentGraph.h"
#include "MassifWindProcesses.h"

namespace MassifEnvironmentChannels
{
	const FName& AnnualPrecipitation()
	{
		static const FName Name(TEXT("Precip.Annual"));
		return Name;
	}

	const FName& SnowFraction()
	{
		static const FName Name(TEXT("Precip.SnowFraction"));
		return Name;
	}
}

namespace MassifEnvironmentProcesses
{
	const FName& Precipitation()
	{
		static const FName Name(TEXT("Precip.Field"));
		return Name;
	}
}

namespace
{
	/** The channels the solve reads, resolved once and sampled by world position. */
	class FPrecipitationInputs
	{
	public:
		FPrecipitationInputs(const FMassifEnvironmentState& InState, const FMassifEnvironmentContext& Context)
			: State(InState)
			, Altitude(Context.GetInput(MassifEnvironmentChannels::Altitude()))
			, Wind(Context.GetInput(MassifEnvironmentChannels::WindField()))
			, Temperature(Context.GetInput(MassifEnvironmentChannels::MeanTemperature()))
		{
		}

		double AltitudeAt(FVector2d PositionMeters) const
		{
			return State.SampleScalar(Altitude, PositionMeters);
		}

		FVector2d WindAt(FVector2d PositionMeters) const
		{
			return State.SampleVector(Wind, PositionMeters);
		}

		double TemperatureAt(FVector2d PositionMeters) const
		{
			return State.SampleScalar(Temperature, PositionMeters);
		}

		/** Uphill gradient of the ground, differenced across one cell. */
		FVector2d UphillGradientAt(FVector2d PositionMeters, double SpacingMeters) const
		{
			const double East = AltitudeAt(PositionMeters + FVector2d(SpacingMeters, 0.0));
			const double West = AltitudeAt(PositionMeters - FVector2d(SpacingMeters, 0.0));
			const double North = AltitudeAt(PositionMeters + FVector2d(0.0, SpacingMeters));
			const double South = AltitudeAt(PositionMeters - FVector2d(0.0, SpacingMeters));
			return FVector2d((East - West) / (2.0 * SpacingMeters), (North - South) / (2.0 * SpacingMeters));
		}

	private:
		const FMassifEnvironmentState& State;
		FMassifChannelHandle Altitude;
		FMassifChannelHandle Wind;
		FMassifChannelHandle Temperature;
	};

	/**
	 * Vertical speed the terrain forces on the air: the wind's component straight
	 * up the slope it is running into. Positive is climbing, negative descending.
	 */
	double ForcedLiftMetresPerSecond(
		const FPrecipitationInputs& Inputs, FVector2d PositionMeters, double SpacingMeters)
	{
		const FVector2d Wind = Inputs.WindAt(PositionMeters);
		const FVector2d Gradient = Inputs.UphillGradientAt(PositionMeters, SpacingMeters);
		return FVector2d::DotProduct(Wind, Gradient);
	}

	/**
	 * Lift the air already did before it got here, summed back along the wind.
	 *
	 * A rain shadow is not a property of the dry side. It is what the wet side
	 * already took out of the air, so it can only be found by looking upwind.
	 */
	double UpwindLiftIntegral(
		const FPrecipitationInputs& Inputs, FVector2d PositionMeters, const FMassifPrecipitationParams& Params,
		double SpacingMeters)
	{
		const FVector2d Wind = Inputs.WindAt(PositionMeters);
		const FVector2d Upwind = -Wind.GetSafeNormal();
		const int32 StepCount = FMath::Max(1, Params.UpwindStepCount);

		double Integral = 0.0;
		for (int32 Step = 1; Step <= StepCount; ++Step)
		{
			const double DistanceMeters = Params.UpwindFetchMeters * Step / StepCount;
			const double Lift =
				ForcedLiftMetresPerSecond(Inputs, PositionMeters + Upwind * DistanceMeters, SpacingMeters);
			Integral += FMath::Max(0.0, Lift) / StepCount;
		}
		return Integral;
	}

	/** Rain below the turn, snow above it, and a band in between rather than a step. */
	double SnowFractionAt(double TemperatureCelsius, const FMassifPrecipitationParams& Params, bool bBanded)
	{
		if (!bBanded)
		{
			return static_cast<double>(TemperatureCelsius <= Params.FreezingCelsius);
		}

		const double Band = FMath::Max(Params.SnowTransitionCelsius, UE_DOUBLE_KINDA_SMALL_NUMBER);
		const double Above = TemperatureCelsius - Params.FreezingCelsius;
		return FMath::Clamp(0.5 - Above / (2.0 * Band), 0.0, 1.0);
	}

	FMassifChannelDescriptor MakeChannel(
		const FName& Name, const TCHAR* Unit, double CellSizeMeters, EMassifChannelColorMap ColorMap)
	{
		FMassifChannelDescriptor Descriptor;
		Descriptor.Name = Name;
		Descriptor.Type = EMassifChannelType::Scalar;
		Descriptor.Encoding = EMassifChannelEncoding::Uint16;
		Descriptor.CellSizeMeters = CellSizeMeters;
		Descriptor.Unit = Unit;
		Descriptor.ColorMap = ColorMap;
		return Descriptor;
	}
}

/**
 * What falls on the world over a year, and how much of it falls as snow.
 *
 * Precipitation over a massif is mostly not about the massif's own weather: it
 * is about what the air was carrying when it arrived and what the ground made
 * it do on the way in. Air forced up a windward slope drops its water before
 * the crest, and everything behind that crest is dry for exactly that reason.
 */
class FPrecipitationProcess final : public IMassifEnvironmentProcess
{
public:
	explicit FPrecipitationProcess(const FMassifPrecipitationParams& InParams)
		: Params(InParams)
	{
	}

	FMassifProcessDeclaration Declare() const override;
	void Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const override;

private:
	void WriteUniform(FMassifEnvironmentContext& Context) const;

	void SolveRow(
		const FPrecipitationInputs& Inputs, const FMassifChannelGrid& Grid, int32 Row, EMassifFidelity Fidelity,
		int32 UpwindStepCount, TArrayView<double> OutAnnual, TArrayView<double> OutSnowFraction) const;

	double AnnualMillimetresAt(
		const FPrecipitationInputs& Inputs, FVector2d PositionMeters, double SpacingMeters,
		EMassifFidelity Fidelity, int32 UpwindStepCount) const;

	FMassifPrecipitationParams Params;
};

FMassifProcessDeclaration FPrecipitationProcess::Declare() const
{
	FMassifProcessDeclaration Declaration;
	Declaration.Name = MassifEnvironmentProcesses::Precipitation();
	Declaration.InputChannels = {
		MassifEnvironmentChannels::Altitude(), MassifEnvironmentChannels::WindField(),
		MassifEnvironmentChannels::MeanTemperature()};
	Declaration.SupportedFidelities = {
		EMassifFidelity::Cheap, EMassifFidelity::Standard, EMassifFidelity::Reference};

	Declaration.OutputChannels.Add(MakeChannel(
		MassifEnvironmentChannels::AnnualPrecipitation(), TEXT("mm/yr"), Params.CellSizeMeters,
		EMassifChannelColorMap::BlueWhite));

	FMassifChannelDescriptor Snow = MakeChannel(
		MassifEnvironmentChannels::SnowFraction(), TEXT("0-1"), Params.CellSizeMeters,
		EMassifChannelColorMap::BlueWhite);
	Snow.DisplayRange = FVector2d(0.0, 1.0);
	Declaration.OutputChannels.Add(Snow);
	return Declaration;
}

void FPrecipitationProcess::WriteUniform(FMassifEnvironmentContext& Context) const
{
	// Off is flat weather: the same rain everywhere and none of it frozen. The
	// channels still exist, so nothing downstream asks whether it rained.
	const TUniquePtr<FMassifChannelWriteScope> Annual =
		Context.OpenOutput(MassifEnvironmentChannels::AnnualPrecipitation());
	const TUniquePtr<FMassifChannelWriteScope> Snow =
		Context.OpenOutput(MassifEnvironmentChannels::SnowFraction());

	TArrayView<double> AnnualValues = Annual->GetComponent(0);
	TArrayView<double> SnowValues = Snow->GetComponent(0);
	for (int32 CellIndex = 0; CellIndex < AnnualValues.Num(); ++CellIndex)
	{
		AnnualValues[CellIndex] = Params.BaseAnnualMillimetres;
		SnowValues[CellIndex] = 0.0;
	}

	Annual->Commit();
	Snow->Commit();
}

double FPrecipitationProcess::AnnualMillimetresAt(
	const FPrecipitationInputs& Inputs, FVector2d PositionMeters, double SpacingMeters, EMassifFidelity Fidelity,
	int32 UpwindStepCount) const
{
	const double AltitudeMeters = Inputs.AltitudeAt(PositionMeters);
	const double Millimetres = Params.BaseAnnualMillimetres
		+ Params.AltitudeGainMillimetresPerKilometre * AltitudeMeters / 1000.0;
	if (Fidelity == EMassifFidelity::Cheap)
	{
		return FMath::Clamp(Millimetres, 0.0, Params.MaxAnnualMillimetres);
	}

	const double Lift = ForcedLiftMetresPerSecond(Inputs, PositionMeters, SpacingMeters);
	const double Orographic = Params.OrographicGainMillimetresPerLift * FMath::Max(0.0, Lift);
	const double Descending = FMath::Max(0.0, -Lift);
	const double LeeFactor = 1.0 - Params.LeeLossFraction * FMath::Min(1.0, Descending);

	FMassifPrecipitationParams FetchParams = Params;
	FetchParams.UpwindStepCount = UpwindStepCount;
	const double Shadow =
		FMath::Exp(-Params.DepletionCoefficient * UpwindLiftIntegral(Inputs, PositionMeters, FetchParams, SpacingMeters));

	return FMath::Clamp((Millimetres + Orographic) * LeeFactor * Shadow, 0.0, Params.MaxAnnualMillimetres);
}

void FPrecipitationProcess::SolveRow(
	const FPrecipitationInputs& Inputs, const FMassifChannelGrid& Grid, int32 Row, EMassifFidelity Fidelity,
	int32 UpwindStepCount, TArrayView<double> OutAnnual, TArrayView<double> OutSnowFraction) const
{
	const bool bBanded = Fidelity != EMassifFidelity::Cheap;
	for (int32 Column = 0; Column < Grid.Width; ++Column)
	{
		const FVector2d Position = Grid.CellCentreMeters(Column, Row);
		const int32 CellIndex = Row * Grid.Width + Column;
		OutAnnual[CellIndex] =
			AnnualMillimetresAt(Inputs, Position, Grid.CellSizeMeters, Fidelity, UpwindStepCount);
		OutSnowFraction[CellIndex] = SnowFractionAt(Inputs.TemperatureAt(Position), Params, bBanded);
	}
}

void FPrecipitationProcess::Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const
{
	if (Fidelity == EMassifFidelity::Off)
	{
		WriteUniform(Context);
		return;
	}

	const FPrecipitationInputs Inputs(Context.GetState(), Context);

	int32 UpwindStepCount = Params.UpwindStepCount;
	if (Fidelity == EMassifFidelity::Reference)
	{
		UpwindStepCount = Params.UpwindStepCount * 2;
	}

	const TUniquePtr<FMassifChannelWriteScope> Annual =
		Context.OpenOutput(MassifEnvironmentChannels::AnnualPrecipitation());
	const TUniquePtr<FMassifChannelWriteScope> Snow =
		Context.OpenOutput(MassifEnvironmentChannels::SnowFraction());

	const FMassifChannelGrid& Grid = Annual->GetGrid();
	for (int32 Row = 0; Row < Grid.Height; ++Row)
	{
		SolveRow(
			Inputs, Grid, Row, Fidelity, UpwindStepCount, Annual->GetComponent(0), Snow->GetComponent(0));
	}

	Annual->Commit();
	Snow->Commit();
}

void MassifRegisterPrecipitationProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifPrecipitationParams& Params)
{
	Scheduler.RegisterProcess(MakeShared<FPrecipitationProcess>(Params));
}
