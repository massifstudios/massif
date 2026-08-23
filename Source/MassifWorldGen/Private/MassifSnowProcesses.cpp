#include "MassifSnowProcesses.h"

#include "MassifClimateProcesses.h"
#include "MassifEnvironmentGraph.h"
#include "MassifPrecipitationProcesses.h"
#include "MassifSunProcesses.h"
#include "MassifVegetationProcesses.h"
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

	const FName& SnowDensity()
	{
		static const FName Name(TEXT("Snow.Density"));
		return Name;
	}

	const FName& SnowAlbedo()
	{
		static const FName Name(TEXT("Snow.Albedo"));
		return Name;
	}

	const FName& AvalancheRelease()
	{
		static const FName Name(TEXT("Snow.AvalancheRelease"));
		return Name;
	}

	const FName& AvalanchePath()
	{
		static const FName Name(TEXT("Snow.AvalanchePath"));
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
		bool bCompactsFasterWhenWarm = false;
		bool bTracksAlbedo = false;
		bool bSolvesAvalanches = false;
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
		Settings.bCompactsFasterWhenWarm = true;
		Settings.bTracksAlbedo = true;
		Settings.bSolvesAvalanches = true;
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
			, Altitude(Context.GetInput(MassifEnvironmentChannels::Altitude()))
			, AnnualPrecipitation(Context.GetInput(MassifEnvironmentChannels::AnnualPrecipitation()))
			, SeasonalPrecipitation(
				Context.FindOptionalInput(MassifEnvironmentChannels::SeasonalPrecipitation()))
			, FallenSnowFraction(Context.FindOptionalInput(MassifEnvironmentChannels::SnowFraction()))
			, SeasonalTemperature(Context.GetInput(MassifEnvironmentChannels::SeasonalTemperature()))
			, DirectInsolation(Context.GetInput(MassifEnvironmentChannels::DirectInsolation()))
			, WindExposure(Context.GetInput(MassifEnvironmentChannels::WindExposure()))
			, VegetationCanopy(Context.FindOptionalInput(MassifEnvironmentChannels::VegetationCanopy()))
		{
		}

		double AltitudeAt(FVector2d PositionMeters) const
		{
			return State.SampleScalar(Altitude, PositionMeters);
		}

		double AnnualMillimetresAt(FVector2d PositionMeters) const
		{
			return State.SampleScalar(AnnualPrecipitation, PositionMeters);
		}

		/**
		 * What fell in one half-month. Falls back to an even share of the year where
		 * no seasonal channel is registered, which is the same thing the pack assumed
		 * before one existed.
		 */
		double SeasonalMillimetresAt(FVector2d PositionMeters, int32 TimeSlice, int32 SliceCount) const
		{
			if (!SeasonalPrecipitation.IsValid())
			{
				return State.SampleScalar(AnnualPrecipitation, PositionMeters) / SliceCount;
			}
			return State.SampleScalar(SeasonalPrecipitation, PositionMeters, TimeSlice);
		}

		/** The share of that half-month's fall that arrived frozen, where anything says so. */
		bool TryFallenSnowFractionAt(FVector2d PositionMeters, int32 TimeSlice, double& OutFraction) const
		{
			if (!FallenSnowFraction.IsValid())
			{
				return false;
			}
			OutFraction = State.SampleScalar(FallenSnowFraction, PositionMeters, TimeSlice);
			return true;
		}

		double SeasonalCelsiusAt(FVector2d PositionMeters, int32 TimeSlice) const
		{
			return State.SampleScalar(SeasonalTemperature, PositionMeters, TimeSlice);
		}

		double DirectInsolationAt(FVector2d PositionMeters, int32 TimeSlice) const
		{
			return State.SampleScalar(DirectInsolation, PositionMeters, TimeSlice);
		}

		double WindExposureAt(FVector2d PositionMeters) const
		{
			return State.SampleScalar(WindExposure, PositionMeters);
		}

		/** Nothing anchors the snow in a graph with no vegetation in it. */
		double CanopyAt(FVector2d PositionMeters) const
		{
			if (!VegetationCanopy.IsValid())
			{
				return 0.0;
			}
			return State.SampleScalar(VegetationCanopy, PositionMeters);
		}

		int32 GetTemperatureSliceCount() const
		{
			return State.GetDescriptor(SeasonalTemperature).TimeSliceCount;
		}

		int32 GetInsolationSliceCount() const
		{
			return State.GetDescriptor(DirectInsolation).TimeSliceCount;
		}

	private:
		const FMassifEnvironmentState& State;
		FMassifChannelHandle Altitude;
		FMassifChannelHandle AnnualPrecipitation;
		FMassifChannelHandle SeasonalPrecipitation;
		FMassifChannelHandle FallenSnowFraction;
		FMassifChannelHandle SeasonalTemperature;
		FMassifChannelHandle DirectInsolation;
		FMassifChannelHandle WindExposure;
		FMassifChannelHandle VegetationCanopy;
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

	/**
	 * How much faster than the air temperature alone the sun clears this ground in
	 * this half-month.
	 *
	 * The insolation is measured against level open ground on an average growing-
	 * season day, so this carries the season as well as the aspect: a midwinter
	 * slice sits well below one and a midsummer slice above it. Floored at zero,
	 * because sunlight that is merely weak still cannot make snow.
	 */
	double MeltExposureAt(
		const FMassifSnowParams& Params, const FSnowFidelitySettings& Settings, double DirectInsolation)
	{
		if (!Settings.bScalesMeltByInsolation)
		{
			return 1.0;
		}
		return FMath::Max(0.0, 1.0 + Params.InsolationMeltGain * (DirectInsolation - 1.0));
	}

	/** Density of a pack that has just taken a fresh fall on top of what was already lying. */
	double DensityAfterSnowfall(
		const FMassifSnowParams& Params, double LyingMeters, double LyingDensity, double SnowfallMeters)
	{
		const double TotalMeters = LyingMeters + SnowfallMeters;
		if (TotalMeters <= UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			return Params.FreshSnowDensityKilogramsPerCubicMetre;
		}

		const double LyingMass = LyingMeters * LyingDensity;
		const double FallenMass = SnowfallMeters * Params.FreshSnowDensityKilogramsPerCubicMetre;
		return (LyingMass + FallenMass) / TotalMeters;
	}

	/**
	 * How much faster than a cold pack one near its melting point settles.
	 *
	 * Melt-freeze rounds the grains off and packs them within days, where snow well
	 * below freezing keeps the structure it fell with for months. This is why an
	 * April pack carries a machine and a January one does not.
	 */
	double CompactionWarmthFactor(
		const FMassifSnowParams& Params, const FSnowFidelitySettings& Settings, double TemperatureCelsius)
	{
		if (!Settings.bCompactsFasterWhenWarm)
		{
			return 1.0;
		}

		const double BandCelsius = FMath::Max(Params.WarmCompactionBandCelsius, UE_DOUBLE_KINDA_SMALL_NUMBER);
		const double BelowThreshold = Params.MeltThresholdCelsius - TemperatureCelsius;
		const double Warmth = FMath::Clamp(1.0 - BelowThreshold / BandCelsius, 0.0, 1.0);
		return FMath::Lerp(1.0, Params.WarmCompactionMultiplier, Warmth);
	}

	/** Where a cell's density stands after the slice: fresh again once nothing lies, settled further otherwise. */
	double SettledDensity(
		const FMassifSnowParams& Params, const FSnowFidelitySettings& Settings, double MixedDensity,
		double RemainingMeters, double TemperatureCelsius)
	{
		if (RemainingMeters <= UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			return Params.FreshSnowDensityKilogramsPerCubicMetre;
		}

		const double Rate = FMath::Clamp(
			Params.CompactionRatePerSlice * CompactionWarmthFactor(Params, Settings, TemperatureCelsius), 0.0, 1.0);
		const double Relaxed =
			MixedDensity + (Params.SettledSnowDensityKilogramsPerCubicMetre - MixedDensity) * Rate;
		return FMath::Clamp(Relaxed, Params.FreshSnowDensityKilogramsPerCubicMetre,
			Params.SettledSnowDensityKilogramsPerCubicMetre);
	}

	/** How far the pack has settled from the density it fell at towards the one it ends at. */
	double SettlednessOf(const FMassifSnowParams& Params, double Density)
	{
		const double SpanKilogramsPerCubicMetre = FMath::Max(
			Params.SettledSnowDensityKilogramsPerCubicMetre - Params.FreshSnowDensityKilogramsPerCubicMetre,
			UE_DOUBLE_KINDA_SMALL_NUMBER);
		const double Settled =
			(Density - Params.FreshSnowDensityKilogramsPerCubicMetre) / SpanKilogramsPerCubicMetre;
		return FMath::Clamp(Settled, 0.0, 1.0);
	}

	/**
	 * What the ground throws back: bare where nothing lies, near white under a fresh
	 * fall, and greyer the further the pack has settled.
	 *
	 * Keyed off the lying threshold rather than off any depth at all, so a
	 * centimetre of old snow behind a rock does not turn a hillside white.
	 */
	double AlbedoAt(const FMassifSnowParams& Params, double DepthMeters, double Density)
	{
		const double SnowAlbedo =
			FMath::Lerp(Params.FreshSnowAlbedo, Params.OldSnowAlbedo, SettlednessOf(Params, Density));

		const double ThresholdMetres = FMath::Max(Params.LyingSnowThresholdMetres, UE_DOUBLE_KINDA_SMALL_NUMBER);
		const double Cover = FMath::Clamp(DepthMeters / ThresholdMetres, 0.0, 1.0);
		return FMath::Lerp(Params.BareGroundAlbedo, SnowAlbedo, Cover);
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

		TArrayView<const double> GetDensityKilogramsPerCubicMetre() const
		{
			return DensityKilogramsPerCubicMetre;
		}

		TArrayView<const double> GetAlbedo() const
		{
			return Albedo;
		}

	private:
		void CacheGround(const FSnowInputs& Inputs);
		void CacheTemperatures(const FSnowInputs& Inputs);
		void CacheMeltExposure(const FSnowInputs& Inputs);
		void CacheSnowfall(const FSnowInputs& Inputs);

		void AccumulateSnowfall(int32 TimeSlice);
		void RedistributeSnowfall();
		double SnowfallNormalisation() const;
		void AdvanceCell(int32 TimeSlice, int32 CellIndex);
		double LossMeters(int32 TimeSlice, int32 CellIndex) const;

		double CelsiusAt(int32 TimeSlice, int32 CellIndex) const
		{
			return SeasonalCelsius[TimeSlice * Grid.CellCount() + CellIndex];
		}

		double MeltExposureAtCell(int32 TimeSlice, int32 CellIndex) const
		{
			return MeltExposure[TimeSlice * Grid.CellCount() + CellIndex];
		}

		FMassifSnowParams Params;
		FSnowFidelitySettings Settings;
		FMassifChannelGrid Grid;

		TArray<double> AnnualMillimetres;

		/** Metres of snow arriving in each half-month, slice-major. */
		TArray<double> FallenSnowMeters;
		TArray<double> RedistributionWeight;

		/** Both slice-major, so a slice's drivers are contiguous the way they are walked. */
		TArray<double> SeasonalCelsius;
		TArray<double> MeltExposure;

		TArray<double> SnowfallMeters;
		TArray<double> DepthMeters;
		TArray<double> DensityKilogramsPerCubicMetre;
		TArray<double> Albedo;
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
		SeasonalCelsius.SetNumZeroed(Grid.CellCount() * SnowSliceCount);
		MeltExposure.SetNumZeroed(Grid.CellCount() * SnowSliceCount);
		SnowfallMeters.SetNumZeroed(Grid.CellCount());
		DepthMeters.SetNumZeroed(Grid.CellCount());
		DensityKilogramsPerCubicMetre.Init(Params.FreshSnowDensityKilogramsPerCubicMetre, Grid.CellCount());
		Albedo.Init(Params.BareGroundAlbedo, Grid.CellCount());

		CacheGround(Inputs);
		CacheTemperatures(Inputs);
		CacheMeltExposure(Inputs);
		CacheSnowfall(Inputs);
	}

	void FSnowpackSolver::CacheGround(const FSnowInputs& Inputs)
	{
		for (int32 CellIndex = 0; CellIndex < Grid.CellCount(); ++CellIndex)
		{
			const FVector2d Position = CellCentreMeters(Grid, CellIndex);
			AnnualMillimetres[CellIndex] = Inputs.AnnualMillimetresAt(Position);
			RedistributionWeight[CellIndex] = RedistributionWeightAt(Params, Inputs.WindExposureAt(Position));
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

	void FSnowpackSolver::CacheMeltExposure(const FSnowInputs& Inputs)
	{
		// Per slice, not once for the year: the sun a half-month melts under is that
		// half-month's own, and a single reading would run the whole year under
		// whichever one it happened to take.
		for (int32 TimeSlice = 0; TimeSlice < SnowSliceCount; ++TimeSlice)
		{
			for (int32 CellIndex = 0; CellIndex < Grid.CellCount(); ++CellIndex)
			{
				const double Insolation =
					Inputs.DirectInsolationAt(CellCentreMeters(Grid, CellIndex), TimeSlice);
				MeltExposure[TimeSlice * Grid.CellCount() + CellIndex] =
					MeltExposureAt(Params, Settings, Insolation);
			}
		}
	}

	/**
	 * What arrives as snow in each half-month.
	 *
	 * Read once for the year rather than once per pass, like the temperatures: the
	 * year is walked several times and what fell on it does not change between
	 * walks. Where the precipitation solve says when the water came and how much of
	 * it came frozen, that is used; where it does not, the year is spread evenly and
	 * the phase split falls back to this pack's own freezing band.
	 */
	void FSnowpackSolver::CacheSnowfall(const FSnowInputs& Inputs)
	{
		FallenSnowMeters.SetNumZeroed(SnowSliceCount * Grid.CellCount());
		for (int32 TimeSlice = 0; TimeSlice < SnowSliceCount; ++TimeSlice)
		{
			for (int32 CellIndex = 0; CellIndex < Grid.CellCount(); ++CellIndex)
			{
				const FVector2d Position = CellCentreMeters(Grid, CellIndex);
				const double WaterMillimetres =
					Inputs.SeasonalMillimetresAt(Position, TimeSlice, SnowSliceCount);

				double FrozenShare = FrozenShareAt(CelsiusAt(TimeSlice, CellIndex), Params);
				Inputs.TryFallenSnowFractionAt(Position, TimeSlice, FrozenShare);

				FallenSnowMeters[TimeSlice * Grid.CellCount() + CellIndex] =
					WaterMillimetres * FrozenShare * Params.SnowToWaterRatio / 1000.0;
			}
		}
	}

	void FSnowpackSolver::AccumulateSnowfall(int32 TimeSlice)
	{
		for (int32 CellIndex = 0; CellIndex < SnowfallMeters.Num(); ++CellIndex)
		{
			SnowfallMeters[CellIndex] = FallenSnowMeters[TimeSlice * Grid.CellCount() + CellIndex];
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
			Params.DegreeDayFactorMetresPerDegreeDay * DegreeDays * MeltExposureAtCell(TimeSlice, CellIndex);
		if (!Settings.bSublimates)
		{
			return MeltMeters;
		}
		return MeltMeters + Params.SublimationMetresPerDay * DaysPerSlice;
	}

	/**
	 * One cell through one half-month: what fell mixed into what was lying, what the
	 * sun and the air took back, and what is left settling under its own weight.
	 *
	 * The depth is clamped rather than subtracted: melt and sublimation can only
	 * take what is lying, and a depth allowed to go negative would carry the
	 * shortfall into the next slice and eat snow that has not fallen yet.
	 */
	void FSnowpackSolver::AdvanceCell(int32 TimeSlice, int32 CellIndex)
	{
		const double MixedDensity = DensityAfterSnowfall(
			Params, DepthMeters[CellIndex], DensityKilogramsPerCubicMetre[CellIndex], SnowfallMeters[CellIndex]);

		const double LyingMeters = DepthMeters[CellIndex] + SnowfallMeters[CellIndex];
		const double RemainingMeters = FMath::Max(0.0, LyingMeters - LossMeters(TimeSlice, CellIndex));

		DepthMeters[CellIndex] = RemainingMeters;
		DensityKilogramsPerCubicMetre[CellIndex] = SettledDensity(
			Params, Settings, MixedDensity, RemainingMeters, CelsiusAt(TimeSlice, CellIndex));
		Albedo[CellIndex] = AlbedoAt(Params, RemainingMeters, DensityKilogramsPerCubicMetre[CellIndex]);
	}

	void FSnowpackSolver::AdvanceSlice(int32 TimeSlice)
	{
		AccumulateSnowfall(TimeSlice);
		RedistributeSnowfall();

		for (int32 CellIndex = 0; CellIndex < DepthMeters.Num(); ++CellIndex)
		{
			AdvanceCell(TimeSlice, CellIndex);
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

	/** The ground the avalanche pass runs over, sampled once onto the grid the pack is held on. */
	struct FAvalancheGround
	{
		TArray<double> AltitudeMeters;
		TArray<double> Canopy;
	};

	FAvalancheGround CacheAvalancheGround(const FSnowInputs& Inputs, const FMassifChannelGrid& Grid)
	{
		FAvalancheGround Ground;
		Ground.AltitudeMeters.SetNumZeroed(Grid.CellCount());
		Ground.Canopy.SetNumZeroed(Grid.CellCount());
		for (int32 CellIndex = 0; CellIndex < Grid.CellCount(); ++CellIndex)
		{
			const FVector2d Position = CellCentreMeters(Grid, CellIndex);
			Ground.AltitudeMeters[CellIndex] = Inputs.AltitudeAt(Position);
			Ground.Canopy[CellIndex] = Inputs.CanopyAt(Position);
		}
		return Ground;
	}

	/** The eight neighbours, in a fixed order so a tie always falls the same way. */
	constexpr int32 NeighbourColumnOffsets[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
	constexpr int32 NeighbourRowOffsets[8] = {-1, -1, -1, 0, 0, 1, 1, 1};

	double AltitudeAtOffset(
		const FMassifChannelGrid& Grid, TArrayView<const double> AltitudeMeters, int32 Column, int32 Row)
	{
		const int32 ClampedColumn = FMath::Clamp(Column, 0, Grid.Width - 1);
		const int32 ClampedRow = FMath::Clamp(Row, 0, Grid.Height - 1);
		return AltitudeMeters[ClampedRow * Grid.Width + ClampedColumn];
	}

	/** Ground steepness in degrees, differenced across one cell of the grid the pack is solved on. */
	double SlopeDegreesAt(
		const FMassifChannelGrid& Grid, TArrayView<const double> AltitudeMeters, int32 CellIndex)
	{
		const int32 Column = CellIndex % Grid.Width;
		const int32 Row = CellIndex / Grid.Width;

		const double East = AltitudeAtOffset(Grid, AltitudeMeters, Column + 1, Row);
		const double West = AltitudeAtOffset(Grid, AltitudeMeters, Column - 1, Row);
		const double North = AltitudeAtOffset(Grid, AltitudeMeters, Column, Row - 1);
		const double South = AltitudeAtOffset(Grid, AltitudeMeters, Column, Row + 1);

		const double SpanMeters = FMath::Max(2.0 * Grid.CellSizeMeters, UE_DOUBLE_KINDA_SMALL_NUMBER);
		const FVector2d Gradient((East - West) / SpanMeters, (South - North) / SpanMeters);
		return FMath::RadiansToDegrees(FMath::Atan(Gradient.Length()));
	}

	/**
	 * How far into the release band a slope sits.
	 *
	 * Zero below the angle snow simply sits at and zero above the angle it sloughs
	 * off at as it arrives, one in the middle of the band where a slab both builds
	 * and lets go.
	 */
	double ReleaseSlopeWeight(const FMassifSnowParams& Params, double SlopeDegrees)
	{
		const double SpanDegrees = FMath::Max(
			Params.MaxReleaseSlopeDegrees - Params.MinReleaseSlopeDegrees, UE_DOUBLE_KINDA_SMALL_NUMBER);
		const double BandFraction = (SlopeDegrees - Params.MinReleaseSlopeDegrees) / SpanDegrees;
		if (BandFraction <= 0.0 || BandFraction >= 1.0)
		{
			return 0.0;
		}
		return FMath::Sin(UE_DOUBLE_PI * BandFraction);
	}

	/**
	 * How readily one cell lets a slab go: steep enough to slide, loaded enough to
	 * matter, and open enough that nothing is holding the snow down.
	 *
	 * A closed stand anchors a slab outright — trunks pin the layers to the ground —
	 * which is why an avalanche track is a stripe through a forest rather than over
	 * it.
	 */
	double ReleaseAt(
		const FMassifSnowParams& Params, double SlopeDegrees, double PeakDepthMeters, double Canopy)
	{
		const double LoadDepthMetres =
			FMath::Max(Params.ReleaseFullLoadDepthMetres, UE_DOUBLE_KINDA_SMALL_NUMBER);
		const double Load = FMath::Clamp(PeakDepthMeters / LoadDepthMetres, 0.0, 1.0);
		const double Anchoring = FMath::Clamp(1.0 - Canopy, 0.0, 1.0);
		return ReleaseSlopeWeight(Params, SlopeDegrees) * Load * Anchoring;
	}

	TArray<double> SolveAvalancheRelease(
		const FMassifSnowParams& Params, const FMassifChannelGrid& Grid, const FAvalancheGround& Ground,
		TArrayView<const double> PeakDepthMeters)
	{
		TArray<double> Release;
		Release.SetNumZeroed(Grid.CellCount());
		for (int32 CellIndex = 0; CellIndex < Release.Num(); ++CellIndex)
		{
			const double SlopeDegrees = SlopeDegreesAt(Grid, Ground.AltitudeMeters, CellIndex);
			Release[CellIndex] =
				ReleaseAt(Params, SlopeDegrees, PeakDepthMeters[CellIndex], Ground.Canopy[CellIndex]);
		}
		return Release;
	}

	/** Where a slab standing on a cell goes next, and how steeply it gets there. */
	struct FDescentStep
	{
		int32 CellIndex = INDEX_NONE;
		double SlopeDegrees = 0.0;
	};

	/** Drop per metre travelled towards one neighbour, and zero where that neighbour is not lower. */
	double DescentGradientTowards(
		const FMassifChannelGrid& Grid, TArrayView<const double> AltitudeMeters, int32 Column, int32 Row,
		int32 NeighbourIndex)
	{
		const int32 NeighbourColumn = Column + NeighbourColumnOffsets[NeighbourIndex];
		const int32 NeighbourRow = Row + NeighbourRowOffsets[NeighbourIndex];
		const int32 NeighbourCell = Grid.CellIndex(NeighbourColumn, NeighbourRow);
		if (NeighbourCell == INDEX_NONE)
		{
			return 0.0;
		}

		const double DropMeters = AltitudeMeters[Row * Grid.Width + Column] - AltitudeMeters[NeighbourCell];
		const double StepCells = FMath::Sqrt(double(
			FMath::Square(NeighbourColumnOffsets[NeighbourIndex])
			+ FMath::Square(NeighbourRowOffsets[NeighbourIndex])));
		return FMath::Max(0.0, DropMeters / (Grid.CellSizeMeters * StepCells));
	}

	FDescentStep SteepestDescentFrom(
		const FMassifChannelGrid& Grid, TArrayView<const double> AltitudeMeters, int32 CellIndex)
	{
		const int32 Column = CellIndex % Grid.Width;
		const int32 Row = CellIndex / Grid.Width;

		FDescentStep Steepest;
		double SteepestGradient = 0.0;
		for (int32 NeighbourIndex = 0; NeighbourIndex < 8; ++NeighbourIndex)
		{
			const double Gradient = DescentGradientTowards(Grid, AltitudeMeters, Column, Row, NeighbourIndex);
			if (Gradient > SteepestGradient)
			{
				SteepestGradient = Gradient;
				Steepest.CellIndex = Grid.CellIndex(
					Column + NeighbourColumnOffsets[NeighbourIndex], Row + NeighbourRowOffsets[NeighbourIndex]);
			}
		}

		Steepest.SlopeDegrees = FMath::RadiansToDegrees(FMath::Atan(SteepestGradient));
		return Steepest;
	}

	/**
	 * Walks what one release area let go down the hill, marking every cell it crosses.
	 *
	 * A fixed step budget rather than a walk that runs until it stops: a hollow the
	 * descent cannot leave would otherwise trap the trace, and a bounded walk is the
	 * same walk on every machine (ARCHITECTURE §4).
	 */
	void TraceOnePath(
		const FMassifSnowParams& Params, const FMassifChannelGrid& Grid,
		TArrayView<const double> AltitudeMeters, int32 ReleaseCell, double ReleaseAmount,
		TArrayView<double> OutTrack)
	{
		if (ReleaseAmount <= UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			return;
		}

		int32 CellIndex = ReleaseCell;
		for (int32 Step = 0; Step < Params.MaxRunoutStepCount; ++Step)
		{
			const FDescentStep Next = SteepestDescentFrom(Grid, AltitudeMeters, CellIndex);
			if (Next.CellIndex == INDEX_NONE || Next.SlopeDegrees < Params.RunoutSlopeDegrees)
			{
				return;
			}
			OutTrack[Next.CellIndex] += ReleaseAmount;
			CellIndex = Next.CellIndex;
		}
	}

	/**
	 * What every release area above a cell puts across it.
	 *
	 * The same shape as flow accumulation, but gathered only from ground that can
	 * release and stopped where the hill runs out: this is the track, not the
	 * catchment.
	 */
	TArray<double> TraceAvalanchePaths(
		const FMassifSnowParams& Params, const FMassifChannelGrid& Grid,
		TArrayView<const double> AltitudeMeters, TArrayView<const double> Release)
	{
		TArray<double> Track;
		Track.SetNumZeroed(Release.Num());
		for (int32 CellIndex = 0; CellIndex < Release.Num(); ++CellIndex)
		{
			TraceOnePath(Params, Grid, AltitudeMeters, CellIndex, Release[CellIndex], Track);
		}

		const double SaturationLoad = FMath::Max(Params.PathSaturationLoad, UE_DOUBLE_KINDA_SMALL_NUMBER);
		for (double& Marked : Track)
		{
			Marked = FMath::Clamp(Marked / SaturationLoad, 0.0, 1.0);
		}
		return Track;
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

	/** The same, for the channels that are shares of one rather than quantities. */
	FMassifChannelDescriptor MakeSnowFractionChannel(
		const FName& Name, const TCHAR* Description, double CellSizeMeters, int32 TimeSliceCount)
	{
		FMassifChannelDescriptor Descriptor =
			MakeSnowChannel(Name, TEXT("0-1"), Description, CellSizeMeters, TimeSliceCount);
		Descriptor.DisplayRange = FVector2d(0.0, 1.0);
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
 * snowfall, wind transport, melt, sublimation, settling — and reduced afterwards
 * to what everything downstream wants: how deep it ever gets, how much of the
 * year the ground is white, how hard the pack is, how much light it throws back,
 * and which ground it comes off in slabs.
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

	void WriteSliceOfYear(
		FMassifEnvironmentContext& Context, const FSnowpackSolver& Solver, const FSnowFidelitySettings& Settings,
		int32 TimeSlice) const;

	/** Walks the year that is kept, writes every slice of it, and hands back the peak depth. */
	TArray<double> WriteSolvedYear(
		FMassifEnvironmentContext& Context, FSnowpackSolver& Solver, const FSnowFidelitySettings& Settings) const;

	void WriteAvalanches(
		FMassifEnvironmentContext& Context, const FSnowInputs& Inputs, const FMassifChannelGrid& Grid,
		TArrayView<const double> PeakDepthMeters) const;

	FMassifSnowParams Params;
};

FMassifProcessDeclaration FSnowpackProcess::Declare() const
{
	FMassifProcessDeclaration Declaration;
	Declaration.Name = MassifEnvironmentProcesses::Snowpack();
	Declaration.InputChannels = {
		MassifEnvironmentChannels::Altitude(), MassifEnvironmentChannels::AnnualPrecipitation(),
		MassifEnvironmentChannels::SeasonalTemperature(), MassifEnvironmentChannels::DirectInsolation(),
		MassifEnvironmentChannels::WindExposure()};

	// A closed stand pins a slab to the ground it stands on. Optional, so the pack
	// still solves in a graph with no vegetation in it — there is simply nothing
	// anchoring anything.
	// The precipitation solve says when in the year the water arrives and how much
	// of it arrives frozen. Optional, so the pack still walks a year in a graph
	// that holds only an annual total - it simply spreads it evenly, as it did
	// before either channel existed.
	Declaration.OptionalInputChannels = {
		MassifEnvironmentChannels::VegetationCanopy(), MassifEnvironmentChannels::SeasonalPrecipitation(),
		MassifEnvironmentChannels::SnowFraction()};
	Declaration.SupportedFidelities = {
		EMassifFidelity::Cheap, EMassifFidelity::Standard, EMassifFidelity::Reference};

	// Vegetation reads how long this snow lies, the canopy it grows slows the wind
	// that moved the snow in the first place, and the seasonal temperature reads the
	// albedo written here: the pack sits inside all three loops.
	Declaration.bParticipatesInFeedback = true;

	Declaration.OutputChannels.Add(MakeSnowChannel(
		MassifEnvironmentChannels::SnowDepth(), TEXT("m"), TEXT("Snow lying on the ground"),
		Params.CellSizeMeters, SnowSliceCount));
	Declaration.OutputChannels.Add(MakeSnowChannel(
		MassifEnvironmentChannels::PeakSnowDepth(), TEXT("m"), TEXT("Deepest the pack gets over the year"),
		Params.CellSizeMeters, 1));
	Declaration.OutputChannels.Add(MakeSnowChannel(
		MassifEnvironmentChannels::SnowDensity(), TEXT("kg/m3"), TEXT("Density of the lying pack"),
		Params.CellSizeMeters, SnowSliceCount));

	Declaration.OutputChannels.Add(MakeSnowFractionChannel(
		MassifEnvironmentChannels::SnowCoverDuration(), TEXT("Share of the year with snow lying"),
		Params.CellSizeMeters, 1));
	Declaration.OutputChannels.Add(MakeSnowFractionChannel(
		MassifEnvironmentChannels::SnowAlbedo(), TEXT("Share of the sunlight the ground throws back"),
		Params.CellSizeMeters, SnowSliceCount));
	Declaration.OutputChannels.Add(MakeSnowFractionChannel(
		MassifEnvironmentChannels::AvalancheRelease(), TEXT("How readily the ground lets a slab go"),
		Params.CellSizeMeters, 1));
	Declaration.OutputChannels.Add(MakeSnowFractionChannel(
		MassifEnvironmentChannels::AvalanchePath(), TEXT("Ground the release areas above it run over"),
		Params.CellSizeMeters, 1));
	return Declaration;
}

void FSnowpackProcess::WriteBareGround(FMassifEnvironmentContext& Context) const
{
	// Off is a world nothing ever lies on, and every channel reads empty rather
	// than plausible: a density of fresh snow or an albedo of bare ground would be
	// a claim about a pack that is not there, and the temperature that reads the
	// albedo back would take it for one. The channels still exist, so nothing
	// downstream has to ask whether the pack was solved.
	for (int32 TimeSlice = 0; TimeSlice < SnowSliceCount; ++TimeSlice)
	{
		WriteZeroedSlice(Context, MassifEnvironmentChannels::SnowDepth(), TimeSlice);
		WriteZeroedSlice(Context, MassifEnvironmentChannels::SnowDensity(), TimeSlice);
		WriteZeroedSlice(Context, MassifEnvironmentChannels::SnowAlbedo(), TimeSlice);
	}

	WriteZeroedSlice(Context, MassifEnvironmentChannels::PeakSnowDepth(), 0);
	WriteZeroedSlice(Context, MassifEnvironmentChannels::SnowCoverDuration(), 0);
	WriteZeroedSlice(Context, MassifEnvironmentChannels::AvalancheRelease(), 0);
	WriteZeroedSlice(Context, MassifEnvironmentChannels::AvalanchePath(), 0);
}

void FSnowpackProcess::WriteSliceOfYear(
	FMassifEnvironmentContext& Context, const FSnowpackSolver& Solver, const FSnowFidelitySettings& Settings,
	int32 TimeSlice) const
{
	WriteSliceValues(Context, MassifEnvironmentChannels::SnowDepth(), TimeSlice, Solver.GetDepthMeters());
	WriteSliceValues(
		Context, MassifEnvironmentChannels::SnowDensity(), TimeSlice,
		Solver.GetDensityKilogramsPerCubicMetre());

	if (!Settings.bTracksAlbedo)
	{
		WriteZeroedSlice(Context, MassifEnvironmentChannels::SnowAlbedo(), TimeSlice);
		return;
	}
	WriteSliceValues(Context, MassifEnvironmentChannels::SnowAlbedo(), TimeSlice, Solver.GetAlbedo());
}

TArray<double> FSnowpackProcess::WriteSolvedYear(
	FMassifEnvironmentContext& Context, FSnowpackSolver& Solver, const FSnowFidelitySettings& Settings) const
{
	TArray<double> PeakDepthMeters;
	TArray<double> LyingSliceCount;
	PeakDepthMeters.SetNumZeroed(Solver.GetDepthMeters().Num());
	LyingSliceCount.SetNumZeroed(Solver.GetDepthMeters().Num());

	for (int32 TimeSlice = 0; TimeSlice < SnowSliceCount; ++TimeSlice)
	{
		Solver.AdvanceSlice(TimeSlice);
		WriteSliceOfYear(Context, Solver, Settings, TimeSlice);
		AccumulateReductions(
			Solver.GetDepthMeters(), Params.LyingSnowThresholdMetres, PeakDepthMeters, LyingSliceCount);
	}

	WriteSliceValues(Context, MassifEnvironmentChannels::PeakSnowDepth(), 0, PeakDepthMeters);
	WriteSliceValues(
		Context, MassifEnvironmentChannels::SnowCoverDuration(), 0, CoverDurationFrom(LyingSliceCount));
	return PeakDepthMeters;
}

void FSnowpackProcess::WriteAvalanches(
	FMassifEnvironmentContext& Context, const FSnowInputs& Inputs, const FMassifChannelGrid& Grid,
	TArrayView<const double> PeakDepthMeters) const
{
	const FAvalancheGround Ground = CacheAvalancheGround(Inputs, Grid);
	const TArray<double> Release = SolveAvalancheRelease(Params, Grid, Ground, PeakDepthMeters);
	const TArray<double> Track = TraceAvalanchePaths(Params, Grid, Ground.AltitudeMeters, Release);

	WriteSliceValues(Context, MassifEnvironmentChannels::AvalancheRelease(), 0, Release);
	WriteSliceValues(Context, MassifEnvironmentChannels::AvalanchePath(), 0, Track);
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
	checkf(Inputs.GetInsolationSliceCount() == SnowSliceCount,
		TEXT("The pack walks %d slices of the year but the direct insolation holds %d"),
		SnowSliceCount, Inputs.GetInsolationSliceCount());

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

	const TArray<double> PeakDepthMeters = WriteSolvedYear(Context, Solver, Settings);
	if (!Settings.bSolvesAvalanches)
	{
		WriteZeroedSlice(Context, MassifEnvironmentChannels::AvalancheRelease(), 0);
		WriteZeroedSlice(Context, MassifEnvironmentChannels::AvalanchePath(), 0);
		return;
	}

	WriteAvalanches(Context, Inputs, Grid, PeakDepthMeters);
}

void MassifRegisterSnowProcesses(FMassifEnvironmentScheduler& Scheduler, const FMassifSnowParams& Params)
{
	Scheduler.RegisterProcess(MakeShared<FSnowpackProcess>(Params));
}
