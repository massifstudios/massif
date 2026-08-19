#include "WorldGenResult.h"

#include "MassifStateHash.h"

float FWorldGenResult::GetHeightAtPixel(int32 PixelX, int32 PixelY) const
{
	const int32 ClampedX = FMath::Clamp(PixelX, 0, Resolution - 1);
	const int32 ClampedY = FMath::Clamp(PixelY, 0, Resolution - 1);
	return HeightMeters[ClampedY * Resolution + ClampedX];
}

float FWorldGenResult::SampleHeightMeters(float WorldX, float WorldY) const
{
	const float HalfSize = WorldSizeMeters * 0.5f;
	const float PixelSpaceX = (WorldX + HalfSize) / MetersPerPixel;
	const float PixelSpaceY = (WorldY + HalfSize) / MetersPerPixel;

	const int32 BaseX = FMath::FloorToInt32(PixelSpaceX);
	const int32 BaseY = FMath::FloorToInt32(PixelSpaceY);
	const float FracX = FMath::Clamp(PixelSpaceX - static_cast<float>(BaseX), 0.0f, 1.0f);
	const float FracY = FMath::Clamp(PixelSpaceY - static_cast<float>(BaseY), 0.0f, 1.0f);

	const float North = FMath::Lerp(GetHeightAtPixel(BaseX, BaseY), GetHeightAtPixel(BaseX + 1, BaseY), FracX);
	const float South = FMath::Lerp(GetHeightAtPixel(BaseX, BaseY + 1), GetHeightAtPixel(BaseX + 1, BaseY + 1), FracX);
	return FMath::Lerp(North, South, FracY);
}

FVector3f FWorldGenResult::SampleNormal(float WorldX, float WorldY) const
{
	const float Step = MetersPerPixel;
	const float HeightWest = SampleHeightMeters(WorldX - Step, WorldY);
	const float HeightEast = SampleHeightMeters(WorldX + Step, WorldY);
	const float HeightSouth = SampleHeightMeters(WorldX, WorldY - Step);
	const float HeightNorth = SampleHeightMeters(WorldX, WorldY + Step);

	const FVector3f Normal(HeightWest - HeightEast, HeightSouth - HeightNorth, 2.0f * Step);
	return Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector3f::UpVector);
}

float FWorldGenResult::SampleSlopeDegrees(float WorldX, float WorldY) const
{
	const float NormalZ = FMath::Clamp(SampleNormal(WorldX, WorldY).Z, -1.0f, 1.0f);
	return FMath::RadiansToDegrees(FMath::Acos(NormalZ));
}

namespace
{
	bool IsLowerThanNeighbors(const TArray<float>& HeightMeters, int32 Resolution, int32 PixelX, int32 PixelY)
	{
		const int32 Index = PixelY * Resolution + PixelX;
		const float Height = HeightMeters[Index];
		return Height < HeightMeters[Index - 1]
			&& Height < HeightMeters[Index + 1]
			&& Height < HeightMeters[Index - Resolution]
			&& Height < HeightMeters[Index + Resolution];
	}
}

int32 FWorldGenResult::CountLocalMinima() const
{
	int32 MinimumCount = 0;
	for (int32 PixelY = 1; PixelY < Resolution - 1; ++PixelY)
	{
		for (int32 PixelX = 1; PixelX < Resolution - 1; ++PixelX)
		{
			MinimumCount += IsLowerThanNeighbors(HeightMeters, Resolution, PixelX, PixelY);
		}
	}
	return MinimumCount;
}

uint64 FWorldGenResult::ComputeHash() const
{
	FMassifStateHash StateHash;
	StateHash.AppendInt(Resolution);
	StateHash.AppendFloats(TConstArrayView<float>(&WorldSizeMeters, 1));
	StateHash.AppendFloats(HeightMeters);
	return StateHash.Finalize();
}
