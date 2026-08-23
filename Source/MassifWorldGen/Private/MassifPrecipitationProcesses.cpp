#include "MassifPrecipitationProcesses.h"

#include "MassifClimateProcesses.h"
#include "MassifEnvironmentGraph.h"
#include "MassifVegetationProcesses.h"
#include "MassifWindProcesses.h"

namespace MassifEnvironmentChannels
{
	const FName& AnnualPrecipitation()
	{
		static const FName Name(TEXT("Precip.Annual"));
		return Name;
	}

	const FName& SeasonalPrecipitation()
	{
		static const FName Name(TEXT("Precip.Seasonal"));
		return Name;
	}

	const FName& SnowFraction()
	{
		static const FName Name(TEXT("Precip.SnowFraction"));
		return Name;
	}

	const FName& Evapotranspiration()
	{
		static const FName Name(TEXT("Climate.Evapotranspiration"));
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
			, SeasonalTemperature(Context.GetInput(MassifEnvironmentChannels::SeasonalTemperature()))
			, VegetationCover(Context.FindOptionalInput(MassifEnvironmentChannels::VegetationCover()))
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

		double SeasonalTemperatureAt(FVector2d PositionMeters, int32 TimeSlice) const
		{
			return State.SampleScalar(SeasonalTemperature, PositionMeters, TimeSlice);
		}

		/** Bare ground where no vegetation is in the graph, and until the loop has grown any. */
		double VegetationCoverAt(FVector2d PositionMeters) const
		{
			if (!VegetationCover.IsValid())
			{
				return 0.0;
			}
			return State.SampleScalar(VegetationCover, PositionMeters);
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
		FMassifChannelHandle SeasonalTemperature;
		FMassifChannelHandle VegetationCover;
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

	/** One cosine over the year, peaking at the wettest slice and never going negative. */
	double SeasonalWeight(const FMassifPrecipitationParams& Params, int32 TimeSlice)
	{
		const double Amplitude = FMath::Clamp(Params.SeasonalAmplitude, 0.0, 1.0);
		const double PhaseRadians = UE_DOUBLE_TWO_PI * (TimeSlice - Params.WettestSlice) / MassifSeasonalSliceCount;
		return 1.0 + Amplitude * FMath::Cos(PhaseRadians);
	}

	/**
	 * Share of the year's water falling in one half-month.
	 *
	 * Normalised over the whole year rather than trusted to sum: the shape decides
	 * when the water arrives and must never decide how much of it there is, so
	 * whatever the amplitude, the slices still add up to the annual total the
	 * climatology and everything reading it agreed on.
	 */
	double SeasonalShare(const FMassifPrecipitationParams& Params, int32 TimeSlice)
	{
		double YearWeight = 0.0;
		for (int32 Slice = 0; Slice < MassifSeasonalSliceCount; ++Slice)
		{
			YearWeight += SeasonalWeight(Params, Slice);
		}
		return SeasonalWeight(Params, TimeSlice) / YearWeight;
	}

	/** Below Standard the year is flat: the same fall every half-month, still summing to the total. */
	double SeasonalShareForFidelity(
		const FMassifPrecipitationParams& Params, int32 TimeSlice, EMassifFidelity Fidelity)
	{
		if (Fidelity == EMassifFidelity::Off || Fidelity == EMassifFidelity::Cheap)
		{
			return 1.0 / MassifSeasonalSliceCount;
		}
		return SeasonalShare(Params, TimeSlice);
	}

	/**
	 * What a year would take back off ground with water to spare.
	 *
	 * Thornthwaite's form: temperature stands in for the energy available, because
	 * the humidity, radiation and wind speed a Penman-Monteith form needs are not
	 * things this graph holds. Frozen ground returns nothing.
	 */
	double PotentialEvapotranspirationMillimetres(
		const FMassifPrecipitationParams& Params, double TemperatureCelsius)
	{
		const double ReferenceCelsius =
			FMath::Max(Params.EvapotranspirationReferenceCelsius, UE_DOUBLE_KINDA_SMALL_NUMBER);
		const double AboveBase = FMath::Max(0.0, TemperatureCelsius - Params.EvapotranspirationBaseCelsius);
		return Params.ReferenceEvapotranspirationMillimetres * AboveBase / ReferenceCelsius;
	}

	/** Rock evaporates what is on it and transpires nothing; a closed canopy does both. */
	double VegetationEvaporationScale(const FMassifPrecipitationParams& Params, double VegetationCover)
	{
		const double BareFraction = FMath::Clamp(Params.BareGroundEvaporationFraction, 0.0, 1.0);
		return BareFraction + (1.0 - BareFraction) * FMath::Clamp(VegetationCover, 0.0, 1.0);
	}

	/**
	 * What the air actually gets, which is the potential limited twice over: by the
	 * water that fell here, and by whether anything is growing to move it.
	 */
	double ActualEvapotranspirationMillimetres(
		const FMassifPrecipitationParams& Params, double TemperatureCelsius, double AnnualMillimetres,
		double VegetationCover, EMassifFidelity Fidelity)
	{
		const double Potential = PotentialEvapotranspirationMillimetres(Params, TemperatureCelsius);
		const double Supplied = FMath::Min(Potential, FMath::Max(0.0, AnnualMillimetres));
		if (Fidelity == EMassifFidelity::Cheap)
		{
			return Supplied;
		}
		return Supplied * VegetationEvaporationScale(Params, VegetationCover);
	}

	void ScaleInto(TArrayView<const double> Values, double Scale, TArrayView<double> OutValues)
	{
		for (int32 CellIndex = 0; CellIndex < OutValues.Num(); ++CellIndex)
		{
			OutValues[CellIndex] = Values[CellIndex] * Scale;
		}
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
 * What falls on the world over a year, when it falls, how much of it falls as
 * snow, and what the air takes back.
 *
 * Precipitation over a massif is mostly not about the massif's own weather: it
 * is about what the air was carrying when it arrived and what the ground made
 * it do on the way in. Air forced up a windward slope drops its water before
 * the crest, and everything behind that crest is dry for exactly that reason.
 *
 * The annual total is a climatology and stays one. Everything half-monthly
 * hangs off it: a share of the year's water per slice, and the phase that share
 * falls in, which is a property of the day rather than of the place.
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

	/** The annual field, returned as well as written: everything else is a share of it. */
	TArray<double> WriteAnnual(
		FMassifEnvironmentContext& Context, const FPrecipitationInputs& Inputs, EMassifFidelity Fidelity,
		int32 UpwindStepCount) const;

	void WriteSeasonal(
		FMassifEnvironmentContext& Context, TArrayView<const double> AnnualMillimetres,
		EMassifFidelity Fidelity) const;

	void WriteSnowFraction(
		FMassifEnvironmentContext& Context, const FPrecipitationInputs& Inputs, EMassifFidelity Fidelity) const;

	void WriteSnowFractionSlice(
		FMassifEnvironmentContext& Context, const FPrecipitationInputs& Inputs, int32 TimeSlice,
		EMassifFidelity Fidelity) const;

	void WriteEvapotranspiration(
		FMassifEnvironmentContext& Context, const FPrecipitationInputs& Inputs,
		TArrayView<const double> AnnualMillimetres, EMassifFidelity Fidelity) const;

	void SolveAnnualRow(
		const FPrecipitationInputs& Inputs, const FMassifChannelGrid& Grid, int32 Row, EMassifFidelity Fidelity,
		int32 UpwindStepCount, TArrayView<double> OutAnnual) const;

	void SolveSnowFractionRow(
		const FPrecipitationInputs& Inputs, const FMassifChannelGrid& Grid, int32 Row, int32 TimeSlice,
		EMassifFidelity Fidelity, TArrayView<double> OutSnowFraction) const;

	void SolveEvapotranspirationRow(
		const FPrecipitationInputs& Inputs, const FMassifChannelGrid& Grid, int32 Row,
		TArrayView<const double> AnnualMillimetres, EMassifFidelity Fidelity,
		TArrayView<double> OutMillimetres) const;

	double AnnualMillimetresAt(
		const FPrecipitationInputs& Inputs, FVector2d PositionMeters, double SpacingMeters,
		EMassifFidelity Fidelity, int32 UpwindStepCount) const;

	FMassifPrecipitationParams Params;
};

FMassifProcessDeclaration FPrecipitationProcess::Declare() const
{
	FMassifProcessDeclaration Declaration;
	Declaration.Name = MassifEnvironmentProcesses::Precipitation();
	Declaration.DisplayName = TEXT("Rain and Snowfall");
	Declaration.InputChannels = {
		MassifEnvironmentChannels::Altitude(), MassifEnvironmentChannels::WindField(),
		MassifEnvironmentChannels::MeanTemperature(), MassifEnvironmentChannels::SeasonalTemperature()};

	// What is growing decides how much of the rain goes back into the air, but
	// vegetation reads the water balance this closes: optional, so precipitation
	// solves in a graph with nothing growing in it and simply evaporates off bare
	// ground everywhere.
	Declaration.OptionalInputChannels = {MassifEnvironmentChannels::VegetationCover()};
	Declaration.SupportedFidelities = {
		EMassifFidelity::Cheap, EMassifFidelity::Standard, EMassifFidelity::Reference};

	// The wind it reads is slowed by a canopy that grows on the snow this feeds, so
	// precipitation sits inside the loop whether or not it closes one itself.
	Declaration.bParticipatesInFeedback = true;

	Declaration.OutputChannels.Add(MakeChannel(
		MassifEnvironmentChannels::AnnualPrecipitation(), TEXT("mm/yr"), Params.CellSizeMeters,
		EMassifChannelColorMap::BlueWhite));

	FMassifChannelDescriptor Seasonal = MakeChannel(
		MassifEnvironmentChannels::SeasonalPrecipitation(), TEXT("mm"), Params.CellSizeMeters,
		EMassifChannelColorMap::BlueWhite);
	Seasonal.TimeSliceCount = MassifSeasonalSliceCount;
	Seasonal.Description = TEXT("Millimetres falling in one half-month");
	Declaration.OutputChannels.Add(Seasonal);

	FMassifChannelDescriptor Snow = MakeChannel(
		MassifEnvironmentChannels::SnowFraction(), TEXT("0-1"), Params.CellSizeMeters,
		EMassifChannelColorMap::BlueWhite);
	Snow.TimeSliceCount = MassifSeasonalSliceCount;
	Snow.DisplayRange = FVector2d(0.0, 1.0);
	Snow.Description = TEXT("Share of one half-month's fall arriving as snow");
	Declaration.OutputChannels.Add(Snow);

	FMassifChannelDescriptor Evaporated = MakeChannel(
		MassifEnvironmentChannels::Evapotranspiration(), TEXT("mm/yr"), Params.CellSizeMeters,
		EMassifChannelColorMap::Magma);
	Evaporated.Description = TEXT("Water a year returns to the air off the ground and its plants");
	Declaration.OutputChannels.Add(Evaporated);
	return Declaration;
}

void FPrecipitationProcess::WriteUniform(FMassifEnvironmentContext& Context) const
{
	// Off is flat weather: the same rain every half-month, everywhere, and none of
	// it going back into the air.
	MassifFillChannelSlice(Context, MassifEnvironmentChannels::AnnualPrecipitation(), 0, Params.BaseAnnualMillimetres);
	MassifFillChannelSlice(Context, MassifEnvironmentChannels::Evapotranspiration(), 0, 0.0);

	const double SliceMillimetres = Params.BaseAnnualMillimetres / MassifSeasonalSliceCount;
	for (int32 TimeSlice = 0; TimeSlice < MassifSeasonalSliceCount; ++TimeSlice)
	{
		MassifFillChannelSlice(Context, MassifEnvironmentChannels::SeasonalPrecipitation(), TimeSlice, SliceMillimetres);
	}

	// The phase is not part of what Off switches off. Writing "none of it is
	// frozen" would not be a trivial value, it would be a false one: the snowpack
	// reads this channel, so a world twenty degrees below freezing would be told no
	// snow ever falls on it. What Off drops is the structure - the orographic gain,
	// the rain shadow, the band around the turn - not the fact that cold rain is
	// snow.
	const FPrecipitationInputs Inputs(Context.GetState(), Context);
	WriteSnowFraction(Context, Inputs, EMassifFidelity::Cheap);
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

void FPrecipitationProcess::SolveAnnualRow(
	const FPrecipitationInputs& Inputs, const FMassifChannelGrid& Grid, int32 Row, EMassifFidelity Fidelity,
	int32 UpwindStepCount, TArrayView<double> OutAnnual) const
{
	for (int32 Column = 0; Column < Grid.Width; ++Column)
	{
		const FVector2d Position = Grid.CellCentreMeters(Column, Row);
		OutAnnual[Row * Grid.Width + Column] =
			AnnualMillimetresAt(Inputs, Position, Grid.CellSizeMeters, Fidelity, UpwindStepCount);
	}
}

TArray<double> FPrecipitationProcess::WriteAnnual(
	FMassifEnvironmentContext& Context, const FPrecipitationInputs& Inputs, EMassifFidelity Fidelity,
	int32 UpwindStepCount) const
{
	const TUniquePtr<FMassifChannelWriteScope> Annual =
		Context.OpenOutput(MassifEnvironmentChannels::AnnualPrecipitation());

	const FMassifChannelGrid& Grid = Annual->GetGrid();
	TArrayView<double> Millimetres = Annual->GetComponent(0);
	for (int32 Row = 0; Row < Grid.Height; ++Row)
	{
		SolveAnnualRow(Inputs, Grid, Row, Fidelity, UpwindStepCount, Millimetres);
	}

	TArray<double> AnnualMillimetres(Millimetres.GetData(), Millimetres.Num());
	Annual->Commit();
	return AnnualMillimetres;
}

void FPrecipitationProcess::WriteSeasonal(
	FMassifEnvironmentContext& Context, TArrayView<const double> AnnualMillimetres,
	EMassifFidelity Fidelity) const
{
	// Every channel here is solved on the same grid, so a slice is the annual field
	// scaled cell by cell and nothing has to be resampled to write one.
	for (int32 TimeSlice = 0; TimeSlice < MassifSeasonalSliceCount; ++TimeSlice)
	{
		const double Share = SeasonalShareForFidelity(Params, TimeSlice, Fidelity);
		const TUniquePtr<FMassifChannelWriteScope> Scope =
			Context.OpenOutput(MassifEnvironmentChannels::SeasonalPrecipitation(), TimeSlice);
		ScaleInto(AnnualMillimetres, Share, Scope->GetComponent(0));
		Scope->Commit();
	}
}

void FPrecipitationProcess::SolveSnowFractionRow(
	const FPrecipitationInputs& Inputs, const FMassifChannelGrid& Grid, int32 Row, int32 TimeSlice,
	EMassifFidelity Fidelity, TArrayView<double> OutSnowFraction) const
{
	const bool bBanded = Fidelity != EMassifFidelity::Cheap;
	for (int32 Column = 0; Column < Grid.Width; ++Column)
	{
		const FVector2d Position = Grid.CellCentreMeters(Column, Row);
		OutSnowFraction[Row * Grid.Width + Column] =
			SnowFractionAt(Inputs.SeasonalTemperatureAt(Position, TimeSlice), Params, bBanded);
	}
}

void FPrecipitationProcess::WriteSnowFractionSlice(
	FMassifEnvironmentContext& Context, const FPrecipitationInputs& Inputs, int32 TimeSlice,
	EMassifFidelity Fidelity) const
{
	const TUniquePtr<FMassifChannelWriteScope> Scope =
		Context.OpenOutput(MassifEnvironmentChannels::SnowFraction(), TimeSlice);

	const FMassifChannelGrid& Grid = Scope->GetGrid();
	TArrayView<double> Fraction = Scope->GetComponent(0);
	for (int32 Row = 0; Row < Grid.Height; ++Row)
	{
		SolveSnowFractionRow(Inputs, Grid, Row, TimeSlice, Fidelity, Fraction);
	}

	Scope->Commit();
}

void FPrecipitationProcess::WriteSnowFraction(
	FMassifEnvironmentContext& Context, const FPrecipitationInputs& Inputs, EMassifFidelity Fidelity) const
{
	// Off the half-monthly temperature rather than the annual mean: the phase is a
	// property of the day it fell on, and a slope that takes rain in July takes
	// snow in January.
	for (int32 TimeSlice = 0; TimeSlice < MassifSeasonalSliceCount; ++TimeSlice)
	{
		WriteSnowFractionSlice(Context, Inputs, TimeSlice, Fidelity);
	}
}

void FPrecipitationProcess::SolveEvapotranspirationRow(
	const FPrecipitationInputs& Inputs, const FMassifChannelGrid& Grid, int32 Row,
	TArrayView<const double> AnnualMillimetres, EMassifFidelity Fidelity,
	TArrayView<double> OutMillimetres) const
{
	for (int32 Column = 0; Column < Grid.Width; ++Column)
	{
		const FVector2d Position = Grid.CellCentreMeters(Column, Row);
		const int32 CellIndex = Row * Grid.Width + Column;
		OutMillimetres[CellIndex] = ActualEvapotranspirationMillimetres(
			Params, Inputs.TemperatureAt(Position), AnnualMillimetres[CellIndex],
			Inputs.VegetationCoverAt(Position), Fidelity);
	}
}

void FPrecipitationProcess::WriteEvapotranspiration(
	FMassifEnvironmentContext& Context, const FPrecipitationInputs& Inputs,
	TArrayView<const double> AnnualMillimetres, EMassifFidelity Fidelity) const
{
	const TUniquePtr<FMassifChannelWriteScope> Scope =
		Context.OpenOutput(MassifEnvironmentChannels::Evapotranspiration());

	const FMassifChannelGrid& Grid = Scope->GetGrid();
	TArrayView<double> Millimetres = Scope->GetComponent(0);
	for (int32 Row = 0; Row < Grid.Height; ++Row)
	{
		SolveEvapotranspirationRow(Inputs, Grid, Row, AnnualMillimetres, Fidelity, Millimetres);
	}

	Scope->Commit();
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

	const TArray<double> AnnualMillimetres = WriteAnnual(Context, Inputs, Fidelity, UpwindStepCount);
	WriteSeasonal(Context, AnnualMillimetres, Fidelity);
	WriteSnowFraction(Context, Inputs, Fidelity);
	WriteEvapotranspiration(Context, Inputs, AnnualMillimetres, Fidelity);
}

void MassifRegisterPrecipitationProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifPrecipitationParams& Params)
{
	Scheduler.RegisterProcess(MakeShared<FPrecipitationProcess>(Params));
}
