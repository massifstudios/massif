#pragma once

#include "CoreMinimal.h"

class FMassifHeightmap;

/**
 * Everything the environment layer is allowed to know about the ground it runs
 * over.
 *
 * Nothing in that layer knows what terrain it stands on: altitude by position
 * and sample spacing, the extent of the world box, and the spacing below which
 * there is nothing more to resolve. A process that reached past this interface
 * for the imported elevation model would stop working the moment a different
 * model was slotted in, and fails against the analytic sources the automation
 * suite runs on.
 *
 * Sampling is a pure function of position: it is called from many threads and
 * must never depend on call order or on state that a query mutates.
 */
class MASSIFWORLDGEN_API IMassifElevationSource
{
public:
	virtual ~IMassifElevationSource() = default;

	/** Ground altitude in metres. Must be a pure function of position: called from many threads. */
	virtual double SampleAltitudeMeters(FVector2d PositionMeters, double SampleSpacingMeters) const = 0;

	/** Extent of the world box in metres, origin at its centre, X east, Y north. */
	virtual FVector2d GetWorldSizeMeters() const = 0;

	/** Spacing below which sampling resolves nothing further. */
	virtual double GetFinestSpacingMeters() const = 0;
};

/**
 * Where on Earth the world sits. The one input not derivable from a shape, and
 * the reason sun position and datum temperature are exempt from map
 * independence: they feed the sun's position and strength and the zero point of
 * the temperature field, and nothing else.
 */
struct FMassifGeoreference
{
	double LatitudeDegrees = 47.2;
	double LongitudeDegrees = 11.4;
	int32 SourceEpsgCode = 31254;

	/**
	 * Growing-season mean air temperature reduced to the datum: the temperature
	 * field's zero point, and through it where the treeline lands.
	 *
	 * The growing-season mean rather than the annual one, because what decides
	 * whether a tree can stand somewhere is the season it has to grow in, not the
	 * average of that season and the winter it sleeps through.
	 */
	double SeaLevelMeanTemperatureCelsius = 19.0;
};

/** The imported elevation model as an elevation source. */
class MASSIFWORLDGEN_API FMassifHeightmapElevationSource final : public IMassifElevationSource
{
public:
	explicit FMassifHeightmapElevationSource(TSharedRef<const FMassifHeightmap> InHeightmap);

	double SampleAltitudeMeters(FVector2d PositionMeters, double SampleSpacingMeters) const override;
	FVector2d GetWorldSizeMeters() const override;
	double GetFinestSpacingMeters() const override;

private:
	TSharedRef<const FMassifHeightmap> Heightmap;
};

/** Closed-form shapes the environment solves can be tested against without a DEM on disk. */
enum class EMassifAnalyticTerrain : uint8
{
	/** Dead flat at the base altitude. */
	Flat,

	/** Constant gradient, descending towards the bearing. */
	TiltedPlane,

	/** Radially symmetric peak at the world centre. Drainage should leave it in every direction. */
	Cone,

	/** A straight ridge across the world, so windward and shaded flanks can be compared. */
	Ridge,

	/** A single closed depression. Nothing drains out of it. */
	Pit,

	/** A saddle between two peaks, for flow that has to choose. */
	Saddle,
};

/**
 * Analytic terrain: a square world box whose altitude is a closed form of
 * position, exact at every sample spacing and solvable in milliseconds.
 *
 * This is what the automation suite runs against. A process that secretly read
 * the heightmap fails here, because there is no heightmap to read.
 */
class MASSIFWORLDGEN_API FMassifAnalyticElevationSource final : public IMassifElevationSource
{
public:
	struct FParams
	{
		EMassifAnalyticTerrain Shape = EMassifAnalyticTerrain::Cone;
		double WorldSizeMeters = 2000.0;
		double BaseAltitudeMeters = 1000.0;

		/** Altitude between the lowest and highest point of the shape. */
		double ReliefMeters = 500.0;

		/** Compass bearing the shape is oriented along: 0 north, 90 east. */
		double BearingDegrees = 0.0;

		double FinestSpacingMeters = 1.0;
	};

	explicit FMassifAnalyticElevationSource(const FParams& InParams);

	double SampleAltitudeMeters(FVector2d PositionMeters, double SampleSpacingMeters) const override;
	FVector2d GetWorldSizeMeters() const override;
	double GetFinestSpacingMeters() const override;

	const FParams& GetParams() const;

private:
	FParams Params;
};
