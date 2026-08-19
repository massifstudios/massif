#include "WorldGenDebugExport.h"

#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/Paths.h"

namespace
{
	bool SaveGrayscalePng(const TArray<FColor>& Pixels, int32 Resolution, const FString& FilePath)
	{
		const FImageView Image(const_cast<FColor*>(Pixels.GetData()), Resolution, Resolution);
		return FImageUtils::SaveImageByExtension(*FilePath, Image);
	}
}

bool FWorldGenDebugExport::SaveHeightfieldPngs(const FWorldGenResult& Result, const FString& Directory)
{
	if (!Result.IsValid())
	{
		return false;
	}
	IFileManager::Get().MakeDirectory(*Directory, true);
	const int32 Resolution = Result.Resolution;

	float MinHeight = TNumericLimits<float>::Max();
	float MaxHeight = TNumericLimits<float>::Lowest();
	for (const float Height : Result.HeightMeters)
	{
		MinHeight = FMath::Min(MinHeight, Height);
		MaxHeight = FMath::Max(MaxHeight, Height);
	}
	const float InverseRange = 1.0f / FMath::Max(1.0f, MaxHeight - MinHeight);

	TArray<FColor> HeightPixels;
	HeightPixels.SetNumUninitialized(Resolution * Resolution);
	for (int32 Index = 0; Index < Result.HeightMeters.Num(); ++Index)
	{
		const uint8 Gray = static_cast<uint8>(FMath::Clamp((Result.HeightMeters[Index] - MinHeight) * InverseRange, 0.0f, 1.0f) * 255.0f);
		HeightPixels[Index] = FColor(Gray, Gray, Gray, 255);
	}

	const FVector3f LightDirection = FVector3f(-0.5f, -0.5f, 0.7071f).GetSafeNormal();
	const float HalfSize = Result.WorldSizeMeters * 0.5f;
	TArray<FColor> ShadePixels;
	ShadePixels.SetNumUninitialized(Resolution * Resolution);
	ParallelFor(Resolution, [&](int32 PixelY)
	{
		const float WorldY = -HalfSize + PixelY * Result.MetersPerPixel;
		for (int32 PixelX = 0; PixelX < Resolution; ++PixelX)
		{
			const float WorldX = -HalfSize + PixelX * Result.MetersPerPixel;
			const FVector3f Normal = Result.SampleNormal(WorldX, WorldY);
			const float Shade = FMath::Clamp(Normal.Dot(LightDirection) * 0.5f + 0.5f, 0.0f, 1.0f);
			const uint8 Gray = static_cast<uint8>(Shade * 255.0f);
			ShadePixels[PixelY * Resolution + PixelX] = FColor(Gray, Gray, Gray, 255);
		}
	});

	const bool bSavedHeight = SaveGrayscalePng(HeightPixels, Resolution, Directory / TEXT("Heightfield.png"));
	const bool bSavedShade = SaveGrayscalePng(ShadePixels, Resolution, Directory / TEXT("Hillshade.png"));
	return bSavedHeight && bSavedShade;
}
