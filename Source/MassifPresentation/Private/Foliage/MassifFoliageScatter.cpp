#include "MassifFoliageScatter.h"

#include "MassifFoliageSpecies.h"
#include "MassifSeed.h"
#include "MassifTerrainField.h"

namespace
{
	constexpr double SquareMetersPerHectare = 10000.0;

	/** Sampling spacing for the field: fine enough that a plant reads the ground it stands on. */
	constexpr double GroundSampleSpacingMeters = 1.0;

	/** A tile costs one field sample per candidate, so a misconfigured density cannot stall a frame. */
	constexpr int32 MaxCellsPerSide = 256;

	/** 0 outside the band, 1 well inside it, ramping over the fade at both ends. */
	double AltitudeWeight(double AltitudeMeters, const FMassifFoliageSpecies& Species)
	{
		const double RisingEdge = (AltitudeMeters - Species.MinAltitudeMeters) / Species.AltitudeFadeMeters;
		const double FallingEdge = (Species.MaxAltitudeMeters - AltitudeMeters) / Species.AltitudeFadeMeters;
		return FMath::Clamp(FMath::Min(RisingEdge, FallingEdge), 0.0, 1.0);
	}

	/** 1 on ground the species holds comfortably, falling to 0 at its steepest. */
	double SlopeWeight(double SlopeDegrees, const FMassifFoliageSpecies& Species)
	{
		const double Headroom = (Species.MaxSlopeDegrees - SlopeDegrees) / Species.SlopeFadeDegrees;
		return FMath::Clamp(Headroom, 0.0, 1.0);
	}

	int32 CellsPerSide(const FMassifFoliageSpecies& Species)
	{
		const double TileAreaHectares = Species.TileSizeMeters * Species.TileSizeMeters / SquareMetersPerHectare;
		const double CandidateCount = Species.DensityPerHectare * TileAreaHectares;
		return FMath::Clamp(FMath::CeilToInt(FMath::Sqrt(CandidateCount)), 1, MaxCellsPerSide);
	}

	/**
	 * One grid cell's candidate: jittered inside the cell, then accepted against
	 * the ground it landed on. Draws the same number of random values whether or
	 * not it is accepted, so the stream stays aligned across cells.
	 */
	bool TryPlaceInCell(
		const FMassifTerrainField& Field,
		const FMassifFoliageSpecies& Species,
		FRandomStream& Stream,
		FVector2d CellOriginMeters,
		double CellSizeMeters,
		MassifFoliageScatter::FInstance& OutInstance)
	{
		const FVector2d PositionMeters(
			CellOriginMeters.X + Stream.FRand() * CellSizeMeters,
			CellOriginMeters.Y + Stream.FRand() * CellSizeMeters);
		const double Acceptance = Stream.FRand();
		const float ScaleFraction = Stream.FRand();
		const float YawDegrees = Stream.FRand() * 360.0f;

		const double AltitudeMeters = Field.SampleHeightMeters(PositionMeters, GroundSampleSpacingMeters);
		const double Weight = AltitudeWeight(AltitudeMeters, Species);
		if (Weight <= 0.0 || Acceptance >= Weight)
		{
			return false;
		}

		const double SlopeDegrees = Field.SampleSlopeDegrees(PositionMeters, GroundSampleSpacingMeters);
		if (Acceptance >= Weight * SlopeWeight(SlopeDegrees, Species))
		{
			return false;
		}

		OutInstance.LocationMeters = FVector3d(PositionMeters.X, PositionMeters.Y, AltitudeMeters);
		OutInstance.YawDegrees = YawDegrees;
		OutInstance.Scale = FMath::Lerp(Species.MinScale, Species.MaxScale, ScaleFraction);
		return true;
	}
}

uint32 MassifFoliageScatter::MakeSpeciesSeed(uint32 MasterSeed, const TCHAR* SpeciesName)
{
	return FMassifSeed(MasterSeed).MakeChannelSeed(SpeciesName);
}

void MassifFoliageScatter::ScatterTile(
	const FMassifTerrainField& Field,
	const FMassifFoliageSpecies& Species,
	uint32 SpeciesSeed,
	int32 TileX,
	int32 TileY,
	TArray<FInstance>& OutInstances)
{
	OutInstances.Reset();

	const uint32 TileSeed = HashCombine(HashCombine(SpeciesSeed, GetTypeHash(TileX)), GetTypeHash(TileY));
	FRandomStream Stream(static_cast<int32>(TileSeed));

	const int32 SideCellCount = CellsPerSide(Species);
	const double CellSizeMeters = Species.TileSizeMeters / SideCellCount;
	const FVector2d TileOriginMeters(TileX * Species.TileSizeMeters, TileY * Species.TileSizeMeters);

	for (int32 CellIndex = 0; CellIndex < SideCellCount * SideCellCount; ++CellIndex)
	{
		const FVector2d CellOriginMeters = TileOriginMeters + FVector2d(
			(CellIndex % SideCellCount) * CellSizeMeters,
			(CellIndex / SideCellCount) * CellSizeMeters);

		FInstance Instance;
		if (TryPlaceInCell(Field, Species, Stream, CellOriginMeters, CellSizeMeters, Instance))
		{
			OutInstances.Add(Instance);
		}
	}
}
