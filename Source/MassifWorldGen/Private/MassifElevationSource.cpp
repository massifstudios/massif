#include "MassifElevationSource.h"

#include "MassifHeightmap.h"

namespace
{
	/** A position resolved into the shape's own frame: along the bearing, across it, and out from the centre. */
	struct FShapeCoordinates
	{
		double AlongMeters = 0.0;
		double AcrossMeters = 0.0;
		double RadiusMeters = 0.0;
	};

	FVector2d ClampToWorldBox(FVector2d PositionMeters, double HalfSizeMeters)
	{
		return FVector2d(
			FMath::Clamp(PositionMeters.X, -HalfSizeMeters, HalfSizeMeters),
			FMath::Clamp(PositionMeters.Y, -HalfSizeMeters, HalfSizeMeters));
	}

	FShapeCoordinates MakeShapeCoordinates(FVector2d PositionMeters, double BearingDegrees)
	{
		const double BearingRadians = FMath::DegreesToRadians(BearingDegrees);
		const FVector2d AlongBearing(FMath::Sin(BearingRadians), FMath::Cos(BearingRadians));
		const FVector2d AcrossBearing(AlongBearing.Y, -AlongBearing.X);

		FShapeCoordinates Coordinates;
		Coordinates.AlongMeters = FVector2d::DotProduct(PositionMeters, AlongBearing);
		Coordinates.AcrossMeters = FVector2d::DotProduct(PositionMeters, AcrossBearing);
		Coordinates.RadiusMeters = PositionMeters.Length();
		return Coordinates;
	}

	/** One at the centre, falling linearly to zero at the world edge and staying there beyond it. */
	double LinearFalloff(double DistanceMeters, double HalfSizeMeters)
	{
		return FMath::Max(0.0, 1.0 - DistanceMeters / HalfSizeMeters);
	}

	/**
	 * Zero to one over the box: peaks at the two ends of the bearing axis, lows
	 * at the two ends of the across axis, and the pass at the world centre,
	 * halfway up, where flow has to pick a side.
	 */
	double SaddleFraction(const FShapeCoordinates& Coordinates, double HalfSizeMeters)
	{
		const double AlongPhase = UE_DOUBLE_PI * Coordinates.AlongMeters / HalfSizeMeters;
		const double AcrossPhase = UE_DOUBLE_PI * Coordinates.AcrossMeters / HalfSizeMeters;
		return 0.5 + 0.25 * (FMath::Cos(AcrossPhase) - FMath::Cos(AlongPhase));
	}
}

FMassifHeightmapElevationSource::FMassifHeightmapElevationSource(TSharedRef<const FMassifHeightmap> InHeightmap)
	: Heightmap(MoveTemp(InHeightmap))
{
}

double FMassifHeightmapElevationSource::SampleAltitudeMeters(FVector2d PositionMeters, double SampleSpacingMeters) const
{
	return Heightmap->SampleHeightMeters(PositionMeters, SampleSpacingMeters);
}

FVector2d FMassifHeightmapElevationSource::GetWorldSizeMeters() const
{
	return FVector2d(Heightmap->GetWorldSizeXMeters(), Heightmap->GetWorldSizeYMeters());
}

double FMassifHeightmapElevationSource::GetFinestSpacingMeters() const
{
	return Heightmap->GetFinestSpacingMeters();
}

FMassifAnalyticElevationSource::FMassifAnalyticElevationSource(const FParams& InParams)
	: Params(InParams)
{
	check(Params.WorldSizeMeters > 0.0);
	check(Params.FinestSpacingMeters > 0.0);
}

double FMassifAnalyticElevationSource::SampleAltitudeMeters(FVector2d PositionMeters, double SampleSpacingMeters) const
{
	// The spacing carries no information here: every shape is exact at every
	// scale, so there is nothing left for a coarser sampling to filter away.
	const double HalfSizeMeters = Params.WorldSizeMeters * 0.5;
	const FVector2d BoxPositionMeters = ClampToWorldBox(PositionMeters, HalfSizeMeters);
	const FShapeCoordinates Coordinates = MakeShapeCoordinates(BoxPositionMeters, Params.BearingDegrees);

	switch (Params.Shape)
	{
		case EMassifAnalyticTerrain::Flat:
			return Params.BaseAltitudeMeters;

		case EMassifAnalyticTerrain::TiltedPlane:
			// Descends towards the bearing: the relief sits on the edge the bearing
			// points away from, the base altitude on the edge it points to.
			return Params.BaseAltitudeMeters
				+ Params.ReliefMeters * (0.5 - Coordinates.AlongMeters / (2.0 * HalfSizeMeters));

		case EMassifAnalyticTerrain::Cone:
			return Params.BaseAltitudeMeters
				+ Params.ReliefMeters * LinearFalloff(Coordinates.RadiusMeters, HalfSizeMeters);

		case EMassifAnalyticTerrain::Ridge:
			// The crest runs along the bearing through the world centre.
			return Params.BaseAltitudeMeters
				+ Params.ReliefMeters * LinearFalloff(FMath::Abs(Coordinates.AcrossMeters), HalfSizeMeters);

		case EMassifAnalyticTerrain::Pit:
			return Params.BaseAltitudeMeters
				- Params.ReliefMeters * LinearFalloff(Coordinates.RadiusMeters, HalfSizeMeters);

		case EMassifAnalyticTerrain::Saddle:
			return Params.BaseAltitudeMeters + Params.ReliefMeters * SaddleFraction(Coordinates, HalfSizeMeters);
	}

	return Params.BaseAltitudeMeters;
}

FVector2d FMassifAnalyticElevationSource::GetWorldSizeMeters() const
{
	return FVector2d(Params.WorldSizeMeters, Params.WorldSizeMeters);
}

double FMassifAnalyticElevationSource::GetFinestSpacingMeters() const
{
	return Params.FinestSpacingMeters;
}

const FMassifAnalyticElevationSource::FParams& FMassifAnalyticElevationSource::GetParams() const
{
	return Params;
}
