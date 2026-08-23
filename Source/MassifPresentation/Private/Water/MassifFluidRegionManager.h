#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MassifFluidRegionManager.generated.h"

class FMassifTerrainField;
class UDynamicMeshComponent;
class UMaterialInterface;

/**
 * Draws the water one baked region settled into.
 *
 * The tiled river surface elsewhere draws a level per station, which across a
 * channel is a horizontal line and reads as a plate. This draws the skin of an
 * actual body of water: particles run through the region at bake time, and the
 * sheet their field crosses zero at is the mesh handed here.
 *
 * One region for now, on the largest river the world has. Covering a world means
 * many of these, which is a residency problem rather than a different solve.
 */
UCLASS()
class AMassifFluidRegionManager : public AActor
{
	GENERATED_BODY()

public:
	AMassifFluidRegionManager();

	/**
	 * Bakes the region around a point on a river and puts its surface in the world.
	 *
	 * Blocking: the solve runs here rather than on a worker, because nothing can be
	 * drawn until it finishes and the region is sized to be affordable.
	 */
	void Initialize(
		TSharedPtr<const FMassifTerrainField> InTerrainField,
		FVector2d CentreMeters,
		double WaterSurfaceAltitudeMeters,
		UMaterialInterface* InWaterMaterial);

private:
	UPROPERTY()
	TObjectPtr<UDynamicMeshComponent> SurfaceComponent;
};
