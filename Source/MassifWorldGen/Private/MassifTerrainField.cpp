#include "MassifTerrainField.h"

#include "MassifHeightmap.h"

FMassifTerrainField::FMassifTerrainField(TSharedRef<const FMassifHeightmap> InHeightmap)
	: Heightmap(MoveTemp(InHeightmap))
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
	return Heightmap->SampleHeightMeters(PositionMeters, SampleSpacingMeters);
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
