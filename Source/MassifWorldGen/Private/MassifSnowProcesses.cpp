#include "MassifSnowProcesses.h"

#include "MassifClimateProcesses.h"
#include "MassifPrecipitationProcesses.h"
#include "MassifSunProcesses.h"
#include "MassifWindProcesses.h"

namespace MassifEnvironmentChannels
{
	const FName& SnowDepth()
	{
		static const FName Name(TEXT("Snow.Depth"));
		return Name;
	}

	const FName& PeakSnowDepth()
	{
		static const FName Name(TEXT("Snow.PeakDepth"));
		return Name;
	}

	const FName& SnowCoverDuration()
	{
		static const FName Name(TEXT("Snow.CoverDuration"));
		return Name;
	}
}

namespace MassifEnvironmentProcesses
{
	const FName& Snowpack()
	{
		static const FName Name(TEXT("Snow.Pack"));
		return Name;
	}
}

namespace
{
	/** Half-monthly slices of a representative year, as the seasonal temperature holds them. */
	constexpr int32 SnowSliceCount = 24;

	constexpr double DaysPerSlice = 365.0 / SnowSliceCount;

	/** What each fidelity leaves in the model, resolved once rather than tested per cell. */
	struct FSnowFidelitySettings
	{
		bool bRedistributesByWind = false;
		bool bScalesMeltByInsolation = false;
		bool bSublimates = false;

		/** Years walked. Only the last is written; the ones before it set the pack it starts from. */
		int32 PassCount = 2;
	};

	FSnowFidelitySettings SettingsForFidelity(EMassifFidelity Fidelity)
	{
		FSnowFidelitySettings Settings;
		if (Fidelity == EMassifFidelity::Cheap)
		{
			return Settings;
		}

		Settings.bRedistributesByWind = true;
		Settings.bScalesMeltByInsolation = true;
		if (Fidelity == EMassifFidelity::Reference)
		{
			Settings.bSublimates = true;
			Settings.PassCount = 3;
		}
		return Settings;
	}

	/** World position of a cell addressed by its flat index, which is how the pack is held. */
	FVector2d CellCentreMeters(const FMassifChannelGrid& Grid, int32 CellIndex)
	{
		return Grid.CellCentreMeters(CellIndex % Grid.Width, CellIndex / Grid.Width);
	}

	/** The channels the pack is driven by, resolved once and sampled by world position. */
	class FSnowInputs
	{
	public:
		explicit FSnowInputs(const FMassifEnvironmentContext& Context)
			: State(Context.GetState())
			, AnnualPrecipitation(Context.GetInput(MassifEnvironmentChannels::AnnualPrecipitation()))
			, SeasonalTemperature(Context.GetInput(MassifEnvironmentChannels::SeasonalTemperature()))
			, DirectInsolation(Context.GetInput(MassifEnvironmentChannels::DirectInsolation()))
			, WindExposure(Context.GetInput(MassifEnvironmentChannels::WindExposure()))
		{
		}

		double AnnualMillimetresAt(FVector2d PositionMeters) const
		{
			return State.SampleScalar(AnnualPrecipitation, PositionMeters);
		}

		double SeasonalCelsiusAt(FVector2d PositionMeters, int32 TimeSlice) const
		{
			return State.SampleScalar(SeasonalTemperature, PositionMeters, TimeSlice);
		}

		double DirectInsolationAt(FVector2d PositionMeters) const
		{
			return State.SampleScalar(DirectInsolation, PositionMeters);
		}

		double WindExposureAt(FVector2d PositionMeters) const
		{
			return State.SampleScalar(WindExposure, PositionMeters);
		}

		int32 GetTemperatureSliceCount() const
		{
			return State.GetDescriptor(SeasonalTemperature).TimeSliceCount;
		}

	private:
		const FMassifEnvironmentState& State;
		FMassifChannelHandle AnnualPrecipitation;
		FMassifChannelHandle SeasonalTemperature;
		FMassifChannelHandle DirectInsolation;
		FMassifChannelHandle WindExposure;
	};

	/** Rain below the turn, snow above it, and a band in between rather than a step. */
	double FrozenShareAt(double TemperatureCelsius, const FMassifSnowParams& Params)
	{
		const double BandCelsius = FMath::Max(Params.SnowTransitionCelsius, UE_DOUBLE_KINDA_SMALL_NUMBER);
		const double AboveCelsius = TemperatureCelsius - Params.FreezingCelsius;
		return FMath::Clamp(0.5 - AboveCelsius / (2.0 * BandCelsius), 0.0, 1.0);
	}

	/**
	 * Share of a slice's snowfall a cell takes before the grid is normalised.
	 *
	 * Exposure is a multiplier on open level ground, so a crest above one is
	 * scoured and a lee pocket below one is loaded. This is only a share: what
	 * makes it transport rather than creation is the normalisation that follows.
	 */
	double RedistributionWeightAt(const FMassifSnowParams& Params, double WindExposure)
	{
		const double Weight = 1.0 + Params.WindRedistributionStrength * (1.0 - WindExposure);
		return FMath::Clamp(Weight, Params.MinRedistributionWeight, Params.MaxRedistributionWeight);
	}

	/** How much faster than the air temperature alone the sun clears this ground. */
	double MeltExposureAt(
		const FMassifSnowParams& Params, const FSnowFidelitySettings& Settings, double DirectInsolation)
	{
		if (!Settings.bScalesMeltByInsolation)
		{
			return 1.0;
		}
		return FMath::Max(0.0, 1.0 + Params.InsolationMeltGain * (DirectInsolation - 1.0));
	}

	/**
	 * The pack as it stands, walked forward through a representative year.
	 *
	 * The one sequential process in the graph: a slice's depth is the slice before
	 * it plus what fell and minus what left, so the year cannot be solved slice by
	 * slice independently the way temperature or insolation can, and the pack has
	 * to be carried in memory from one slice to the next (ARCHITECTURE §5.6.4).
	 */
	class FSnowpackSolver
	{
	public:
		FSnowpackSolver(
			const FMassifSnowParams& InParams, const FSnowFidelitySettings& InSettings,
			const FMassifChannelGrid& InGrid, const FSnowInputs& Inputs);

		/** Carries the pack from the slice before this one into this one. */
		void AdvanceSlice(int32 TimeSlice);

		TArrayView<const double> GetDepthMeters() const
		{
			return DepthMeters;
		}

	private:
		void CacheGround(const FSnowInputs& Inputs);
		void CacheTemperatures(const FSnowInputs& Inputs);

		void AccumulateSnowfall(int32 TimeSlice);
		void RedistributeSnowfall();
		double SnowfallNormalisation() const;
		double LossMeters(int32 TimeSlice, int32 CellIndex) const;

		double CelsiusAt(int32 TimeSlice, int32 CellIndex) const
		{
			return SeasonalCelsius[TimeSlice * Grid.CellCount() + CellIndex];
		}

		FMassifSnowParams Params;
		FSnowFidelitySettings Settings;
		FMassifChannelGrid Grid;

		TArray<double> AnnualMillimetres;
		TArray<double> RedistributionWeight;
		TArray<double> MeltExposure;

		/** Slice-major, so a slice's temperatures are contiguous the way they are walked. */
		TArray<double> SeasonalCelsius;

		TArray<double> SnowfallMeters;
		TArray<double> DepthMeters;
	};

	FSnowpackSolver::FSnowpackSolver(
		const FMassifSnowParams& InParams, const FSnowFidelitySettings& InSettings,
		const FMassifChannelGrid& InGrid, const FSnowInputs& Inputs)
		: Params(InParams)
		, Settings(InSettings)
		, Grid(InGrid)
	{
		AnnualMillimetres.SetNumZeroed(Grid.CellCount());
		RedistributionWeight.SetNumZeroed(Grid.CellCount());
		MeltExposure.SetNumZeroed(Grid.CellCount());
		SeasonalCelsius.SetNumZeroed(Grid.CellCount() * SnowSliceCount);
		SnowfallMeters.SetNumZeroed(Grid.CellCount());
		DepthMeters.SetNumZeroed(Grid.CellCount());

		CacheGround(Inputs);
		CacheTemperatures(Inputs);
	}

	void FSnowpackSolver::CacheGround(const FSnowInputs& Inputs)
	{
		for (int32 CellIndex = 0; CellIndex < Grid.CellCount(); ++CellIndex)
		{
			const FVector2d Position = CellCentreMeters(Grid, CellIndex);
			AnnualMillimetres[CellIndex] = Inputs.AnnualMillimetresAt(Position);
			RedistributionWeight[CellIndex] = RedistributionWeightAt(Params, Inputs.WindExposureAt(Position));
			MeltExposure[CellIndex] = MeltExposureAt(Params, Settings, Inputs.DirectInsolationAt(Position));
		}
	}

	void FSnowpackSolver::CacheTemperatures(const FSnowInputs& Inputs)
	{
		// Read once rather than once per pass: the year is walked several times and
		// the temperature it is walked against never changes.
		for (int32 TimeSlice = 0; TimeSlice < SnowSliceCount; ++TimeSlice)
		{
			for (int32 CellIndex = 0; CellIndex < Grid.CellCount(); ++CellIndex)
			{
				SeasonalCelsius[TimeSlice * Grid.CellCount() + CellIndex] =
					Inputs.SeasonalCelsiusAt(CellCentreMeters(Grid, CellIndex), TimeSlice);
			}
		}
	}

	void FSnowpackSolver::AccumulateSnowfall(int32 TimeSlice)
	{
		for (int32 CellIndex = 0; CellIndex < SnowfallMeters.Num(); ++CellIndex)
		{
			const double WaterMillimetres = AnnualMillimetres[CellIndex] / SnowSliceCount;
			const double FrozenShare = FrozenShareAt(CelsiusAt(TimeSlice, CellIndex), Params);
			SnowfallMeters[CellIndex] = WaterMillimetres * FrozenShare * Params.SnowToWaterRatio / 1000.0;
		}
	}

	/**
	 * Scale that puts the weighted snowfall back on the total that actually fell.
	 *
	 * Weighted against the fall rather than against the cell count: the two are the
	 * same only where the same amount falls everywhere, and a weighting normalised
	 * against the wrong one moves snow into shelter and quietly changes how much of
	 * it there is.
	 */
	double FSnowpackSolver::SnowfallNormalisation() const
	{
		double FallenMeters = 0.0;
		double WeightedMeters = 0.0;
		for (int32 CellIndex = 0; CellIndex < SnowfallMeters.Num(); ++CellIndex)
		{
			FallenMeters += SnowfallMeters[CellIndex];
			WeightedMeters += SnowfallMeters[CellIndex] * RedistributionWeight[CellIndex];
		}

		if (WeightedMeters <= UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			return 1.0;
		}
		return FallenMeters / WeightedMeters;
	}

	/**
	 * Moves the slice's snowfall off exposed ground into shelter without changing
	 * how much of it there is. Wind transports snow; it neither makes nor destroys
	 * it, which is what the normalisation is for.
	 */
	void FSnowpackSolver::RedistributeSnowfall()
	{
		if (!Settings.bRedistributesByWind)
		{
			return;
		}

		const double Normalisation = SnowfallNormalisation();
		for (int32 CellIndex = 0; CellIndex < SnowfallMeters.Num(); ++CellIndex)
		{
			SnowfallMeters[CellIndex] *= RedistributionWeight[CellIndex] * Normalisation;
		}
	}

	double FSnowpackSolver::LossMeters(int32 TimeSlice, int32 CellIndex) const
	{
		const double DegreeDays =
			FMath::Max(0.0, CelsiusAt(TimeSlice, CellIndex) - Params.MeltThresholdCelsius) * DaysPerSlice;
		const double MeltMeters =
			Params.DegreeDayFactorMetresPerDegreeDay * DegreeDays * MeltExposure[CellIndex];
		if (!Settings.bSublimates)
		{
			return MeltMeters;
		}
		return MeltMeters + Params.SublimationMetresPerDay * DaysPerSlice;
	}

	void FSnowpackSolver::AdvanceSlice(int32 TimeSlice)
	{
		AccumulateSnowfall(TimeSlice);
		RedistributeSnowfall();

		// Clamped rather than subtracted: melt and sublimation can only take what is
		// lying, and a depth allowed to go negative would carry the shortfall into the
		// next slice and eat snow that has not fallen yet.
		for (int32 CellIndex = 0; CellIndex < DepthMeters.Num(); ++CellIndex)
		{
			const double LyingMeters = DepthMeters[CellIndex] + SnowfallMeters[CellIndex];
			DepthMeters[CellIndex] = FMath::Max(0.0, LyingMeters - LossMeters(TimeSlice, CellIndex));
		}
	}

	/** A year run only to leave the pack where the next one starts from. */
	void RunSpinUpYear(FSnowpackSolver& Solver)
	{
		for (int32 TimeSlice = 0; TimeSlice < SnowSliceCount; ++TimeSlice)
		{
			Solver.AdvanceSlice(TimeSlice);
		}
	}

	FMassifChannelDescriptor MakeSnowChannel(
		const FName& Name, const TCHAR* Unit, const TCHAR* Description, double CellSizeMeters,
		int32 TimeSliceCount)
	{
		FMassifChannelDescriptor Descriptor;
		Descriptor.Name = Name;
		Descriptor.Type = EMassifChannelType::Scalar;
		Descriptor.Encoding = EMassifChannelEncoding::Uint16;
		Descriptor.CellSizeMeters = CellSizeMeters;
		Descriptor.TimeSliceCount = TimeSliceCount;
		Descriptor.Unit = Unit;
		Descriptor.Description = Description;
		Descriptor.ColorMap = EMassifChannelColorMap::BlueWhite;
		return Descriptor;
	}

	void WriteSliceValues(
		const FMassifEnvironmentContext& Context, const FName& ChannelName, int32 TimeSlice,
		TArrayView<const double> Values)
	{
		const TUniquePtr<FMassifChannelWriteScope> Scope = Context.OpenOutput(ChannelName, TimeSlice);
		TArrayView<double> Target = Scope->GetComponent(0);
		for (int32 CellIndex = 0; CellIndex < Target.Num(); ++CellIndex)
		{
			Target[CellIndex] = Values[CellIndex];
		}
		Scope->Commit();
	}

	/** The scope's scratch starts zeroed, so an empty slice is a commit and nothing else. */
	void WriteZeroedSlice(const FMassifEnvironmentContext& Context, const FName& ChannelName, int32 TimeSlice)
	{
		const TUniquePtr<FMassifChannelWriteScope> Scope = Context.OpenOutput(ChannelName, TimeSlice);
		Scope->Commit();
	}

	/** Both reductions, gathered as the year is written rather than read back out of it afterwards. */
	void AccumulateReductions(
		TArrayView<const double> DepthMeters, double LyingThresholdMetres, TArrayView<double> PeakDepthMeters,
		TArrayView<double> LyingSliceCount)
	{
		for (int32 CellIndex = 0; CellIndex < DepthMeters.Num(); ++CellIndex)
		{
			PeakDepthMeters[CellIndex] = FMath::Max(PeakDepthMeters[CellIndex], DepthMeters[CellIndex]);
			if (DepthMeters[CellIndex] > LyingThresholdMetres)
			{
				LyingSliceCount[CellIndex] += 1.0;
			}
		}
	}

	TArray<double> CoverDurationFrom(TArrayView<const double> LyingSliceCount)
	{
		TArray<double> CoverDuration;
		CoverDuration.Reserve(LyingSliceCount.Num());
		for (double SliceCount : LyingSliceCount)
		{
			CoverDuration.Add(SliceCount / SnowSliceCount);
		}
		return CoverDuration;
	}
}

/**
 * Snow lying on the ground through a representative year.
 *
 * Snow is the one thing in the environment that is not a steady state: what lies
 * in April is what fell since October minus what the sun and the air took back,
 * and no single number for the year can say it. So the year is walked in order —
 * snowfall, wind transport, melt, sublimation — and reduced afterwards to the two
 * numbers everything downstream wants: how deep it ever gets, and how much of the
 * year the ground is white.
 */
class FSnowpackProcess final : public IMassifEnvironmentProcess
{
public:
	explicit FSnowpackProcess(const FMassifSnowParams& InParams)
		: Params(InParams)
	{
	}

	FMassifProcessDeclaration Declare() const override;
	void Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const override;

private:
	void WriteBareGround(FMassifEnvironmentContext& Context) const;
	void WriteSolvedYear(FMassifEnvironmentContext& Context, FSnowpackSolver& Solver) const;

	FMassifSnowParams Params;
};

FMassifProcessDeclaration FSnowpackProcess::Declare() const
{
	FMassifProcessDeclaration Declaration;
	Declaration.Name = MassifEnvironmentProcesses::Snowpack();
	Declaration.InputChannels = {
		MassifEnvironmentChannels::AnnualPrecipitation(), MassifEnvironmentChannels::SeasonalTemperature(),
		MassifEnvironmentChannels::DirectInsolation(), MassifEnvironmentChannels::WindExposure()};
	Declaration.SupportedFidelities = {
		EMassifFidelity::Cheap, EMassifFidelity::Standard, EMassifFidelity::Reference};

	// Vegetation reads how long this snow lies, and the canopy it grows slows the
	// wind that moved the snow in the first place: the pack sits inside that loop.
	Declaration.bParticipatesInFeedback = true;

	Declaration.OutputChannels.Add(MakeSnowChannel(
		MassifEnvironmentChannels::SnowDepth(), TEXT("m"), TEXT("Snow lying on the ground"),
		Params.CellSizeMeters, SnowSliceCount));
	Declaration.OutputChannels.Add(MakeSnowChannel(
		MassifEnvironmentChannels::PeakSnowDepth(), TEXT("m"), TEXT("Deepest the pack gets over the year"),
		Params.CellSizeMeters, 1));

	FMassifChannelDescriptor CoverDuration = MakeSnowChannel(
		MassifEnvironmentChannels::SnowCoverDuration(), TEXT("0-1"), TEXT("Share of the year with snow lying"),
		Params.CellSizeMeters, 1);
	CoverDuration.DisplayRange = FVector2d(0.0, 1.0);
	Declaration.OutputChannels.Add(CoverDuration);
	return Declaration;
}

void FSnowpackProcess::WriteBareGround(FMassifEnvironmentContext& Context) const
{
	// Off is a world nothing ever lies on. The channels still exist, so nothing
	// downstream has to ask whether the pack was solved.
	for (int32 TimeSlice = 0; TimeSlice < SnowSliceCount; ++TimeSlice)
	{
		WriteZeroedSlice(Context, MassifEnvironmentChannels::SnowDepth(), TimeSlice);
	}

	WriteZeroedSlice(Context, MassifEnvironmentChannels::PeakSnowDepth(), 0);
	WriteZeroedSlice(Context, MassifEnvironmentChannels::SnowCoverDuration(), 0);
}

void FSnowpackProcess::WriteSolvedYear(FMassifEnvironmentContext& Context, FSnowpackSolver& Solver) const
{
	TArray<double> PeakDepthMeters;
	TArray<double> LyingSliceCount;
	PeakDepthMeters.SetNumZeroed(Solver.GetDepthMeters().Num());
	LyingSliceCount.SetNumZeroed(Solver.GetDepthMeters().Num());

	for (int32 TimeSlice = 0; TimeSlice < SnowSliceCount; ++TimeSlice)
	{
		Solver.AdvanceSlice(TimeSlice);
		WriteSliceValues(Context, MassifEnvironmentChannels::SnowDepth(), TimeSlice, Solver.GetDepthMeters());
		AccumulateReductions(
			Solver.GetDepthMeters(), Params.LyingSnowThresholdMetres, PeakDepthMeters, LyingSliceCount);
	}

	WriteSliceValues(Context, MassifEnvironmentChannels::PeakSnowDepth(), 0, PeakDepthMeters);
	WriteSliceValues(
		Context, MassifEnvironmentChannels::SnowCoverDuration(), 0, CoverDurationFrom(LyingSliceCount));
}

void FSnowpackProcess::Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const
{
	if (Fidelity == EMassifFidelity::Off)
	{
		WriteBareGround(Context);
		return;
	}

	const FSnowInputs Inputs(Context);
	checkf(Inputs.GetTemperatureSliceCount() == SnowSliceCount,
		TEXT("The pack walks %d slices of the year but the seasonal temperature holds %d"),
		SnowSliceCount, Inputs.GetTemperatureSliceCount());

	const FSnowFidelitySettings Settings = SettingsForFidelity(Fidelity);
	const FMassifChannelGrid& Grid =
		Context.GetState().GetGrid(Context.GetOutput(MassifEnvironmentChannels::SnowDepth()));
	FSnowpackSolver Solver(Params, Settings, Grid, Inputs);

	// The year is walked a fixed number of times and only the last one kept, so the
	// pack starts the year with whatever it ended the one before it with instead of
	// from bare ground — which is the difference between a cirque that holds its
	// snow through the summer and one that is reset every January. A count, never a
	// test against how far apart two passes still are (ARCHITECTURE §4).
	for (int32 Pass = 1; Pass < Settings.PassCount; ++Pass)
	{
		RunSpinUpYear(Solver);
	}

	WriteSolvedYear(Context, Solver);
}

void MassifRegisterSnowProcesses(FMassifEnvironmentScheduler& Scheduler, const FMassifSnowParams& Params)
{
	Scheduler.RegisterProcess(MakeShared<FSnowpackProcess>(Params));
}
