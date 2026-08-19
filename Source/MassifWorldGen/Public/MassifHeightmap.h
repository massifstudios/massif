#pragma once

#include "CoreMinimal.h"

/**
 * A measured terrain surface: the elevation model the world is built on,
 * imported from a public dataset by Tools/fetch_tirol_dem.py.
 *
 * Samples are quantised to uint16 over the window's own altitude band and
 * held in a mip chain, each level covering the same world box at half the
 * sample count. A query picks its level from the spacing the caller intends to
 * sample at and blends the two levels around it, so a coarse sampling and a
 * fine sampling of the same point agree instead of stepping where clipmap
 * rings meet — and distant tiles read a filtered surface instead of aliasing
 * the finest one.
 *
 * Coordinates are game world meters, origin at the centre of the window,
 * X east, Y north. Positions outside the window clamp to its border.
 *
 * Thread-safe once loaded: sampling touches only immutable state.
 */
class MASSIFWORLDGEN_API FMassifHeightmap
{
public:
	/** Georeference of the source data, carried through for later imports and overlays. */
	struct FSourceInfo
	{
		FString Label;
		FString CoordinateSystem;
		double OriginEastingMeters = 0.0;
		double OriginNorthingMeters = 0.0;
	};

	/** Reads a .massifdem. Returns null and fills OutError if the file is missing or malformed. */
	static TSharedPtr<const FMassifHeightmap> LoadFromFile(const FString& FilePath, FString& OutError);

	/** Parses the .massifdem byte layout. Separate from file IO so tests can exercise the format. */
	static TSharedPtr<const FMassifHeightmap> LoadFromBytes(TArrayView<const uint8> Bytes, FString& OutError);

	/** Builds a heightmap from raw altitudes. For tests and for generated stand-in terrain. */
	static TSharedRef<const FMassifHeightmap> FromAltitudes(
		TArrayView<const double> AltitudesMeters, int32 Width, int32 Height, double SampleSpacingMeters);

	/** Ground altitude in meters at a world position, filtered for the given sample spacing. */
	double SampleHeightMeters(FVector2d PositionMeters, double SampleSpacingMeters) const;

	double GetWorldSizeXMeters() const;
	double GetWorldSizeYMeters() const;

	double GetMinAltitudeMeters() const
	{
		return MinAltitudeMeters;
	}

	double GetMaxAltitudeMeters() const
	{
		return MaxAltitudeMeters;
	}

	double GetFinestSpacingMeters() const
	{
		return FinestSpacingMeters;
	}

	const FSourceInfo& GetSourceInfo() const
	{
		return SourceInfo;
	}

	int32 GetMipCount() const
	{
		return MipLevels.Num();
	}

private:
	/** One resolution of the same world box: coarser levels have fewer, wider samples. */
	struct FMipLevel
	{
		TArray<uint16> Samples;
		int32 Width = 0;
		int32 Height = 0;
		double SpacingXMeters = 0.0;
		double SpacingYMeters = 0.0;
	};

	static uint16 AverageQuad(const FMipLevel& Level, int32 Column, int32 Row);

	void BuildMipChain();
	void ComputeAltitudeRange();

	double SampleLevelMeters(const FMipLevel& Level, FVector2d PositionMeters) const;
	double SampleColumnRunMeters(const FMipLevel& Level, int32 Column, int32 Row, double ColumnFraction) const;
	double GetSampleMeters(const FMipLevel& Level, int32 Column, int32 Row) const;

	TArray<FMipLevel> MipLevels;
	FSourceInfo SourceInfo;
	double QuantisationBaseMeters = 0.0;
	double QuantisationStepMeters = 1.0;
	double FinestSpacingMeters = 1.0;
	double MinAltitudeMeters = 0.0;
	double MaxAltitudeMeters = 0.0;
};
