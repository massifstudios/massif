#include "MassifTerrainField.h"

#include "MassifHeightmap.h"
#include "MassifRiverField.h"

FMassifTerrainField::FMassifTerrainField(TSharedRef<const FMassifHeightmap> InHeightmap)
	: Heightmap(MoveTemp(InHeightmap))
{
}

FMassifTerrainField::FMassifTerrainField(
	TSharedRef<const FMassifHeightmap> InHeightmap,
	TSharedRef<const FMassifRiverField> InRiverField)
	: Heightmap(MoveTemp(InHeightmap))
	, RiverField(MoveTemp(InRiverField))
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

/**
 * The measured ground with the channels' own beds standing in it.
 *
 * The field does the blending, because it is the only thing that knows how far
 * from a centreline the solved bed is still the answer and where the hillside
 * takes over again. Everything this adds is the measurement to blend against.
 *
 * Under the water the result is the bed the carve worked and nothing of the
 * hillside, which is what a surface solved to sit a fixed depth over that bed
 * needs. Outside the sections it is the measurement untouched, and no width of
 * the blend depends on the sample spacing — so two tiles meeting at different
 * vertex spacings disagree along their shared edge by no more than the terrain
 * alone does.
 */
double FMassifTerrainField::SampleHeightMeters(FVector2d PositionMeters, double SampleSpacingMeters) const
{
	const double MeasuredMeters = Heightmap->SampleHeightMeters(PositionMeters, SampleSpacingMeters);
	if (!RiverField.IsValid())
	{
		return MeasuredMeters;
	}
	return RiverField->SampleChannelGroundMeters(PositionMeters, MeasuredMeters);
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
