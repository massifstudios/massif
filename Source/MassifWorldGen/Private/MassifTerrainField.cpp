#include "MassifTerrainField.h"

#include "MassifHeightmap.h"
#include "MassifRiverField.h"

namespace
{
	/** 1 in the water, falling to 0 at the far edge of the bank, flat where it meets both. */
	double BankWeight(double SignedDistanceMeters, double BankMeters)
	{
		if (SignedDistanceMeters <= 0.0)
		{
			return 1.0;
		}
		if (SignedDistanceMeters >= BankMeters)
		{
			return 0.0;
		}

		const double Fraction = SignedDistanceMeters / BankMeters;
		return 1.0 - Fraction * Fraction * (3.0 - 2.0 * Fraction);
	}
}

FMassifTerrainField::FMassifTerrainField(TSharedRef<const FMassifHeightmap> InHeightmap)
	: Heightmap(MoveTemp(InHeightmap))
{
}

FMassifTerrainField::FMassifTerrainField(
	TSharedRef<const FMassifHeightmap> InHeightmap,
	TSharedRef<const FMassifRiverField> InRiverField,
	const FMassifRiverCarveParams& InCarveParams)
	: Heightmap(MoveTemp(InHeightmap))
	, RiverField(MoveTemp(InRiverField))
	, CarveParams(InCarveParams)
{
}

double FMassifTerrainField::GetWorldSizeMeters() const
{
	return FMath::Min(Heightmap->GetWorldSizeXMeters(), Heightmap->GetWorldSizeYMeters());
}

double FMassifTerrainField::GetMinAltitudeMeters() const
{
	return Heightmap->GetMinAltitudeMeters();
}

double FMassifTerrainField::GetMaxAltitudeMeters() const
{
	return Heightmap->GetMaxAltitudeMeters();
}

double FMassifTerrainField::SampleHeightMeters(FVector2d PositionMeters, double SampleSpacingMeters) const
{
	const double MeasuredMeters = Heightmap->SampleHeightMeters(PositionMeters, SampleSpacingMeters);
	if (!RiverField.IsValid())
	{
		return MeasuredMeters;
	}
	return CarveChannel(MeasuredMeters, PositionMeters, SampleSpacingMeters);
}

double FMassifTerrainField::CarveChannel(
	double MeasuredMeters, FVector2d PositionMeters, double SampleSpacingMeters) const
{
	const FMassifRiverSample River = RiverField->Sample(PositionMeters);
	const double BankMeters = FMath::Clamp(
		FMath::Max(River.WidthMeters * CarveParams.BankWidthFraction, SampleSpacingMeters),
		CarveParams.MinBankMeters,
		CarveParams.MaxBankMeters);

	const double Weight = BankWeight(River.SignedDistanceMeters, BankMeters);
	if (Weight <= 0.0)
	{
		return MeasuredMeters;
	}

	const double CarvedMeters = FMath::Lerp(MeasuredMeters, River.BedAltitudeMeters, Weight);
	return FMath::Min(MeasuredMeters, CarvedMeters);
}

FVector3d FMassifTerrainField::SampleNormal(FVector2d PositionMeters, double SampleSpacingMeters) const
{
	// Differenced at the caller's spacing, so the normal describes the surface
	// the caller is about to build rather than one finer than its own vertices.
	const double Step = FMath::Max(SampleSpacingMeters, UE_DOUBLE_SMALL_NUMBER);
	const double West = SampleHeightMeters(PositionMeters - FVector2d(Step, 0.0), SampleSpacingMeters);
	const double East = SampleHeightMeters(PositionMeters + FVector2d(Step, 0.0), SampleSpacingMeters);
	const double South = SampleHeightMeters(PositionMeters - FVector2d(0.0, Step), SampleSpacingMeters);
	const double North = SampleHeightMeters(PositionMeters + FVector2d(0.0, Step), SampleSpacingMeters);

	const FVector3d Normal(West - East, South - North, 2.0 * Step);
	return Normal.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector3d::UpVector);
}

double FMassifTerrainField::SampleSlopeDegrees(FVector2d PositionMeters, double SampleSpacingMeters) const
{
	const double NormalZ = FMath::Clamp(SampleNormal(PositionMeters, SampleSpacingMeters).Z, -1.0, 1.0);
	return FMath::RadiansToDegrees(FMath::Acos(NormalZ));
}
