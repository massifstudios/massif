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
 * The measured ground, less the trench the channels cut into it.
 *
 * A subtraction rather than a level the ground is pulled down to. The trench is
 * the shape the carve worked across each station's own cross-section — a pool
 * against the outer bank, a bar against the inner one, the bed between them —
 * and taking it off whatever the model measured lands that shape on the real
 * hillside instead of replacing the hillside with a fitted one.
 *
 * Nothing here reads the sample spacing, which is what makes the surface a
 * function of position alone: two tiles meeting at different vertex spacings get
 * the same altitude along the edge they share rather than two, and the coarser
 * of them is a filtered version of the same cut.
 */
double FMassifTerrainField::SampleHeightMeters(FVector2d PositionMeters, double SampleSpacingMeters) const
{
	const double MeasuredMeters = Heightmap->SampleHeightMeters(PositionMeters, SampleSpacingMeters);
	if (!RiverField.IsValid())
	{
		return MeasuredMeters;
	}
	return MeasuredMeters - RiverField->SampleCutDepthMeters(PositionMeters);
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
