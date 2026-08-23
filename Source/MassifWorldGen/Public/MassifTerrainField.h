#pragma once

#include "CoreMinimal.h"

class FMassifHeightmap;
class FMassifRiverField;

/**
 * The terrain, as a function of world position:
 *
 *     Height(Position, SampleSpacing)
 *
 * The surface comes from a measured elevation model (§5): a real massif,
 * imported once and sampled here. Later layers — geological refinement and
 * sub-meter procedural detail — are added on top of it behind this same API,
 * so callers keep sampling one continuous function and never see the source.
 *
 * SampleSpacing is the distance between the samples the caller intends to
 * take. Detail finer than that spacing is filtered out rather than switched
 * off, so a coarse sampling and a fine sampling of the same point agree
 * instead of stepping where they meet.
 *
 * Positions are doubles throughout. A float coordinate at the world edge
 * resolves only about a millimeter, which would spend the fine-detail budget
 * on coordinate error before any terrain was evaluated.
 *
 * Thread-safe once constructed: sampling touches only immutable state.
 */
class MASSIFWORLDGEN_API FMassifTerrainField
{
public:
	explicit FMassifTerrainField(TSharedRef<const FMassifHeightmap> InHeightmap);

	/**
	 * The same surface with the rivers cut into it. The river field is a layer
	 * over the elevation model, never under it: the environment bake that traced
	 * those rivers reads the model directly, so the carve cannot feed back into
	 * the solve that produced it.
	 */
	FMassifTerrainField(
		TSharedRef<const FMassifHeightmap> InHeightmap,
		TSharedRef<const FMassifRiverField> InRiverField);

	FMassifTerrainField(const FMassifTerrainField&) = delete;
	FMassifTerrainField& operator=(const FMassifTerrainField&) = delete;

	/** Ground altitude in meters. */
	double SampleHeightMeters(FVector2d PositionMeters, double SampleSpacingMeters) const;

	/** Upward surface normal, differenced at the sample spacing. */
	FVector3d SampleNormal(FVector2d PositionMeters, double SampleSpacingMeters) const;

	double SampleSlopeDegrees(FVector2d PositionMeters, double SampleSpacingMeters) const;

	/** Side of the square the terrain covers, origin at its centre. */
	double GetWorldSizeMeters() const;

	double GetMinAltitudeMeters() const;
	double GetMaxAltitudeMeters() const;

	const FMassifHeightmap& GetHeightmap() const
	{
		return Heightmap.Get();
	}

	/** For anything that outlives the call, such as an elevation source built over it. */
	TSharedRef<const FMassifHeightmap> GetHeightmapShared() const
	{
		return Heightmap;
	}

	/** Null on a field built without rivers, which is what the worldgen tests and the bake use. */
	const FMassifRiverField* GetRiverField() const
	{
		return RiverField.Get();
	}

private:
	TSharedRef<const FMassifHeightmap> Heightmap;
	TSharedPtr<const FMassifRiverField> RiverField;
};
