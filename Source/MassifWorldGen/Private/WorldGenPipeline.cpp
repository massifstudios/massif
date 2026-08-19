#include "WorldGenPipeline.h"

#include "GpuErosion.h"
#include "MassifNoise.h"
#include "MassifSeed.h"
#include "RiverNetwork.h"

namespace
{
	/**
	 * The river field sampled on a coarse lattice. Evaluating the whole graph
	 * costs the same for every point, so it is done once per lattice node and
	 * interpolated; the field is smooth at this spacing, and the noise detail
	 * that is not goes on afterwards at full resolution.
	 */
	struct FRiverFieldGrid
	{
		TArray<float> BaseHeightMeters;
		TArray<float> DistanceToWaterMeters;
		TArray<float> NearestWaterElevationMeters;
		int32 SamplesPerSide = 0;
		float MetersPerSample = 0.0f;
		float HalfWorldMeters = 0.0f;

		FRiverFieldGrid(const FWorldGenConfig& Config, const FRiverNetwork& Network, int32 InSamplesPerSide)
			: SamplesPerSide(InSamplesPerSide)
			, MetersPerSample(Config.WorldSizeMeters / static_cast<float>(InSamplesPerSide - 1))
			, HalfWorldMeters(Config.WorldSizeMeters * 0.5f)
		{
			BaseHeightMeters.SetNumUninitialized(SamplesPerSide * SamplesPerSide);
			DistanceToWaterMeters.SetNumUninitialized(SamplesPerSide * SamplesPerSide);
			NearestWaterElevationMeters.SetNumUninitialized(SamplesPerSide * SamplesPerSide);

			ParallelFor(SamplesPerSide, [this, &Config, &Network](int32 SampleY)
			{
				FillRow(Config, Network, SampleY);
			});
		}

		void FillRow(const FWorldGenConfig& Config, const FRiverNetwork& Network, int32 SampleY)
		{
			const float PositionY = -HalfWorldMeters + static_cast<float>(SampleY) * MetersPerSample;
			for (int32 SampleX = 0; SampleX < SamplesPerSide; ++SampleX)
			{
				const float PositionX = -HalfWorldMeters + static_cast<float>(SampleX) * MetersPerSample;
				const FRiverTerrainSample Sample = Network.Sample(FVector2f(PositionX, PositionY), Config);
				const int32 Index = SampleY * SamplesPerSide + SampleX;
				BaseHeightMeters[Index] = Sample.BaseHeightMeters;
				DistanceToWaterMeters[Index] = Sample.DistanceToWaterMeters;
				NearestWaterElevationMeters[Index] = Sample.NearestWaterElevationMeters;
			}
		}

		FRiverTerrainSample SampleBilinear(FVector2f PositionMeters) const
		{
			const float SampleSpaceX = (PositionMeters.X + HalfWorldMeters) / MetersPerSample;
			const float SampleSpaceY = (PositionMeters.Y + HalfWorldMeters) / MetersPerSample;
			const int32 LowX = FMath::Clamp(FMath::FloorToInt32(SampleSpaceX), 0, SamplesPerSide - 1);
			const int32 LowY = FMath::Clamp(FMath::FloorToInt32(SampleSpaceY), 0, SamplesPerSide - 1);
			const int32 HighX = FMath::Min(LowX + 1, SamplesPerSide - 1);
			const int32 HighY = FMath::Min(LowY + 1, SamplesPerSide - 1);
			const float FractionX = FMath::Clamp(SampleSpaceX - static_cast<float>(LowX), 0.0f, 1.0f);
			const float FractionY = FMath::Clamp(SampleSpaceY - static_cast<float>(LowY), 0.0f, 1.0f);

			FRiverTerrainSample Result;
			Result.BaseHeightMeters = BlendField(BaseHeightMeters, LowX, LowY, HighX, HighY, FractionX, FractionY);
			Result.DistanceToWaterMeters = BlendField(DistanceToWaterMeters, LowX, LowY, HighX, HighY, FractionX, FractionY);
			Result.NearestWaterElevationMeters = BlendField(NearestWaterElevationMeters, LowX, LowY, HighX, HighY, FractionX, FractionY);
			return Result;
		}

		float BlendField(const TArray<float>& Field, int32 LowX, int32 LowY, int32 HighX, int32 HighY, float FractionX, float FractionY) const
		{
			const float Top = FMath::Lerp(Field[LowY * SamplesPerSide + LowX], Field[LowY * SamplesPerSide + HighX], FractionX);
			const float Bottom = FMath::Lerp(Field[HighY * SamplesPerSide + LowX], Field[HighY * SamplesPerSide + HighX], FractionX);
			return FMath::Lerp(Top, Bottom, FractionY);
		}
	};

	/**
	 * Terrain built around the river graph: the network fixes where the valleys
	 * run and how high their floors sit, and noise only decorates the slopes it
	 * leaves behind.
	 */
	struct FHeightfieldSampler
	{
		FWorldGenConfig Config;
		const FRiverFieldGrid& RiverField;
		uint32 RidgeSeed = 0;
		uint32 DetailSeed = 0;
		uint32 WarpSeedX = 0;
		uint32 WarpSeedY = 0;

		FHeightfieldSampler(const FWorldGenConfig& InConfig, const FRiverFieldGrid& InRiverField)
			: Config(InConfig)
			, RiverField(InRiverField)
		{
			const FMassifSeed Seed(static_cast<uint32>(InConfig.MasterSeed));
			RidgeSeed = Seed.MakeChannelSeed(TEXT("WorldGen.Ridge"));
			DetailSeed = Seed.MakeChannelSeed(TEXT("WorldGen.Detail"));
			WarpSeedX = Seed.MakeChannelSeed(TEXT("WorldGen.WarpX"));
			WarpSeedY = Seed.MakeChannelSeed(TEXT("WorldGen.WarpY"));
		}

		/** UnitX/UnitY in [0, 1] across the map; returns altitude in meters. */
		float SampleAltitude(float UnitX, float UnitY) const
		{
			const FVector2f Position = WarpedPosition(UnitX, UnitY);
			const FRiverTerrainSample River = RiverField.SampleBilinear(Position);

			// Everything above the valley floor is a slope, and how far up it a
			// point sits decides how rough the ground gets.
			const float HeightAboveWater = FMath::Max(0.0f, River.BaseHeightMeters - River.NearestWaterElevationMeters);
			const float SlopeFraction = FMath::Clamp(HeightAboveWater / FMath::Max(1.0f, Config.ValleyReliefMeters), 0.0f, 1.0f);

			const float Altitude = River.BaseHeightMeters
				+ RidgeRelief(UnitX, UnitY, SlopeFraction)
				+ DetailRelief(UnitX, UnitY, SlopeFraction);
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

		/** Ridged noise on the crests, faded out towards the valley floors. */
		float RidgeRelief(float UnitX, float UnitY, float SlopeFraction) const
		{
			const float Ridged = MassifNoise::RidgedFractal(
				UnitX * Config.RidgeFrequency, UnitY * Config.RidgeFrequency,
				RidgeSeed, Config.RidgeOctaveCount, 2.0f, 0.5f, Config.RidgeSharpness);
			return (Ridged - 0.5f) * Config.RidgeAmplitudeMeters * SlopeFraction * SlopeFraction;
		}

		float DetailRelief(float UnitX, float UnitY, float SlopeFraction) const
		{
			const float Detail = MassifNoise::Fractal(
				UnitX * Config.DetailFrequency, UnitY * Config.DetailFrequency, DetailSeed, 5, 2.0f, 0.5f);
			return Detail * Config.DetailAmplitudeMeters * (0.15f + 0.85f * SlopeFraction);
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

	const FRiverNetwork Network = FRiverNetwork::Build(Config);
	const FRiverFieldGrid RiverField(Config, Network, Config.RiverFieldResolution);
	UE_LOG(LogTemp, Display, TEXT("WorldGen rivers: %d graph nodes, %d^2 field"),
		Network.GetNodes().Num(), Config.RiverFieldResolution);

	const FHeightfieldSampler Sampler(Config, RiverField);
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
