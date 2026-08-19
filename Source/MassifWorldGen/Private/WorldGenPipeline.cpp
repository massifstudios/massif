#include "WorldGenPipeline.h"

#include "MassifTerrainField.h"

FWorldGenResult FWorldGenPipeline::MaterialisePreviewWindow(const FMassifTerrainField& Field)
{
	const FWorldGenConfig& Config = Field.GetConfig();
	check(Config.Resolution > 1);

	FWorldGenResult Result;
	Result.Resolution = Config.Resolution;
	Result.WorldSizeMeters = Config.PreviewWindowSizeMeters;
	Result.MetersPerPixel = Config.PreviewWindowSizeMeters / static_cast<float>(Config.Resolution - 1);
	Result.HeightMeters.SetNumUninitialized(Config.Resolution * Config.Resolution);

	const double HalfWindow = Config.PreviewWindowSizeMeters * 0.5;
	const double Spacing = Result.MetersPerPixel;
	float* Heights = Result.HeightMeters.GetData();

	ParallelFor(Config.Resolution, [&Field, Heights, HalfWindow, Spacing, &Config](int32 PixelY)
	{
		const double PositionY = -HalfWindow + PixelY * Spacing;
		float* Row = Heights + PixelY * Config.Resolution;
		for (int32 PixelX = 0; PixelX < Config.Resolution; ++PixelX)
		{
			const double PositionX = -HalfWindow + PixelX * Spacing;
			Row[PixelX] = static_cast<float>(Field.SampleHeightMeters(FVector2d(PositionX, PositionY), Spacing));
		}
	});

	return Result;
}

FWorldGenResult FWorldGenPipeline::Generate(const FWorldGenConfig& Config)
{
	const double FieldStartSeconds = FPlatformTime::Seconds();
	const FMassifTerrainField Field(Config);
	const double FieldSeconds = FPlatformTime::Seconds() - FieldStartSeconds;

	UE_LOG(LogTemp, Display, TEXT("WorldGen field built in %.2f s"), FieldSeconds);

	const double WindowStartSeconds = FPlatformTime::Seconds();
	FWorldGenResult Result = MaterialisePreviewWindow(Field);
	UE_LOG(LogTemp, Display, TEXT("WorldGen preview: %d^2 over %.0f m in %.2f s"),
		Result.Resolution, Result.WorldSizeMeters, FPlatformTime::Seconds() - WindowStartSeconds);

	return Result;
}
