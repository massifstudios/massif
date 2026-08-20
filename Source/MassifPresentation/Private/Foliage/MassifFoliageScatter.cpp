#include "MassifFoliageScatter.h"

#include "MassifFoliageSpecies.h"
#include "MassifHabitat.h"
#include "MassifSeed.h"

namespace
{
	constexpr double SquareMetersPerHectare = 10000.0;

	/** Sampling spacing for the field: fine enough that a plant reads the ground it stands on. */
	constexpr double GroundSampleSpacingMeters = 1.0;

	/** A tile costs one habitat sample per candidate, so a misconfigured density cannot stall a frame. */
	constexpr int32 MaxCellsPerSide = 256;

	/** 0 outside the band, 1 well inside it, ramping over the fade at both ends. */
	double BandWeight(double Value, double Minimum, double Maximum, double Fade)
	{
		const double RisingEdge = (Value - Minimum) / Fade;
		const double FallingEdge = (Maximum - Value) / Fade;
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
	 * the habitat it landed in. Draws the same number of random values whether or
	 * not it is accepted, so the stream stays aligned across cells.
	 */
	bool TryPlaceInCell(
		const FMassifHabitatField& Habitat,
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

		const FMassifHabitatSample Ground = Habitat.Sample(PositionMeters, GroundSampleSpacingMeters);
		if (Acceptance >= MassifFoliageScatter::HabitatWeight(Species, Ground))
		{
			return false;
		}

		OutInstance.LocationMeters = FVector3d(PositionMeters.X, PositionMeters.Y, Ground.AltitudeMeters);
		OutInstance.YawDegrees = YawDegrees;
		OutInstance.Scale = FMath::Lerp(Species.MinScale, Species.MaxScale, ScaleFraction);
		return true;
	}
}

double MassifFoliageScatter::HabitatWeight(const FMassifFoliageSpecies& Species, const FMassifHabitatSample& Habitat)
{
	if ((Species.GroundTypeMask & MassifGroundTypeBit(Habitat.GroundType)) == 0)
	{
		return 0.0;
	}

	const double Temperature = BandWeight(
		Habitat.TemperatureCelsius,
		Species.MinTemperatureCelsius,
		Species.MaxTemperatureCelsius,
		Species.TemperatureFadeCelsius);
	const double Humidity = BandWeight(
		Habitat.Humidity, Species.MinHumidity, Species.MaxHumidity, Species.HumidityFade);
	return Temperature * Humidity * SlopeWeight(Habitat.SlopeDegrees, Species);
}

uint32 MassifFoliageScatter::MakeSpeciesSeed(uint32 MasterSeed, const TCHAR* SpeciesName)
{
	return FMassifSeed(MasterSeed).MakeChannelSeed(SpeciesName);
}

void MassifFoliageScatter::ScatterTile(
	const FMassifHabitatField& Habitat,
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
		if (TryPlaceInCell(Habitat, Species, Stream, CellOriginMeters, CellSizeMeters, Instance))
		{
			OutInstances.Add(Instance);
		}
	}
}
