#include "MassifMainMenuScreen.h"

#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "MassifEnvironmentSettingsScreen.h"
#include "MassifGameMode.h"
#include "MassifMenuBackdrop.h"
#include "MassifWorldSave.h"
#include "SMassifMainMenu.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/SOverlay.h"

namespace
{
	TSharedPtr<SWidget> GMenuWidget;
	TWeakObjectPtr<UGameViewportClient> GMenuViewport;
	TSharedPtr<FMassifMenuBackdrop> GMenuBackdrop;

	/** A menu that let the fly camera keep turning underneath it would be unusable. */
	void HoldMenuCameraStill(UWorld* World, bool bHold)
	{
		APlayerController* Controller = nullptr;
		if (World != nullptr)
		{
			Controller = World->GetFirstPlayerController();
		}
		if (Controller == nullptr)
		{
			return;
		}
		Controller->SetIgnoreLookInput(bHold);
		Controller->SetIgnoreMoveInput(bHold);
	}

	/** The name as a save records it, which is the cvar's own spelling. */
	FString GetMenuElevationModelName()
	{
		const IConsoleVariable* DemVariable = IConsoleManager::Get().FindConsoleVariable(TEXT("massif.Dem"));
		if (DemVariable == nullptr)
		{
			return FString();
		}
		return DemVariable->GetString();
	}

	FText GetElevationModelLabel()
	{
		return FText::FromString(GetMenuElevationModelName().ToUpper());
	}

	void StartNewGame(TWeakObjectPtr<UWorld> World)
	{
		// The settings screen may be up over the menu, and the graph it was editing is
		// about to be replaced by the one the generation builds.
		MassifEnvironmentSettingsScreen::Close();
		MassifMainMenuScreen::Close();
		if (!World.IsValid())
		{
			return;
		}

		AMassifGameMode* GameMode = World->GetAuthGameMode<AMassifGameMode>();
		if (GameMode == nullptr)
		{
			return;
		}
		GameMode->StartNewGame();
	}

	void LoadSave(FString SaveName, TWeakObjectPtr<UWorld> World)
	{
		MassifEnvironmentSettingsScreen::Close();
		MassifMainMenuScreen::Close();
		if (!World.IsValid())
		{
			return;
		}

		AMassifGameMode* GameMode = World->GetAuthGameMode<AMassifGameMode>();
		if (GameMode == nullptr)
		{
			return;
		}
		GameMode->LoadGame(SaveName);
	}

	FString DescribeSave(const FMassifWorldSaveHeader& Header)
	{
		return FString::Printf(TEXT("Seed %d  ·  %s  ·  %s"),
			Header.MasterSeed, *Header.ElevationModelName,
			*Header.SavedAtUtc.ToString(TEXT("%d %b %Y  %H:%M")));
	}

	/**
	 * The saves on disk, as much as can be told about each without reading its body.
	 *
	 * The model's name is checked here because the header carries it; its hash is not,
	 * because answering that would mean reading two hundred megabytes of elevation
	 * model to grey out a row. The load itself checks both and refuses there.
	 */
	TArray<FMassifSaveListItem> GatherSaves()
	{
		const FString CurrentModel = GetMenuElevationModelName();

		TArray<FMassifSaveListItem> Items;
		for (const FString& SaveName : MassifListWorldSaves())
		{
			FMassifWorldSaveHeader Header;
			FString HeaderError;
			if (!MassifReadWorldSaveHeader(MassifWorldSavePath(SaveName), Header, HeaderError))
			{
				Items.Add({ SaveName, FString(), false, HeaderError });
				continue;
			}

			FMassifSaveListItem Item;
			Item.Name = SaveName;
			Item.Detail = DescribeSave(Header);
			Item.bLoadable = Header.ElevationModelName == CurrentModel;
			Item.Problem = FString::Printf(
				TEXT("Built on '%s'; this run is on '%s'."), *Header.ElevationModelName, *CurrentModel);
			Items.Add(MoveTemp(Item));
		}
		return Items;
	}

	void OpenSettings(TWeakObjectPtr<UWorld> World)
	{
		if (!World.IsValid())
		{
			return;
		}
		MassifEnvironmentSettingsScreen::Toggle(World.Get());
	}

	void Quit(TWeakObjectPtr<UWorld> World)
	{
		if (!World.IsValid() || World->GetFirstPlayerController() == nullptr)
		{
			return;
		}
		World->GetFirstPlayerController()->ConsoleCommand(TEXT("quit"));
	}

	TSharedRef<SWidget> BuildScreen(UWorld* World)
	{
		const TWeakObjectPtr<UWorld> WeakWorld(World);
		GMenuBackdrop = MakeShared<FMassifMenuBackdrop>();

		return SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SImage).Image(GMenuBackdrop->GetBrush())
			]
			+ SOverlay::Slot()
			[
				SNew(SMassifMainMenu)
				.ElevationModelName(GetElevationModelLabel())
				.Saves(GatherSaves())
				.OnNewGame(FSimpleDelegate::CreateStatic(&StartNewGame, WeakWorld))
				.OnLoadSave(FMassifOnLoadSave::CreateStatic(&LoadSave, WeakWorld))
				.OnSettings(FSimpleDelegate::CreateStatic(&OpenSettings, WeakWorld))
				.OnQuit(FSimpleDelegate::CreateStatic(&Quit, WeakWorld))
			];
	}

	/** One retry, a tick later. A second failure is a headless run with no viewport at all. */
	void OpenOnceViewportExists(TWeakObjectPtr<UWorld> World)
	{
		if (!World.IsValid() || World->GetGameViewport() == nullptr)
		{
			UE_LOG(LogTemp, Warning, TEXT("Main menu: no game viewport to open on."));
			return;
		}
		MassifMainMenuScreen::Open(World.Get());
	}
}

bool MassifMainMenuScreen::IsOpen()
{
	return GMenuWidget.IsValid();
}

void MassifMainMenuScreen::Close()
{
	if (!GMenuWidget.IsValid())
	{
		return;
	}

	if (GMenuViewport.IsValid())
	{
		GMenuViewport->RemoveViewportWidgetContent(GMenuWidget.ToSharedRef());
		HoldMenuCameraStill(GMenuViewport->GetWorld(), false);
	}
	GMenuWidget.Reset();
	GMenuViewport.Reset();
	GMenuBackdrop.Reset();
}

void MassifMainMenuScreen::Open(UWorld* World)
{
	if (IsOpen() || World == nullptr)
	{
		return;
	}

	// BeginPlay can land before the viewport client exists, and a menu that quietly
	// did not open leaves a run with no way into it at all.
	if (World->GetGameViewport() == nullptr)
	{
		World->GetTimerManager().SetTimerForNextTick(
			FTimerDelegate::CreateStatic(&OpenOnceViewportExists, TWeakObjectPtr<UWorld>(World)));
		return;
	}

	GMenuViewport = World->GetGameViewport();
	GMenuWidget = BuildScreen(World);
	GMenuViewport->AddViewportWidgetContent(GMenuWidget.ToSharedRef(), 200);
	HoldMenuCameraStill(World, true);
}

namespace
{
	FAutoConsoleCommandWithWorld GMainMenuCommand(
		TEXT("massif.Menu"),
		TEXT("Open the main menu."),
		FConsoleCommandWithWorldDelegate::CreateStatic(&MassifMainMenuScreen::Open));
}
