#include "WorldGenPipeline.h"

#include "GpuErosion.h"
#include "MassifErosionFilter.h"
#include "MassifNoise.h"
#include "MassifSeed.h"
#include "RiverNetwork.h"

namespace
{
	/** Ridged noise on the crests, faded out towards the valley floors. */
	float RidgeRelief(const FWorldGenConfig& Config, uint32 RidgeSeed, FVector2f PositionMeters, float ReliefFraction)
	{
		const float CyclesPerMeter = Config.RidgeFrequency / FMath::Max(1.0f, Config.WorldSizeMeters);
		const float Ridged = MassifNoise::RidgedFractal(
			PositionMeters.X * CyclesPerMeter, PositionMeters.Y * CyclesPerMeter,
			RidgeSeed, Config.RidgeOctaveCount, 2.0f, 0.5f, Config.RidgeSharpness);
		return (Ridged - 0.5f) * Config.RidgeAmplitudeMeters * ReliefFraction * ReliefFraction;
	}

	struct FMacroSurfaceSample
	{
		float HeightMeters = 0.0f;
		FVector2f SlopeMetersPerMeter = FVector2f::ZeroVector;
		/** 0 on a valley floor, 1 where the valley side has climbed its full relief. */
		float ReliefFraction = 0.0f;
	};

	/**
	 * The landform without any erosion detail: river valleys plus the ridge
	 * noise riding on their crests, sampled on a coarse lattice. Evaluating the
	 * whole river graph costs the same for every point, so it is done once per
	 * lattice node and interpolated.
	 *
	 * The lattice also yields the surface slope by central difference. That
	 * slope is what orients the erosion filter, so it has to come from the
	 * landform alone: a slope read off the finished terrain would already
	 * contain the gullies the filter is about to cut.
	 */
	struct FMacroSurfaceGrid
	{
		TArray<float> HeightMeters;
		TArray<float> SlopeX;
		TArray<float> SlopeY;
		TArray<float> ReliefFraction;
		int32 SamplesPerSide = 0;
		float MetersPerSample = 0.0f;
		float HalfWorldMeters = 0.0f;

		FMacroSurfaceGrid(const FWorldGenConfig& Config, const FRiverNetwork& Network, uint32 RidgeSeed)
			: SamplesPerSide(FMath::Max(2, Config.RiverFieldResolution))
			, MetersPerSample(Config.WorldSizeMeters / static_cast<float>(FMath::Max(2, Config.RiverFieldResolution) - 1))
			, HalfWorldMeters(Config.WorldSizeMeters * 0.5f)
		{
			const int32 SampleCount = SamplesPerSide * SamplesPerSide;
			HeightMeters.SetNumUninitialized(SampleCount);
			SlopeX.SetNumUninitialized(SampleCount);
			SlopeY.SetNumUninitialized(SampleCount);
			ReliefFraction.SetNumUninitialized(SampleCount);

			ParallelFor(SamplesPerSide, [this, &Config, &Network, RidgeSeed](int32 SampleY)
			{
				FillHeightRow(Config, Network, RidgeSeed, SampleY);
			});
			ParallelFor(SamplesPerSide, [this](int32 SampleY)
			{
				FillSlopeRow(SampleY);
			});
		}

		float PositionAt(int32 SampleIndex) const
		{
			return -HalfWorldMeters + static_cast<float>(SampleIndex) * MetersPerSample;
		}

		int32 ClampSample(int32 SampleIndex) const
		{
			return FMath::Clamp(SampleIndex, 0, SamplesPerSide - 1);
		}

		void FillHeightRow(const FWorldGenConfig& Config, const FRiverNetwork& Network, uint32 RidgeSeed, int32 SampleY)
		{
			const float PositionY = PositionAt(SampleY);
			for (int32 SampleX = 0; SampleX < SamplesPerSide; ++SampleX)
			{
				const FVector2f Position(PositionAt(SampleX), PositionY);
				const FRiverTerrainSample River = Network.Sample(Position, Config);
				const float HeightAboveWater = FMath::Max(0.0f, River.BaseHeightMeters - River.NearestWaterElevationMeters);
				const float Relief = FMath::Clamp(HeightAboveWater / FMath::Max(1.0f, Config.ValleyReliefMeters), 0.0f, 1.0f);

				const int32 Index = SampleY * SamplesPerSide + SampleX;
				ReliefFraction[Index] = Relief;
				HeightMeters[Index] = River.BaseHeightMeters + RidgeRelief(Config, RidgeSeed, Position, Relief);
			}
		}

		void FillSlopeRow(int32 SampleY)
		{
			for (int32 SampleX = 0; SampleX < SamplesPerSide; ++SampleX)
			{
				const int32 Index = SampleY * SamplesPerSide + SampleX;
				SlopeX[Index] = CentralDifference(SampleX, SampleY, 1, 0);
				SlopeY[Index] = CentralDifference(SampleX, SampleY, 0, 1);
			}
		}

		/** One of the steps is always zero, so the clamped index span is the distance travelled. */
		float CentralDifference(int32 SampleX, int32 SampleY, int32 StepX, int32 StepY) const
		{
			const int32 HighX = ClampSample(SampleX + StepX);
			const int32 HighY = ClampSample(SampleY + StepY);
			const int32 LowX = ClampSample(SampleX - StepX);
			const int32 LowY = ClampSample(SampleY - StepY);
			const float RiseMeters = HeightMeters[HighY * SamplesPerSide + HighX] - HeightMeters[LowY * SamplesPerSide + LowX];
			const float RunMeters = static_cast<float>((HighX - LowX) + (HighY - LowY)) * MetersPerSample;
			return RiseMeters / FMath::Max(RunMeters, MetersPerSample);
		}

		/**
		 * Smoothstepped rather than straight bilinear: at this lattice spacing a
		 * plain lerp leaves the cell diagonals as visible creases once the
		 * heightfield is shaded.
		 */
		FMacroSurfaceSample SampleBilinear(FVector2f PositionMeters) const
		{
			const float SampleSpaceX = (PositionMeters.X + HalfWorldMeters) / MetersPerSample;
			const float SampleSpaceY = (PositionMeters.Y + HalfWorldMeters) / MetersPerSample;
			const int32 LowX = ClampSample(FMath::FloorToInt32(SampleSpaceX));
			const int32 LowY = ClampSample(FMath::FloorToInt32(SampleSpaceY));
			const int32 HighX = ClampSample(LowX + 1);
			const int32 HighY = ClampSample(LowY + 1);
			const float FractionX = FMath::SmoothStep(0.0f, 1.0f, SampleSpaceX - static_cast<float>(LowX));
			const float FractionY = FMath::SmoothStep(0.0f, 1.0f, SampleSpaceY - static_cast<float>(LowY));

			FMacroSurfaceSample Result;
			Result.HeightMeters = BlendField(HeightMeters, LowX, LowY, HighX, HighY, FractionX, FractionY);
			Result.SlopeMetersPerMeter = FVector2f(
				BlendField(SlopeX, LowX, LowY, HighX, HighY, FractionX, FractionY),
				BlendField(SlopeY, LowX, LowY, HighX, HighY, FractionX, FractionY));
			Result.ReliefFraction = BlendField(ReliefFraction, LowX, LowY, HighX, HighY, FractionX, FractionY);
			return Result;
		}

		float BlendField(const TArray<float>& Field, int32 LowX, int32 LowY, int32 HighX, int32 HighY, float FractionX, float FractionY) const
		{
			const float Top = FMath::Lerp(Field[LowY * SamplesPerSide + LowX], Field[LowY * SamplesPerSide + HighX], FractionX);
			const float Bottom = FMath::Lerp(Field[HighY * SamplesPerSide + LowX], Field[HighY * SamplesPerSide + HighX], FractionX);
			return FMath::Lerp(Top, Bottom, FractionY);
		}
	};

	FMassifErosionSettings MakeErosionFilterSettings(const FWorldGenConfig& Config, uint32 Seed)
	{
		FMassifErosionSettings Settings;
		Settings.OctaveCount = Config.GullyOctaveCount;
		Settings.LargestGullyWavelengthMeters = Config.LargestGullyWavelengthMeters;
		Settings.LargestGullyDepthMeters = Config.LargestGullyDepthMeters;
		Settings.WavelengthGain = Config.GullyWavelengthGain;
		Settings.DepthGain = Config.GullyDepthGain;
		Settings.CellsPerWavelength = Config.GullyCellsPerWavelength;
		Settings.SlopeReference = Config.GullySlopeReference;
		Settings.DetailPersistence = Config.GullyDetailPersistence;
		Settings.BranchStrength = Config.GullyBranchStrength;
		Settings.RidgeRounding = Config.GullyRidgeRounding;
		Settings.CreaseRounding = Config.GullyCreaseRounding;
		Settings.Seed = Seed;
		return Settings;
	}

	/**
	 * Terrain built around the river graph: the network fixes where the valleys
	 * run and how high their floors sit, and the erosion filter cuts the gullies
	 * into the slopes they leave behind.
	 */
	struct FHeightfieldSampler
	{
		FWorldGenConfig Config;
		const FMacroSurfaceGrid& MacroSurface;
		FMassifErosionSettings ErosionSettings;
		uint32 WarpSeedX = 0;
		uint32 WarpSeedY = 0;

		FHeightfieldSampler(const FWorldGenConfig& InConfig, const FMacroSurfaceGrid& InMacroSurface, const FMassifErosionSettings& InErosionSettings)
			: Config(InConfig)
			, MacroSurface(InMacroSurface)
			, ErosionSettings(InErosionSettings)
		{
			const FMassifSeed Seed(static_cast<uint32>(InConfig.MasterSeed));
			WarpSeedX = Seed.MakeChannelSeed(TEXT("WorldGen.WarpX"));
			WarpSeedY = Seed.MakeChannelSeed(TEXT("WorldGen.WarpY"));
		}

		/** UnitX/UnitY in [0, 1] across the map; returns altitude in meters. */
		float SampleAltitude(float UnitX, float UnitY) const
		{
			const FVector2f Position = WarpedPosition(UnitX, UnitY);
			const FMacroSurfaceSample Macro = MacroSurface.SampleBilinear(Position);

			FMassifErosionInput ErosionInput;
			ErosionInput.PositionMeters = Position;
			ErosionInput.SlopeMetersPerMeter = Macro.SlopeMetersPerMeter;
			ErosionInput.ReliefFraction = Macro.ReliefFraction;

			const float Altitude = Macro.HeightMeters + MassifErosionFilter::EvaluateDisplacementMeters(ErosionInput, ErosionSettings);
			return FMath::Clamp(EdgeFalloff(UnitX, UnitY, Altitude), Config.MinAltitudeMeters, Config.MaxAltitudeMeters);
		}

		/** Query point pushed sideways by noise, so valleys wander instead of following the graph's straight segments. */
		FVector2f WarpedPosition(float UnitX, float UnitY) const
		{
			const float WarpX = MassifNoise::Fractal(
				UnitX * Config.TerrainWarpFrequency, UnitY * Config.TerrainWarpFrequency, WarpSeedX, 3, 2.0f, 0.5f);
			const float WarpY = MassifNoise::Fractal(
				UnitX * Config.TerrainWarpFrequency, UnitY * Config.TerrainWarpFrequency, WarpSeedY, 3, 2.0f, 0.5f);

			const float HalfWorld = Config.WorldSizeMeters * 0.5f;
			return FVector2f(
				-HalfWorld + UnitX * Config.WorldSizeMeters + WarpX * Config.TerrainWarpMeters,
				-HalfWorld + UnitY * Config.WorldSizeMeters + WarpY * Config.TerrainWarpMeters);
		}

		/** Pulls the outermost ground down to the mouth altitude so the map does not end on a cliff. */
		float EdgeFalloff(float UnitX, float UnitY, float Altitude) const
		{
			const float BorderDistance = FMath::Min(
				FMath::Min(UnitX, 1.0f - UnitX), FMath::Min(UnitY, 1.0f - UnitY));
			const float Falloff = FMath::Clamp(BorderDistance / Config.EdgeFalloffFraction, 0.0f, 1.0f);
			return FMath::Lerp(Config.MinAltitudeMeters, Altitude, FMath::SmoothStep(0.0f, 1.0f, Falloff));
		}
	};

	/**
	 * The erosion springs sit on the graph's headwaters, so the simulated water
	 * runs down the valleys the network already laid out instead of cutting its
	 * own unrelated set.
	 */
	TArray<FMassifRiverSource> MakeErosionSources(const FWorldGenConfig& Config, const FRiverNetwork& Network)
	{
		TArray<FMassifRiverSource> Sources;
		if (Config.RiverSourceRate <= 0.0f)
		{
			return Sources;
		}

		const float HalfWorld = Config.WorldSizeMeters * 0.5f;
		const float MetersPerCell = Config.WorldSizeMeters / static_cast<float>(Config.Resolution - 1);
		for (const FRiverNode& Node : Network.GetNodes())
		{
			if (Node.UpstreamNodeCount > 0)
			{
				continue;
			}
			const int32 CellX = FMath::Clamp(FMath::RoundToInt32((Node.PositionMeters.X + HalfWorld) / MetersPerCell), 0, Config.Resolution - 1);
			const int32 CellY = FMath::Clamp(FMath::RoundToInt32((Node.PositionMeters.Y + HalfWorld) / MetersPerCell), 0, Config.Resolution - 1);
			Sources.Add(FMassifRiverSource{ CellX, CellY, Config.RiverSourceRate });
		}
		return Sources;
	}
}

FWorldGenResult FWorldGenPipeline::Generate(const FWorldGenConfig& Config)
{
	check(Config.Resolution > 1);

	FWorldGenResult Result;
	Result.Resolution = Config.Resolution;
	Result.WorldSizeMeters = Config.WorldSizeMeters;
	Result.MetersPerPixel = Config.WorldSizeMeters / static_cast<float>(Config.Resolution - 1);
	Result.HeightMeters.SetNumUninitialized(Config.Resolution * Config.Resolution);

	const FMassifSeed Seed(static_cast<uint32>(Config.MasterSeed));
	const FRiverNetwork Network = FRiverNetwork::Build(Config);
	const FMacroSurfaceGrid MacroSurface(Config, Network, Seed.MakeChannelSeed(TEXT("WorldGen.Ridge")));
	UE_LOG(LogTemp, Display, TEXT("WorldGen rivers: %d graph nodes, %d^2 macro surface"),
		Network.GetNodes().Num(), Config.RiverFieldResolution);

	const FMassifErosionSettings ErosionSettings = MakeErosionFilterSettings(Config, Seed.MakeChannelSeed(TEXT("WorldGen.ErosionFilter")));
	const FHeightfieldSampler Sampler(Config, MacroSurface, ErosionSettings);
	const float UnitStep = 1.0f / static_cast<float>(Config.Resolution - 1);
	float* Heights = Result.HeightMeters.GetData();

	ParallelFor(Config.Resolution, [&Sampler, Heights, UnitStep, &Config](int32 PixelY)
	{
		const float UnitY = static_cast<float>(PixelY) * UnitStep;
		float* Row = Heights + PixelY * Config.Resolution;
		for (int32 PixelX = 0; PixelX < Config.Resolution; ++PixelX)
		{
			Row[PixelX] = Sampler.SampleAltitude(static_cast<float>(PixelX) * UnitStep, UnitY);
		}
	});

	const bool bWantsErosion = Config.HydraulicIterationCount > 0 || Config.ThermalIterationCount > 0;
	if (bWantsErosion && FMassifGpuErosion::CanRun())
	{
		const TArray<FMassifRiverSource> RiverSources = MakeErosionSources(Config, Network);
		const double ErosionStartSeconds = FPlatformTime::Seconds();
		const bool bEroded = FMassifGpuErosion::Run(Config, Result.HeightMeters, RiverSources);
		UE_LOG(LogTemp, Display, TEXT("WorldGen erosion: %s in %.2f s (%d hydraulic, %d thermal iterations, %d river sources)"),
			bEroded ? TEXT("applied") : TEXT("FAILED"), FPlatformTime::Seconds() - ErosionStartSeconds,
			Config.HydraulicIterationCount, Config.ThermalIterationCount, RiverSources.Num());
	}

	return Result;
}
