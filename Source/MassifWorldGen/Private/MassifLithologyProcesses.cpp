#include "MassifLithologyProcesses.h"

#include "MassifClimateProcesses.h"
#include "MassifEnvironmentGraph.h"
#include "MassifSeed.h"
#include "MassifValueNoise.h"

namespace MassifEnvironmentChannels
{
	const FName& RockHardness()
	{
		static const FName Name(TEXT("Geology.RockHardness"));
		return Name;
	}

	const FName& SoilYield()
	{
		static const FName Name(TEXT("Geology.SoilYield"));
		return Name;
	}

	const FName& ScreeProduction()
	{
		static const FName Name(TEXT("Geology.ScreeProduction"));
		return Name;
	}
}

namespace MassifEnvironmentProcesses
{
	const FName& Lithology()
	{
		static const FName Name(TEXT("Geology.Lithology"));
		return Name;
	}
}

namespace
{
	/** Half a turn covers every bearing: a strike and its reverse are the same strike. */
	double StrikeBearingFromSeed(uint32 StrikeSeed)
	{
		return 180.0 * StrikeSeed / static_cast<double>(TNumericLimits<uint32>::Max());
	}

	/** The frame the bands are laid out in: one axis across the strike, one along it. */
	struct FStrikeFrame
	{
		FVector2d AcrossStrike = FVector2d(1.0, 0.0);
		FVector2d AlongStrike = FVector2d(0.0, 1.0);

		static FStrikeFrame FromBearing(double BearingDegrees)
		{
			const double BearingRadians = FMath::DegreesToRadians(BearingDegrees);

			FStrikeFrame Frame;
			Frame.AlongStrike = FVector2d(FMath::Sin(BearingRadians), FMath::Cos(BearingRadians));
			Frame.AcrossStrike = FVector2d(Frame.AlongStrike.Y, -Frame.AlongStrike.X);
			return Frame;
		}

		FVector2d ProjectMeters(FVector2d PositionMeters) const
		{
			return FVector2d(
				FVector2d::DotProduct(PositionMeters, AcrossStrike),
				FVector2d::DotProduct(PositionMeters, AlongStrike));
		}
	};

	/**
	 * Lattice position of the broad bands: one wavelength across the strike, many
	 * along it, so the noise comes out stretched into beds rather than into blobs.
	 */
	FVector2d BandLatticePosition(
		const FStrikeFrame& Frame, FVector2d PositionMeters, const FMassifLithologyParams& Params)
	{
		const double WavelengthMeters = FMath::Max(Params.BandWavelengthMeters, UE_DOUBLE_KINDA_SMALL_NUMBER);
		const double Elongation = FMath::Max(Params.BandElongation, 1.0);

		const FVector2d Projected = Frame.ProjectMeters(PositionMeters);
		return FVector2d(Projected.X / WavelengthMeters, Projected.Y / (WavelengthMeters * Elongation));
	}

	/** A finer scale, signed and unrotated, so it pushes a band either way and in every direction. */
	double PatchOffset(uint32 Seed, FVector2d PositionMeters, double WavelengthMeters, double Amplitude)
	{
		const double Wavelength = FMath::Max(WavelengthMeters, UE_DOUBLE_KINDA_SMALL_NUMBER);
		return Amplitude * (MassifNoise::ValueNoise(Seed, PositionMeters / Wavelength) - 0.5);
	}

	/** One rock property over the world: broad bands along the strike, roughened at finer scales. */
	struct FBandedField
	{
		uint32 BandSeed = 0;
		uint32 PatchSeed = 0;
		uint32 DetailSeed = 0;

		double ValueAt(
			FVector2d PositionMeters, const FStrikeFrame& Frame, const FMassifLithologyParams& Params,
			EMassifFidelity Fidelity) const;
	};

	double FBandedField::ValueAt(
		FVector2d PositionMeters, const FStrikeFrame& Frame, const FMassifLithologyParams& Params,
		EMassifFidelity Fidelity) const
	{
		const double Band = MassifNoise::ValueNoise(BandSeed, BandLatticePosition(Frame, PositionMeters, Params));
		if (Fidelity == EMassifFidelity::Cheap)
		{
			return Band;
		}

		const double Patched = Band
			+ PatchOffset(PatchSeed, PositionMeters, Params.PatchWavelengthMeters, Params.PatchAmplitude);
		if (Fidelity != EMassifFidelity::Reference)
		{
			return FMath::Clamp(Patched, 0.0, 1.0);
		}

		// Reference resolves one scale further down at half the weight. Same model.
		const double Detailed = Patched
			+ PatchOffset(
				DetailSeed, PositionMeters, 0.5 * Params.PatchWavelengthMeters, 0.5 * Params.PatchAmplitude);
		return FMath::Clamp(Detailed, 0.0, 1.0);
	}

	/** One field's three octaves, each seeded from its own purpose so none can shift another. */
	FBandedField MakeBandedField(
		const FMassifEnvironmentContext& Context, const TCHAR* BandPurpose, const TCHAR* PatchPurpose,
		const TCHAR* DetailPurpose)
	{
		FBandedField Field;
		Field.BandSeed = Context.MakeSeed(BandPurpose);
		Field.PatchSeed = Context.MakeSeed(PatchPurpose);
		Field.DetailSeed = Context.MakeSeed(DetailPurpose);
		return Field;
	}

	/**
	 * Where a point stands between the lowest and the highest ground in the world.
	 *
	 * Relative rather than absolute, because what a massif's summits are made of is
	 * a statement about that massif: an absolute altitude would say nothing at all
	 * about a range whose peaks are all below two thousand metres. Ground the graph
	 * cannot rank at all sits halfway, which is what makes the hardening term
	 * vanish rather than bias the world one way.
	 */
	class FAltitudeRank
	{
	public:
		FAltitudeRank(const FMassifEnvironmentState& InState, FMassifChannelHandle InAltitude)
			: State(InState)
			, Altitude(InAltitude)
		{
			if (!Altitude.IsValid())
			{
				return;
			}
			LowestMeters = State.GetChannelMinimum(Altitude);
			SpanMeters = State.GetChannelMaximum(Altitude) - LowestMeters;
		}

		double At(FVector2d PositionMeters) const
		{
			if (!Altitude.IsValid() || SpanMeters <= UE_DOUBLE_KINDA_SMALL_NUMBER)
			{
				return 0.5;
			}
			return (State.SampleScalar(Altitude, PositionMeters) - LowestMeters) / SpanMeters;
		}

	private:
		const FMassifEnvironmentState& State;
		FMassifChannelHandle Altitude;
		double LowestMeters = 0.0;
		double SpanMeters = 0.0;
	};

	/**
	 * Share of the peak frost-shattering rate a mean temperature sees.
	 *
	 * Peaked at freezing rather than at the cold end: it is water changing phase in
	 * a crack that levers rock apart, so permanently frozen ground and permanently
	 * thawed ground both shed very little, and the band between them does the work.
	 */
	double ShatteringFraction(double TemperatureCelsius, const FMassifLithologyParams& Params)
	{
		const double BandCelsius = FMath::Max(Params.ShatteringBandCelsius, UE_DOUBLE_KINDA_SMALL_NUMBER);
		const double Offset = (TemperatureCelsius - Params.PeakShatteringTemperatureCelsius) / BandCelsius;
		return FMath::Exp(-Offset * Offset);
	}

	/** Everything one run reads, resolved once instead of per cell. */
	class FLithologyInputs
	{
	public:
		FLithologyInputs(const FMassifEnvironmentContext& Context, const FMassifLithologyParams& InParams);

		double RockHardnessAt(FVector2d PositionMeters, EMassifFidelity Fidelity) const;
		double SoilYieldAt(FVector2d PositionMeters, double RockHardness, EMassifFidelity Fidelity) const;
		double ScreeMetresPerMillenniumAt(
			FVector2d PositionMeters, double RockHardness, EMassifFidelity Fidelity) const;

	private:
		double ShatteringShareAt(FVector2d PositionMeters, EMassifFidelity Fidelity) const;

		const FMassifEnvironmentState& State;
		FMassifLithologyParams Params;
		FStrikeFrame Strike;
		FBandedField HardnessField;
		FBandedField SoilField;
		FAltitudeRank AltitudeRank;
		FMassifChannelHandle Temperature;
	};

	FLithologyInputs::FLithologyInputs(
		const FMassifEnvironmentContext& Context, const FMassifLithologyParams& InParams)
		: State(Context.GetState())
		, Params(InParams)
		, Strike(FStrikeFrame::FromBearing(StrikeBearingFromSeed(Context.MakeSeed(TEXT("Strike")))))
		, HardnessField(
			MakeBandedField(Context, TEXT("HardnessBand"), TEXT("HardnessPatch"), TEXT("HardnessDetail")))
		, SoilField(MakeBandedField(Context, TEXT("SoilBand"), TEXT("SoilPatch"), TEXT("SoilDetail")))
		, AltitudeRank(Context.GetState(), Context.FindOptionalInput(MassifEnvironmentChannels::Altitude()))
		, Temperature(Context.FindOptionalInput(MassifEnvironmentChannels::MeanTemperature()))
	{
	}

	double FLithologyInputs::RockHardnessAt(FVector2d PositionMeters, EMassifFidelity Fidelity) const
	{
		const double Banded = HardnessField.ValueAt(PositionMeters, Strike, Params, Fidelity);
		if (Fidelity == EMassifFidelity::Cheap)
		{
			return Banded;
		}

		const double Hardening = Params.AltitudeHardening * (AltitudeRank.At(PositionMeters) - 0.5);
		return FMath::Clamp(Banded + Hardening, 0.0, 1.0);
	}

	double FLithologyInputs::SoilYieldAt(
		FVector2d PositionMeters, double RockHardness, EMassifFidelity Fidelity) const
	{
		// Broadly the inverse of the hardness, pulled part of the way towards a field
		// of its own: hard rock that weathers deep and soft rock that gives nothing
		// both exist, so the two must not be locked together.
		const double OwnValue = SoilField.ValueAt(PositionMeters, Strike, Params, Fidelity);
		return FMath::Clamp(FMath::Lerp(1.0 - RockHardness, OwnValue, Params.SoilIndependence), 0.0, 1.0);
	}

	double FLithologyInputs::ShatteringShareAt(FVector2d PositionMeters, EMassifFidelity Fidelity) const
	{
		// Cheap and a graph without a climate fall back to the same flat rate, so the
		// scree channel means the same thing in either.
		if (Fidelity == EMassifFidelity::Cheap || !Temperature.IsValid())
		{
			return Params.BaselineShatteringFraction;
		}
		return ShatteringFraction(State.SampleScalar(Temperature, PositionMeters), Params);
	}

	double FLithologyInputs::ScreeMetresPerMillenniumAt(
		FVector2d PositionMeters, double RockHardness, EMassifFidelity Fidelity) const
	{
		const double SoftnessFactor = 1.0 - Params.HardnessResistance * RockHardness;
		return Params.MaxScreeMetresPerMillennium * ShatteringShareAt(PositionMeters, Fidelity) * SoftnessFactor;
	}

	void WriteConstant(const FMassifEnvironmentContext& Context, const FName& ChannelName, double Value)
	{
		const TUniquePtr<FMassifChannelWriteScope> Scope = Context.OpenOutput(ChannelName);
		TArrayView<double> Values = Scope->GetComponent(0);
		for (int32 CellIndex = 0; CellIndex < Values.Num(); ++CellIndex)
		{
			Values[CellIndex] = Value;
		}
		Scope->Commit();
	}
}

/**
 * What the ground is made of, and what it sheds as it comes apart.
 *
 * The rock is a property of the place: it is laid down from the seed and from
 * world position, and it reads no channel it cannot do without. Altitude ranks
 * the ground where the graph has it, because what is still standing tends to be
 * what resisted being taken away; temperature drives the frost shattering where
 * the graph has that. Neither is required, and without either the process still
 * answers what every slope in the world is made of — which is the thing a world
 * of one uniform imaginary rock cannot answer at all.
 */
class FLithologyProcess final : public IMassifEnvironmentProcess
{
public:
	explicit FLithologyProcess(const FMassifLithologyParams& InParams)
		: Params(InParams)
	{
	}

	FMassifProcessDeclaration Declare() const override;
	void Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const override;

private:
	FMassifChannelDescriptor MakeFractionChannel(
		const FName& Name, const TCHAR* Description, EMassifChannelColorMap ColorMap) const;
	FMassifChannelDescriptor MakeScreeChannel() const;

	void WriteUniformRock(FMassifEnvironmentContext& Context) const;

	void SolveRow(
		const FLithologyInputs& Inputs, const FMassifChannelGrid& Grid, int32 Row, EMassifFidelity Fidelity,
		TArrayView<double> OutHardness, TArrayView<double> OutSoilYield, TArrayView<double> OutScree) const;

	FMassifLithologyParams Params;
};

FMassifChannelDescriptor FLithologyProcess::MakeFractionChannel(
	const FName& Name, const TCHAR* Description, EMassifChannelColorMap ColorMap) const
{
	FMassifChannelDescriptor Descriptor;
	Descriptor.Name = Name;
	Descriptor.Type = EMassifChannelType::Scalar;
	Descriptor.Encoding = EMassifChannelEncoding::Uint16;
	Descriptor.CellSizeMeters = Params.CellSizeMeters;
	Descriptor.Unit = TEXT("0-1");
	Descriptor.Description = Description;
	Descriptor.ColorMap = ColorMap;
	Descriptor.DisplayRange = FVector2d(0.0, 1.0);
	return Descriptor;
}

FMassifChannelDescriptor FLithologyProcess::MakeScreeChannel() const
{
	FMassifChannelDescriptor Descriptor;
	Descriptor.Name = MassifEnvironmentChannels::ScreeProduction();
	Descriptor.Type = EMassifChannelType::Scalar;
	Descriptor.Encoding = EMassifChannelEncoding::Uint16;
	Descriptor.CellSizeMeters = Params.CellSizeMeters;
	Descriptor.Unit = TEXT("m/kyr");
	Descriptor.Description = TEXT("Loose debris shed by frost shattering");
	Descriptor.ColorMap = EMassifChannelColorMap::Magma;
	Descriptor.DisplayRange = FVector2d(0.0, Params.MaxScreeMetresPerMillennium);
	return Descriptor;
}

FMassifProcessDeclaration FLithologyProcess::Declare() const
{
	FMassifProcessDeclaration Declaration;
	Declaration.Name = MassifEnvironmentProcesses::Lithology();
	Declaration.DisplayName = TEXT("Bedrock and Scree");

	// Both optional: rock is not made by weather, and a graph with neither a climate
	// nor a sampled terrain in it still has rock under it.
	Declaration.OptionalInputChannels = {
		MassifEnvironmentChannels::Altitude(), MassifEnvironmentChannels::MeanTemperature()};
	Declaration.SupportedFidelities = {
		EMassifFidelity::Cheap, EMassifFidelity::Standard, EMassifFidelity::Reference};

	Declaration.OutputChannels.Add(MakeFractionChannel(
		MassifEnvironmentChannels::RockHardness(), TEXT("How well the rock resists being taken apart"),
		EMassifChannelColorMap::Magma));
	Declaration.OutputChannels.Add(MakeFractionChannel(
		MassifEnvironmentChannels::SoilYield(), TEXT("How readily the rock weathers into soil"),
		EMassifChannelColorMap::Viridis));
	Declaration.OutputChannels.Add(MakeScreeChannel());
	return Declaration;
}

void FLithologyProcess::WriteUniformRock(FMassifEnvironmentContext& Context) const
{
	// Off is one rock over the whole world, neither hard nor weak and shedding
	// nothing. The channels still exist, so nothing downstream has to ask whether
	// the ground was solved before reading what it is made of.
	WriteConstant(Context, MassifEnvironmentChannels::RockHardness(), 0.5);
	WriteConstant(Context, MassifEnvironmentChannels::SoilYield(), 0.5);
	WriteConstant(Context, MassifEnvironmentChannels::ScreeProduction(), 0.0);
}

void FLithologyProcess::SolveRow(
	const FLithologyInputs& Inputs, const FMassifChannelGrid& Grid, int32 Row, EMassifFidelity Fidelity,
	TArrayView<double> OutHardness, TArrayView<double> OutSoilYield, TArrayView<double> OutScree) const
{
	for (int32 Column = 0; Column < Grid.Width; ++Column)
	{
		const FVector2d Position = Grid.CellCentreMeters(Column, Row);
		const double Hardness = Inputs.RockHardnessAt(Position, Fidelity);

		const int32 CellIndex = Row * Grid.Width + Column;
		OutHardness[CellIndex] = Hardness;
		OutSoilYield[CellIndex] = Inputs.SoilYieldAt(Position, Hardness, Fidelity);
		OutScree[CellIndex] = Inputs.ScreeMetresPerMillenniumAt(Position, Hardness, Fidelity);
	}
}

void FLithologyProcess::Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const
{
	if (Fidelity == EMassifFidelity::Off)
	{
		WriteUniformRock(Context);
		return;
	}

	const FLithologyInputs Inputs(Context, Params);

	const TUniquePtr<FMassifChannelWriteScope> Hardness =
		Context.OpenOutput(MassifEnvironmentChannels::RockHardness());
	const TUniquePtr<FMassifChannelWriteScope> SoilYield =
		Context.OpenOutput(MassifEnvironmentChannels::SoilYield());
	const TUniquePtr<FMassifChannelWriteScope> Scree =
		Context.OpenOutput(MassifEnvironmentChannels::ScreeProduction());

	const FMassifChannelGrid& Grid = Hardness->GetGrid();
	for (int32 Row = 0; Row < Grid.Height; ++Row)
	{
		SolveRow(
			Inputs, Grid, Row, Fidelity, Hardness->GetComponent(0), SoilYield->GetComponent(0),
			Scree->GetComponent(0));
	}

	Hardness->Commit();
	SoilYield->Commit();
	Scree->Commit();
}

double MassifLithologyStrikeBearingDegrees(uint32 MasterSeed)
{
	// The stream the process itself draws the bearing from: FMassifEnvironmentContext
	// names a stream by its process and its purpose.
	const FString StreamName =
		FString::Printf(TEXT("%s.Strike"), *MassifEnvironmentProcesses::Lithology().ToString());
	return StrikeBearingFromSeed(FMassifSeed(MasterSeed).MakeChannelSeed(*StreamName));
}

void MassifRegisterLithologyProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifLithologyParams& Params)
{
	Scheduler.RegisterProcess(MakeShared<FLithologyProcess>(Params));
}
