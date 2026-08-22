#include "MassifDebugChannelView.h"

#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "MassifDebugVectorOverlay.h"
#include "MassifEnvironmentState.h"
#include "MassifHabitat.h"
#include "Terrain/MassifTerrainManager.h"

namespace
{
	TAutoConsoleVariable<int32> CVarMassifDebugChannel(
		TEXT("massif.DebugChannel"), 0,
		TEXT("Paints a generated view over the terrain, as the number keys do. 0 is off, and the rest ")
		TEXT("index the list massif.Env.Views prints."));

	/** The index the view list holds a view at, offset past Off. */
	int32 ViewIndexOfChannel(EMassifDebugChannel Channel)
	{
		return static_cast<int32>(Channel) - 1;
	}

	EMassifDebugChannel ChannelOfViewIndex(int32 ViewIndex)
	{
		return static_cast<EMassifDebugChannel>(ViewIndex + 1);
	}
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
	EnsureViews();
}

AMassifDebugChannelView* AMassifDebugChannelView::Find(const UObject* WorldContext)
{
	return Cast<AMassifDebugChannelView>(
		UGameplayStatics::GetActorOfClass(WorldContext, AMassifDebugChannelView::StaticClass()));
}

void AMassifDebugChannelView::RequestChannel(EMassifDebugChannel Channel)
{
	CVarMassifDebugChannel->Set(static_cast<int32>(Channel), ECVF_SetByConsole);
}

void AMassifDebugChannelView::EnsureViews()
{
	if (!Views.IsEmpty() || !Habitat.IsValid())
	{
		return;
	}
	Views = MassifDebugChannels::BuildViews(Habitat->GetEnvironment());
}

void AMassifDebugChannelView::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Habitat.IsValid())
	{
		return;
	}

	EnsureViews();
	const int32 Requested = FMath::Clamp(CVarMassifDebugChannel.GetValueOnGameThread(), 0, Views.Num());
	if (Requested != static_cast<int32>(CurrentChannel))
	{
		ApplyChannel(static_cast<EMassifDebugChannel>(Requested));
	}

	MassifDebugVectorOverlay::Draw(GetWorld(), *Habitat, Views, ViewIndexOfChannel(CurrentChannel));
}

void AMassifDebugChannelView::ApplyChannel(EMassifDebugChannel Channel)
{
	if (TerrainManager == nullptr || !Habitat.IsValid())
	{
		return;
	}

	CurrentChannel = Channel;
	if (!Views.IsValidIndex(ViewIndexOfChannel(Channel)))
	{
		Legend = FMassifDebugChannelLegend();
		TerrainManager->ClearDebugChannel();
		UE_LOG(LogTemp, Display, TEXT("Debug view: off"));
		return;
	}

	const FMassifDebugView& View = Views[ViewIndexOfChannel(Channel)];
	const double StartSeconds = FPlatformTime::Seconds();
	MassifDebugChannels::RenderChannel(*Habitat, View, ChannelTexture, Legend);
	TerrainManager->SetDebugChannel(ChannelTexture);

	UE_LOG(LogTemp, Display, TEXT("Debug view %d: %s, %.3f to %.3f %s, rendered in %.2f s"),
		static_cast<int32>(Channel), *Legend.Name, Legend.MinValue, Legend.MaxValue, *Legend.Unit,
		FPlatformTime::Seconds() - StartSeconds);
}

namespace
{
	const TArray<FMassifDebugView>* FindViews(UWorld* World)
	{
		const AMassifDebugChannelView* ViewActor = AMassifDebugChannelView::Find(World);
		if (ViewActor == nullptr || ViewActor->GetViews().IsEmpty())
		{
			return nullptr;
		}
		return &ViewActor->GetViews();
	}

	void ListViews(const TArray<FMassifDebugView>& Views)
	{
		UE_LOG(LogTemp, Display, TEXT("  0  off"));
		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
		{
			UE_LOG(LogTemp, Display, TEXT("  %d  %-34s %s"),
				ViewIndex + 1, *Views[ViewIndex].Name.ToString(), *Views[ViewIndex].Unit);
		}
	}

	void HandleViewsCommand(const TArray<FString>&, UWorld* World)
	{
		const TArray<FMassifDebugView>* Views = FindViews(World);
		if (Views == nullptr)
		{
			UE_LOG(LogTemp, Warning, TEXT("massif.Env.Views: no environment has been solved yet"));
			return;
		}
		ListViews(*Views);
	}

	void HandleViewCommand(const TArray<FString>& Arguments, UWorld* World)
	{
		const TArray<FMassifDebugView>* Views = FindViews(World);
		if (Views == nullptr || Arguments.IsEmpty())
		{
			UE_LOG(LogTemp, Display, TEXT("massif.Env.View <ChannelName|off>"));
			return;
		}
		if (Arguments[0].Equals(TEXT("off"), ESearchCase::IgnoreCase))
		{
			AMassifDebugChannelView::RequestChannel(EMassifDebugChannel::Off);
			return;
		}

		const int32 ViewIndex = MassifDebugChannels::FindViewIndex(*Views, FName(*Arguments[0]));
		if (ViewIndex == INDEX_NONE)
		{
			UE_LOG(LogTemp, Warning, TEXT("massif.Env.View: nothing is registered as %s"), *Arguments[0]);
			ListViews(*Views);
			return;
		}
		AMassifDebugChannelView::RequestChannel(ChannelOfViewIndex(ViewIndex));
	}

	FAutoConsoleCommandWithWorldAndArgs GEnvironmentViewCommand(
		TEXT("massif.Env.View"),
		TEXT("Paint one registered channel, or one habitat view, over the terrain by name."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&HandleViewCommand));

	FAutoConsoleCommandWithWorldAndArgs GEnvironmentViewsCommand(
		TEXT("massif.Env.Views"),
		TEXT("List every view the channel registry offers, with the index the number keys reach it by."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&HandleViewsCommand));
}
