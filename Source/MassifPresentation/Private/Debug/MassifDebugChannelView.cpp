#include "MassifDebugChannelView.h"

#include "MassifHabitat.h"
#include "Terrain/MassifTerrainManager.h"

namespace
{
	TAutoConsoleVariable<int32> CVarMassifDebugChannel(
		TEXT("massif.DebugChannel"), 0,
		TEXT("Paints a generated channel over the terrain, as the number keys do. ")
		TEXT("0 off, 1 standing water, 2 flow accumulation, 3 wetness, 4 soil depth, ")
		TEXT("5 ground type, 6 humidity, 7 temperature, 8 altitude, 9 slope."));
}

AMassifDebugChannelView::AMassifDebugChannelView()
{
	PrimaryActorTick.bCanEverTick = true;
}

void AMassifDebugChannelView::Initialize(
	TSharedPtr<const FMassifHabitatField> InHabitat, AMassifTerrainManager* InTerrainManager)
{
	Habitat = MoveTemp(InHabitat);
	TerrainManager = InTerrainManager;
}

void AMassifDebugChannelView::RequestChannel(EMassifDebugChannel Channel)
{
	CVarMassifDebugChannel->Set(static_cast<int32>(Channel), ECVF_SetByConsole);
}

void AMassifDebugChannelView::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	const int32 RequestedIndex = FMath::Clamp(
		CVarMassifDebugChannel.GetValueOnGameThread(), 0, static_cast<int32>(EMassifDebugChannel::Count) - 1);
	const EMassifDebugChannel Requested = static_cast<EMassifDebugChannel>(RequestedIndex);
	if (Requested != CurrentChannel)
	{
		ApplyChannel(Requested);
	}
}

void AMassifDebugChannelView::ApplyChannel(EMassifDebugChannel Channel)
{
	if (!Habitat.IsValid() || TerrainManager == nullptr)
	{
		return;
	}

	CurrentChannel = Channel;
	if (Channel == EMassifDebugChannel::Off)
	{
		Legend = FMassifDebugChannelLegend();
		TerrainManager->ClearDebugChannel();
		UE_LOG(LogTemp, Display, TEXT("Debug view: off"));
		return;
	}

	const double StartSeconds = FPlatformTime::Seconds();
	MassifDebugChannels::RenderChannel(*Habitat, Channel, ChannelTexture, Legend);
	TerrainManager->SetDebugChannel(ChannelTexture);

	UE_LOG(LogTemp, Display, TEXT("Debug view %d: %s, %.3f to %.3f %s, rendered in %.2f s"),
		static_cast<int32>(Channel), *Legend.Name, Legend.MinValue, Legend.MaxValue, *Legend.Unit,
		FPlatformTime::Seconds() - StartSeconds);
}
