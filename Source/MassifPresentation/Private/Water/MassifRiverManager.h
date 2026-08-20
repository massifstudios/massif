#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MassifRiverManager.generated.h"

class FMassifEnvironmentBake;
class UDynamicMeshComponent;
class UMaterialInterface;

/**
 * Draws the river network as water surfaces: one ribbon per reach, batched into
 * square blocks of world so the engine can cull them, each block its own
 * component.
 *
 * The reaches never move and the world's water does not change, so the meshes
 * are built once at start-up rather than streamed. Three hundred kilometres of
 * channel is only tens of thousands of triangles once the polylines have been
 * simplified — far less than a single terrain tile.
 *
 * Purely presentation: owns no gameplay state, and reads the bake without
 * writing to it.
 */
UCLASS()
class AMassifRiverManager : public AActor
{
	GENERATED_BODY()

public:
	AMassifRiverManager();

	void Initialize(TSharedPtr<const FMassifEnvironmentBake> InBake, UMaterialInterface* InRiverMaterial);

private:
	/**
	 * Side of the block each component covers. Small enough that culling a block
	 * is worth something, large enough that a 20 km world does not need hundreds
	 * of components to hold a few hundred triangles each.
	 */
	static constexpr double BlockSizeMeters = 2000.0;

	/** Blocks are keyed by their grid coordinate, packed so the map key is one integer. */
	static uint64 MakeBlockKey(FVector2d PositionMeters);
	static FVector2d BlockOriginMeters(uint64 BlockKey);

	/** A reach belongs to the block its head is in, wherever the rest of it runs. */
	TMap<uint64, TArray<const struct FMassifRiverReach*>> GroupReachesIntoBlocks() const;

	void BuildBlock(uint64 BlockKey, TArrayView<const struct FMassifRiverReach* const> Reaches);

	TSharedPtr<const FMassifEnvironmentBake> Bake;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> RiverMaterial;

	UPROPERTY()
	TArray<TObjectPtr<UDynamicMeshComponent>> BlockComponents;
};
