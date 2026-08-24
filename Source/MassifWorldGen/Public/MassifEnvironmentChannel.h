#pragma once

#include "CoreMinimal.h"

/**
 * Slices a seasonal channel covers a year in: half-monthly, slice 0 the first
 * half of January.
 *
 * One number for every process that resolves a year, so the sun, the climate
 * and the weather cannot disagree about what slice 14 means.
 */
constexpr int32 MassifSeasonalSliceCount = 24;

enum class EMassifChannelType : uint8
{
	Scalar,      // one component
	Vector2,     // two components: wind, flow direction
	Classified,  // one component of class indices; never interpolated
};

enum class EMassifChannelEncoding : uint8
{
	Uint8,
	Uint16,
	Float32,

	/** Raw doubles. For surfaces where a float's resolution is coarser than the differences that matter. */
	Float64,

	/** uint16 over the logarithm of the value, for quantities spanning orders of magnitude. */
	Logarithmic16,
};

enum class EMassifChannelColorMap : uint8
{
	Viridis,
	Magma,
	BlueWhite,
	Terrain,
	Categorical,
};

/** One simulated quantity over the world, and everything needed to store and draw it. */
struct FMassifChannelDescriptor
{
	FName Name;                                       // "Hydrology.FlowAccumulation"
	EMassifChannelType Type = EMassifChannelType::Scalar;
	EMassifChannelEncoding Encoding = EMassifChannelEncoding::Uint16;

	/** Cells this channel is solved at. Channels at different resolutions sample consistently. */
	double CellSizeMeters = 8.0;

	/** Slices of a representative year. 1 is a climatological mean, 24 is half-monthly. */
	int32 TimeSliceCount = 1;

	const TCHAR* Unit = TEXT("");
	const TCHAR* Description = TEXT("");

	EMassifChannelColorMap ColorMap = EMassifChannelColorMap::Viridis;

	/** Unset normalises debug views to the observed range. */
	TOptional<FVector2d> DisplayRange;

	MASSIFWORLDGEN_API bool operator==(const FMassifChannelDescriptor& Other) const;

	bool operator!=(const FMassifChannelDescriptor& Other) const
	{
		return !(*this == Other);
	}
};

/** Resolved channel index, so inner loops index rather than hash a name. */
struct FMassifChannelHandle
{
	int32 Index = INDEX_NONE;

	bool IsValid() const
	{
		return Index != INDEX_NONE;
	}

	bool operator==(const FMassifChannelHandle& Other) const
	{
		return Index == Other.Index;
	}
};

/** Components a channel type stores per cell. */
MASSIFWORLDGEN_API int32 MassifChannelComponentCount(EMassifChannelType Type);

MASSIFWORLDGEN_API const TCHAR* MassifChannelTypeName(EMassifChannelType Type);
MASSIFWORLDGEN_API const TCHAR* MassifChannelEncodingName(EMassifChannelEncoding Encoding);

/** Grid a channel is solved on, derived from the world extent and the channel's cell size. */
struct FMassifChannelGrid
{
	int32 Width = 0;
	int32 Height = 0;
	double CellSizeMeters = 0.0;
	double WorldSizeXMeters = 0.0;
	double WorldSizeYMeters = 0.0;

	int32 CellCount() const
	{
		return Width * Height;
	}

	/** INDEX_NONE outside the grid. */
	MASSIFWORLDGEN_API int32 CellIndex(int32 Column, int32 Row) const;

	/** Column 0 is west, row 0 north, origin at the world centre. */
	MASSIFWORLDGEN_API FVector2d CellCentreMeters(int32 Column, int32 Row) const;

	/** Cell a world position falls inside, clamped to the grid. Not the cell-centre lattice. */
	MASSIFWORLDGEN_API FIntPoint ContainingCell(FVector2d PositionMeters) const;

	/** North-west corner of a cell, which is the origin an offset inside it is measured from. */
	MASSIFWORLDGEN_API FVector2d CellCornerMeters(int32 Column, int32 Row) const;

	MASSIFWORLDGEN_API static FMassifChannelGrid FromWorldSize(FVector2d WorldSizeMeters, double CellSizeMeters);
};
