#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "MassifWaterBodyManager.generated.h"

class AMassifRiverBody;
class AWaterZone;
class FMassifRiverNetwork;
struct FMassifRiverReach;

/**
 * The half-month of the solved year the rivers are drawn at, from
 * massif.Water.DayOfYear.
 *
 * A console variable rather than simulation state: the year was solved at world
 * generation and this chooses which of it to look at.
 */
int32 MassifDrawnWaterTimeSlice();

/**
 * The river network drawn as water bodies: one spline per river, inside a water
 * zone.
 *
 * One per river, not one per reach. The network cuts a reach at every
 * confluence, so what a map would call one river is a chain of them; the chains
 * are strung back together here by following the water through each junction,
 * and each chain becomes one body with one spline and one surface.
 *
 * The zone renders the bodies and the ground under them into its water info
 * texture, and tessellates the water surface off that. Which is the whole point
 * of it: the surface knows how far the bed is below it at every point, so the
 * colour grades with the column and the waterline goes where the water actually
 * runs out — rather than a sheet cut to a shape and shaded flat.
 *
 * Built after world generation from the network's own stations. Position and
 * water surface make the spline; the channel's width, depth and speed at the
 * half-month being drawn are the spline metadata the surface is generated from.
 *
 * Presentation only. The network was solved at world gen against the seed and
 * nothing here changes it — this chooses which half-month of it to look at, the
 * way the fog rows choose how the fog is drawn.
 */
UCLASS()
class AMassifWaterBodyManager : public AActor
{
	GENERATED_BODY()

public:
	AMassifWaterBodyManager();

	void Initialize(const FMassifRiverNetwork& Rivers, FVector2d ZoneCentreMeters, int32 TimeSlice);

private:
	void SpawnWaterZone(FVector2d CentreMeters);
	AMassifRiverBody* SpawnRiverBody(const FMassifRiverReach& River, int32 TimeSlice);
	void LogWaterBodySummary(int32 TracedCount) const;
	void LogZoneOwnership() const;

	UPROPERTY()
	TObjectPtr<AWaterZone> WaterZone;

	UPROPERTY()
	TArray<TObjectPtr<AMassifRiverBody>> RiverBodies;

	/** What the zone covers, which is also what decides which reaches are drawn in it. */
	FBox2d ZoneMeters;

	int32 SplinePointCount = 0;
	double DrawnLengthMeters = 0.0;
};
