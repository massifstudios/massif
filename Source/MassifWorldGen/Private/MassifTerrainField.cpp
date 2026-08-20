#include "MassifTerrainField.h"

#include "MassifHeightmap.h"
#include "MassifRiverField.h"

namespace
{
	/**
	 * How far past the waterline the channel's cross-section would reach zero
	 * depth. A shore that ran exactly dry at the water's edge would leave the mesh
	 * drawn over it coincident with the ground along that whole line; a few
	 * centimetres of water there separates them and gives the shoreline something
	 * to fade over.
	 */
	constexpr double ShoreOvershoot = 1.06;

	/** 1 at the bank, falling to 0 at the far edge of it. */
	double BankWeight(double SignedDistanceMeters, double BankMeters)
	{
		const double Fraction = FMath::Clamp(SignedDistanceMeters / BankMeters, 0.0, 1.0);
		return 1.0 - Fraction * Fraction * (3.0 - 2.0 * Fraction);
	}

	/**
	 * The bed across the channel: the whole solved column midstream, shallowing to
	 * the water's edge. A flat-bottomed trench would stand the full column against
	 * a wall at the waterline, which draws as a hard line down each side of every
	 * river in the world rather than as a shore.
	 */
	double ChannelBedMeters(const FMassifRiverSample& River, double AcrossFraction)
	{
		const double ColumnMeters = River.WaterSurfaceMeters - River.BedAltitudeMeters;
		const double Shape = FMath::Max(1.0 - AcrossFraction * AcrossFraction, 0.0);
		return River.WaterSurfaceMeters - ColumnMeters * Shape;
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
	if (River.SignedDistanceMeters <= 0.0)
	{
		return FMath::Min(MeasuredMeters, ChannelBedMeters(River, River.AcrossFraction / ShoreOvershoot));
	}
	return FMath::Min(MeasuredMeters, BankAltitudeMeters(MeasuredMeters, River, SampleSpacingMeters));
}

/**
 * Ground outside the water: the shore the channel's own cross-section arrives at,
 * rising to the measured surface across the bank. The bank widens with the sample
 * spacing rather than the cut switching off below it, so a coarse tile gets a
 * filtered version of the same shape and not a different one.
 */
double FMassifTerrainField::BankAltitudeMeters(
	double MeasuredMeters, const FMassifRiverSample& River, double SampleSpacingMeters) const
{
	const double BankMeters = FMath::Clamp(
		FMath::Max(River.WidthMeters * CarveParams.BankWidthFraction, SampleSpacingMeters),
		CarveParams.MinBankMeters,
		CarveParams.MaxBankMeters);

	const double Weight = BankWeight(River.SignedDistanceMeters, BankMeters);
	if (Weight <= 0.0)
	{
		return MeasuredMeters;
	}

	// The shore the inside of the channel ends at, so the two meet at the
	// waterline instead of stepping there.
	const double ShoreMeters = ChannelBedMeters(River, 1.0 / ShoreOvershoot);
	return FMath::Lerp(MeasuredMeters, ShoreMeters, Weight);
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
