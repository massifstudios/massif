#include "MassifRiverManager.h"

#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "MassifEnvironmentBake.h"
#include "MassifRiverNetwork.h"
#include "MassifRiverRibbonMesh.h"
#include "MassifUnits.h"

using MassifUnits::MetersToCentimeters;

namespace
{
	/** Half the key space each axis gets, so a signed block coordinate packs unsigned. */
	constexpr int64 BlockCoordinateBias = 1 << 20;
}

AMassifRiverManager::AMassifRiverManager()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

uint64 AMassifRiverManager::MakeBlockKey(FVector2d PositionMeters)
{
	const int64 BlockX = FMath::FloorToInt64(PositionMeters.X / BlockSizeMeters) + BlockCoordinateBias;
	const int64 BlockY = FMath::FloorToInt64(PositionMeters.Y / BlockSizeMeters) + BlockCoordinateBias;
	return (static_cast<uint64>(BlockX) << 32) | (static_cast<uint64>(BlockY) & 0xFFFFFFFF);
}

FVector2d AMassifRiverManager::BlockOriginMeters(uint64 BlockKey)
{
	const int64 BlockX = static_cast<int64>(BlockKey >> 32) - BlockCoordinateBias;
	const int64 BlockY = static_cast<int64>(BlockKey & 0xFFFFFFFF) - BlockCoordinateBias;
	return FVector2d(BlockX * BlockSizeMeters, BlockY * BlockSizeMeters);
}

void AMassifRiverManager::Initialize(
	TSharedPtr<const FMassifEnvironmentBake> InBake, UMaterialInterface* InRiverMaterial)
{
	Bake = InBake;
	RiverMaterial = InRiverMaterial;
	if (!Bake.IsValid())
	{
		return;
	}

	const double StartSeconds = FPlatformTime::Seconds();
	for (const TPair<uint64, TArray<const FMassifRiverReach*>>& Block : GroupReachesIntoBlocks())
	{
		BuildBlock(Block.Key, Block.Value);
	}

	UE_LOG(LogTemp, Display, TEXT("Rivers: %d water surfaces built in %.2f s"),
		BlockComponents.Num(), FPlatformTime::Seconds() - StartSeconds);
}

TMap<uint64, TArray<const FMassifRiverReach*>> AMassifRiverManager::GroupReachesIntoBlocks() const
{
	TMap<uint64, TArray<const FMassifRiverReach*>> Blocks;
	for (const FMassifRiverReach& Reach : Bake->GetRivers().GetReaches())
	{
		const uint64 BlockKey = MakeBlockKey(Reach.Nodes[0].PositionMeters);
		Blocks.FindOrAdd(BlockKey).Add(&Reach);
	}
	return Blocks;
}

void AMassifRiverManager::BuildBlock(uint64 BlockKey, TArrayView<const FMassifRiverReach* const> Reaches)
{
	const FVector2d OriginMeters = BlockOriginMeters(BlockKey);
	TSharedPtr<UE::Geometry::FDynamicMesh3> Mesh =
		MassifRiverRibbonMesh::BuildRibbonMesh(Reaches, OriginMeters);
	if (Mesh->TriangleCount() == 0)
	{
		return;
	}

	UDynamicMeshComponent* Component = NewObject<UDynamicMeshComponent>(this);
	Component->SetupAttachment(RootComponent);
	Component->RegisterComponent();
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// The surface is a single sheet with nothing under it, and it is drawn
	// translucent, so it neither casts nor receives anything worth the cost.
	Component->SetCastShadow(false);
	Component->SetMaterial(0, RiverMaterial);
	Component->SetWorldLocation(FVector(OriginMeters.X, OriginMeters.Y, 0.0) * MetersToCentimeters);
	Component->SetMesh(MoveTemp(*Mesh));

	BlockComponents.Add(Component);
}
