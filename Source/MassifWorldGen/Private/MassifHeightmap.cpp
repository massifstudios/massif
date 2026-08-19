#include "MassifHeightmap.h"

#include "Misc/FileHelper.h"

namespace
{
	const uint8 DemMagic[8] = { 'M', 'A', 'S', 'S', 'I', 'F', 'D', 'M' };
	constexpr int32 DemHeaderBytes = 192;
	constexpr uint32 DemVersion = 1;
	constexpr int32 DemCoordinateSystemBytes = 32;
	constexpr int32 DemLabelBytes = 96;

	/** Below this a level has too few samples for the interpolation window to mean anything. */
	constexpr int32 SmallestMipSide = 4;

	template <typename ValueType>
	ValueType ReadAt(TArrayView<const uint8> Bytes, int32 Offset)
	{
		ValueType Value;
		FMemory::Memcpy(&Value, Bytes.GetData() + Offset, sizeof(ValueType));
		return Value;
	}

	FString ReadFixedString(TArrayView<const uint8> Bytes, int32 Offset, int32 MaxLength)
	{
		const char* Characters = reinterpret_cast<const char*>(Bytes.GetData() + Offset);
		int32 Length = 0;
		while (Length < MaxLength && Characters[Length] != '\0')
		{
			++Length;
		}
		const FUTF8ToTCHAR Converted(Characters, Length);
		return FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());
	}

	double CatmullRom(double Before, double Start, double End, double After, double Fraction)
	{
		const double FractionSquared = Fraction * Fraction;
		const double FractionCubed = FractionSquared * Fraction;
		return 0.5 * ((2.0 * Start)
			+ (-Before + End) * Fraction
			+ (2.0 * Before - 5.0 * Start + 4.0 * End - After) * FractionSquared
			+ (-Before + 3.0 * Start - 3.0 * End + After) * FractionCubed);
	}
}

TSharedPtr<const FMassifHeightmap> FMassifHeightmap::LoadFromFile(const FString& FilePath, FString& OutError)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *FilePath))
	{
		OutError = FString::Printf(TEXT("cannot read %s"), *FilePath);
		return nullptr;
	}
	return LoadFromBytes(Bytes, OutError);
}

TSharedPtr<const FMassifHeightmap> FMassifHeightmap::LoadFromBytes(TArrayView<const uint8> Bytes, FString& OutError)
{
	if (Bytes.Num() < DemHeaderBytes || FMemory::Memcmp(Bytes.GetData(), DemMagic, sizeof(DemMagic)) != 0)
	{
		OutError = TEXT("not a .massifdem file");
		return nullptr;
	}

	const uint32 Version = ReadAt<uint32>(Bytes, 8);
	if (Version != DemVersion)
	{
		OutError = FString::Printf(TEXT("format version %u, expected %u"), Version, DemVersion);
		return nullptr;
	}

	const int32 Width = static_cast<int32>(ReadAt<uint32>(Bytes, 12));
	const int32 Height = static_cast<int32>(ReadAt<uint32>(Bytes, 16));
	const int64 SampleBytes = static_cast<int64>(Width) * Height * sizeof(uint16);
	if (Width < SmallestMipSide || Height < SmallestMipSide || Bytes.Num() != DemHeaderBytes + SampleBytes)
	{
		OutError = FString::Printf(TEXT("%d x %d samples do not match %d bytes of payload"),
			Width, Height, Bytes.Num() - DemHeaderBytes);
		return nullptr;
	}

	TSharedRef<FMassifHeightmap> Heightmap = MakeShared<FMassifHeightmap>();
	Heightmap->FinestSpacingMeters = ReadAt<double>(Bytes, 24);
	Heightmap->QuantisationBaseMeters = ReadAt<double>(Bytes, 32);
	Heightmap->QuantisationStepMeters = ReadAt<double>(Bytes, 40);
	Heightmap->SourceInfo.OriginEastingMeters = ReadAt<double>(Bytes, 48);
	Heightmap->SourceInfo.OriginNorthingMeters = ReadAt<double>(Bytes, 56);
	Heightmap->SourceInfo.CoordinateSystem = ReadFixedString(Bytes, 64, DemCoordinateSystemBytes);
	Heightmap->SourceInfo.Label = ReadFixedString(Bytes, 96, DemLabelBytes);

	if (!(Heightmap->FinestSpacingMeters > 0.0) || !(Heightmap->QuantisationStepMeters > 0.0))
	{
		OutError = TEXT("sample spacing and altitude step must be positive");
		return nullptr;
	}

	FMipLevel& FinestLevel = Heightmap->MipLevels.AddDefaulted_GetRef();
	FinestLevel.Width = Width;
	FinestLevel.Height = Height;
	FinestLevel.SpacingXMeters = Heightmap->FinestSpacingMeters;
	FinestLevel.SpacingYMeters = Heightmap->FinestSpacingMeters;
	FinestLevel.Samples.SetNumUninitialized(Width * Height);
	FMemory::Memcpy(FinestLevel.Samples.GetData(), Bytes.GetData() + DemHeaderBytes, SampleBytes);

	Heightmap->BuildMipChain();
	Heightmap->ComputeAltitudeRange();
	return Heightmap;
}

TSharedRef<const FMassifHeightmap> FMassifHeightmap::FromAltitudes(
	TArrayView<const double> AltitudesMeters, int32 Width, int32 Height, double SampleSpacingMeters)
{
	check(Width >= SmallestMipSide && Height >= SmallestMipSide);
	check(AltitudesMeters.Num() == Width * Height);
	check(SampleSpacingMeters > 0.0);

	double Lowest = TNumericLimits<double>::Max();
	double Highest = TNumericLimits<double>::Lowest();
	for (const double AltitudeMeters : AltitudesMeters)
	{
		Lowest = FMath::Min(Lowest, AltitudeMeters);
		Highest = FMath::Max(Highest, AltitudeMeters);
	}

	TSharedRef<FMassifHeightmap> Heightmap = MakeShared<FMassifHeightmap>();
	Heightmap->FinestSpacingMeters = SampleSpacingMeters;
	Heightmap->QuantisationBaseMeters = Lowest;
	Heightmap->QuantisationStepMeters = FMath::Max((Highest - Lowest) / static_cast<double>(MAX_uint16), UE_DOUBLE_SMALL_NUMBER);
	Heightmap->SourceInfo.Label = TEXT("generated");

	FMipLevel& FinestLevel = Heightmap->MipLevels.AddDefaulted_GetRef();
	FinestLevel.Width = Width;
	FinestLevel.Height = Height;
	FinestLevel.SpacingXMeters = SampleSpacingMeters;
	FinestLevel.SpacingYMeters = SampleSpacingMeters;
	FinestLevel.Samples.SetNumUninitialized(Width * Height);
	for (int32 Index = 0; Index < AltitudesMeters.Num(); ++Index)
	{
		const double Steps = (AltitudesMeters[Index] - Lowest) / Heightmap->QuantisationStepMeters;
		FinestLevel.Samples[Index] = static_cast<uint16>(FMath::Clamp(FMath::RoundToDouble(Steps), 0.0, static_cast<double>(MAX_uint16)));
	}

	Heightmap->BuildMipChain();
	Heightmap->ComputeAltitudeRange();
	return Heightmap;
}

uint16 FMassifHeightmap::AverageQuad(const FMipLevel& Level, int32 Column, int32 Row)
{
	const int32 RightColumn = FMath::Min(Column + 1, Level.Width - 1);
	const int32 LowerRow = FMath::Min(Row + 1, Level.Height - 1);
	const uint32 Total = Level.Samples[Row * Level.Width + Column]
		+ Level.Samples[Row * Level.Width + RightColumn]
		+ Level.Samples[LowerRow * Level.Width + Column]
		+ Level.Samples[LowerRow * Level.Width + RightColumn];
	return static_cast<uint16>((Total + 2) / 4);
}

void FMassifHeightmap::BuildMipChain()
{
	// Every level covers the same world box, so a level's spacing follows from
	// its sample count rather than from doubling. Odd sample counts would
	// otherwise walk the levels apart from each other and from the border.
	const double WorldSizeX = GetWorldSizeXMeters();
	const double WorldSizeY = GetWorldSizeYMeters();

	while (MipLevels.Last().Width >= SmallestMipSide * 2 && MipLevels.Last().Height >= SmallestMipSide * 2)
	{
		const FMipLevel& FinerLevel = MipLevels.Last();
		// Rounded up, so a level with an odd sample count still spans the whole
		// box; the last row and column then average fewer samples rather than
		// dropping the strip and shifting the level against the finer ones.
		const int32 Width = (FinerLevel.Width + 1) / 2;
		const int32 Height = (FinerLevel.Height + 1) / 2;

		FMipLevel CoarserLevel;
		CoarserLevel.Width = Width;
		CoarserLevel.Height = Height;
		CoarserLevel.SpacingXMeters = WorldSizeX / static_cast<double>(Width);
		CoarserLevel.SpacingYMeters = WorldSizeY / static_cast<double>(Height);
		CoarserLevel.Samples.SetNumUninitialized(Width * Height);

		for (int32 Row = 0; Row < Height; ++Row)
		{
			for (int32 Column = 0; Column < Width; ++Column)
			{
				CoarserLevel.Samples[Row * Width + Column] = AverageQuad(FinerLevel, Column * 2, Row * 2);
			}
		}
		MipLevels.Add(MoveTemp(CoarserLevel));
	}
}

void FMassifHeightmap::ComputeAltitudeRange()
{
	uint16 Lowest = MAX_uint16;
	uint16 Highest = 0;
	for (const uint16 Sample : MipLevels[0].Samples)
	{
		Lowest = FMath::Min(Lowest, Sample);
		Highest = FMath::Max(Highest, Sample);
	}
	MinAltitudeMeters = QuantisationBaseMeters + Lowest * QuantisationStepMeters;
	MaxAltitudeMeters = QuantisationBaseMeters + Highest * QuantisationStepMeters;
}

double FMassifHeightmap::GetWorldSizeXMeters() const
{
	return MipLevels[0].Width * FinestSpacingMeters;
}

double FMassifHeightmap::GetWorldSizeYMeters() const
{
	return MipLevels[0].Height * FinestSpacingMeters;
}

double FMassifHeightmap::GetSampleMeters(const FMipLevel& Level, int32 Column, int32 Row) const
{
	const int32 ClampedColumn = FMath::Clamp(Column, 0, Level.Width - 1);
	const int32 ClampedRow = FMath::Clamp(Row, 0, Level.Height - 1);
	return QuantisationBaseMeters + Level.Samples[ClampedRow * Level.Width + ClampedColumn] * QuantisationStepMeters;
}

double FMassifHeightmap::SampleColumnRunMeters(const FMipLevel& Level, int32 Column, int32 Row, double ColumnFraction) const
{
	return CatmullRom(
		GetSampleMeters(Level, Column - 1, Row),
		GetSampleMeters(Level, Column, Row),
		GetSampleMeters(Level, Column + 1, Row),
		GetSampleMeters(Level, Column + 2, Row),
		ColumnFraction);
}

double FMassifHeightmap::SampleLevelMeters(const FMipLevel& Level, FVector2d PositionMeters) const
{
	// Sample centres sit half a cell inside the world box, and row 0 is north.
	const double ColumnCoordinate = (PositionMeters.X + GetWorldSizeXMeters() * 0.5) / Level.SpacingXMeters - 0.5;
	const double RowCoordinate = (GetWorldSizeYMeters() * 0.5 - PositionMeters.Y) / Level.SpacingYMeters - 0.5;

	const int32 Column = static_cast<int32>(FMath::FloorToDouble(ColumnCoordinate));
	const int32 Row = static_cast<int32>(FMath::FloorToDouble(RowCoordinate));
	const double ColumnFraction = ColumnCoordinate - Column;
	const double RowFraction = RowCoordinate - Row;

	return CatmullRom(
		SampleColumnRunMeters(Level, Column, Row - 1, ColumnFraction),
		SampleColumnRunMeters(Level, Column, Row, ColumnFraction),
		SampleColumnRunMeters(Level, Column, Row + 1, ColumnFraction),
		SampleColumnRunMeters(Level, Column, Row + 2, ColumnFraction),
		RowFraction);
}

double FMassifHeightmap::SampleHeightMeters(FVector2d PositionMeters, double SampleSpacingMeters) const
{
	// The level whose samples are about as far apart as the caller's, blended
	// with the next one so height is continuous in spacing: a tile and its
	// coarser neighbour meet at the same ground instead of stepping.
	const double SpacingRatio = FMath::Max(SampleSpacingMeters, UE_DOUBLE_SMALL_NUMBER) / FinestSpacingMeters;
	const double LevelCoordinate = FMath::Clamp(
		FMath::Log2(SpacingRatio), 0.0, static_cast<double>(MipLevels.Num() - 1));

	const int32 FinerLevel = static_cast<int32>(FMath::FloorToDouble(LevelCoordinate));
	const double CoarserWeight = LevelCoordinate - FinerLevel;
	const double FinerHeightMeters = SampleLevelMeters(MipLevels[FinerLevel], PositionMeters);
	if (CoarserWeight <= 0.0)
	{
		return FinerHeightMeters;
	}

	const double CoarserHeightMeters = SampleLevelMeters(MipLevels[FinerLevel + 1], PositionMeters);
	return FMath::Lerp(FinerHeightMeters, CoarserHeightMeters, CoarserWeight);
}
