#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MassifShallowWaterRegion.generated.h"

class FMassifRiverField;
class FMassifTerrainField;
class UDynamicMeshComponent;
class UMaterialInterface;

/**
 * Draws the water one solved patch of river settled into.
 *
 * The tiled river surface elsewhere draws a level per station, which across a
 * channel is a horizontal line and reads as a plate. This draws what the water
 * over that same bed actually does: shallow water solved over the carved
 * terrain, seeded from the channel solve's own surface and held at it around the
 * patch's edge, so the reach delivers water at one end and takes it away at the
 * other while the middle finds its own answer.
 *
 * One patch for now, on the largest river the world has. Covering a world means
 * many of them, which is a residency problem rather than a different solve.
 */
UCLASS()
class AMassifShallowWaterRegion : public AActor
{
	GENERATED_BODY()

public:
	AMassifShallowWaterRegion();

	/**
	 * Solves the patch around a point on a river and puts its surface in the world.
	 *
	 * Blocking: the solve runs here rather than on a worker, because nothing can be
	 * drawn until it finishes and the patch is sized to be affordable.
	 */
	void Initialize(
		TSharedPtr<const FMassifTerrainField> InTerrainField,
		TSharedPtr<const FMassifRiverField> InRiverField,
		FVector2d CentreMeters,
		UMaterialInterface* InWaterMaterial);

private:
	UPROPERTY()
	TObjectPtr<UDynamicMeshComponent> SurfaceComponent;
};
