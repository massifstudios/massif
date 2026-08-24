#include "MassifEnvironmentChannel.h"

namespace
{
	/** Descriptor text is compared by content: two callers may spell the same unit from different literals. */
	bool TextMatches(const TCHAR* Left, const TCHAR* Right)
	{
		if (Left == nullptr || Right == nullptr)
		{
			return Left == Right;
		}
		return FCString::Strcmp(Left, Right) == 0;
	}
}

bool FMassifChannelDescriptor::operator==(const FMassifChannelDescriptor& Other) const
{
	const bool bStorageMatches = Name == Other.Name && Type == Other.Type && Encoding == Other.Encoding
		&& CellSizeMeters == Other.CellSizeMeters && TimeSliceCount == Other.TimeSliceCount;
	const bool bPresentationMatches = ColorMap == Other.ColorMap && DisplayRange == Other.DisplayRange
		&& TextMatches(Unit, Other.Unit) && TextMatches(Description, Other.Description);
	return bStorageMatches && bPresentationMatches;
}

int32 MassifChannelComponentCount(EMassifChannelType Type)
{
	if (Type == EMassifChannelType::Vector2)
	{
		return 2;
	}
	return 1;
}

const TCHAR* MassifChannelTypeName(EMassifChannelType Type)
{
	switch (Type)
	{
		case EMassifChannelType::Scalar: return TEXT("Scalar");
		case EMassifChannelType::Vector2: return TEXT("Vector2");
		case EMassifChannelType::Classified: return TEXT("Classified");
		default: return TEXT("Unknown");
	}
}

const TCHAR* MassifChannelEncodingName(EMassifChannelEncoding Encoding)
{
	switch (Encoding)
	{
		case EMassifChannelEncoding::Uint8: return TEXT("Uint8");
		case EMassifChannelEncoding::Uint16: return TEXT("Uint16");
		case EMassifChannelEncoding::Float32: return TEXT("Float32");
		case EMassifChannelEncoding::Float64: return TEXT("Float64");
		case EMassifChannelEncoding::Logarithmic16: return TEXT("Logarithmic16");
		default: return TEXT("Unknown");
	}
}

int32 FMassifChannelGrid::CellIndex(int32 Column, int32 Row) const
{
	if (Column < 0 || Column >= Width || Row < 0 || Row >= Height)
	{
		return INDEX_NONE;
	}
	return Row * Width + Column;
}

FVector2d FMassifChannelGrid::CellCentreMeters(int32 Column, int32 Row) const
{
	// Row 0 is north, as in the elevation model, so rows run against +Y.
	const double X = -0.5 * WorldSizeXMeters + (Column + 0.5) * CellSizeMeters;
	const double Y = 0.5 * WorldSizeYMeters - (Row + 0.5) * CellSizeMeters;
	return FVector2d(X, Y);
}

FIntPoint FMassifChannelGrid::ContainingCell(FVector2d PositionMeters) const
{
	const int32 Column = FMath::FloorToInt32((PositionMeters.X + 0.5 * WorldSizeXMeters) / CellSizeMeters);
	const int32 Row = FMath::FloorToInt32((0.5 * WorldSizeYMeters - PositionMeters.Y) / CellSizeMeters);
	return FIntPoint(FMath::Clamp(Column, 0, Width - 1), FMath::Clamp(Row, 0, Height - 1));
}

FVector2d FMassifChannelGrid::CellCornerMeters(int32 Column, int32 Row) const
{
	return FVector2d(
		Column * CellSizeMeters - 0.5 * WorldSizeXMeters, 0.5 * WorldSizeYMeters - Row * CellSizeMeters);
}

FMassifChannelGrid FMassifChannelGrid::FromWorldSize(FVector2d WorldSizeMeters, double CellSizeMeters)
{
	FMassifChannelGrid Grid;
	Grid.CellSizeMeters = FMath::Max(CellSizeMeters, UE_DOUBLE_KINDA_SMALL_NUMBER);
	Grid.WorldSizeXMeters = WorldSizeMeters.X;
	Grid.WorldSizeYMeters = WorldSizeMeters.Y;
	Grid.Width = FMath::Max(2, FMath::RoundToInt32(Grid.WorldSizeXMeters / Grid.CellSizeMeters));
	Grid.Height = FMath::Max(2, FMath::RoundToInt32(Grid.WorldSizeYMeters / Grid.CellSizeMeters));
	return Grid;
}
