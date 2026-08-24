#include "MassifEnvironmentSettingsScreen.h"

#include "Debug/MassifEnvironmentInspector.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "MassifElevationSource.h"
#include "MassifEnvironmentBakeParams.h"
#include "MassifEnvironmentGraph.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifEnvironmentSettings.h"
#include "MassifEnvironmentSettingsModel.h"
#include "SMassifEnvironmentSettings.h"
#include "Style/MassifWidgetStyles.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SOverlay.h"

namespace
{
	/** The screen, while it is up. One at a time: it edits the one scheduler the world has. */
	TSharedPtr<SWidget> GScreenWidget;
	TSharedPtr<FMassifEnvironmentSettingsModel> GModel;
	TWeakObjectPtr<UGameViewportClient> GViewport;

	/** Only set where there was no world to read, and dropped with the screen. */
	TSharedPtr<FMassifEnvironmentScheduler> GMenuScheduler;

	/**
	 * A graph with nothing solved in it, for the screen opened from the main menu.
	 *
	 * The rows are a property of the process graph — which systems exist, which
	 * levels each implements — and not of the ground underneath it, so an analytic
	 * world two kilometres across lists exactly what the real one would. Nothing is
	 * ever executed on it.
	 */
	TSharedPtr<FMassifEnvironmentScheduler> MakeMenuScheduler()
	{
		FString PrepareError;
		TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MassifPrepareEnvironment(
			MakeShared<const FMassifAnalyticElevationSource>(FMassifAnalyticElevationSource::FParams()),
			FMassifGeoreference(), 0, FMassifEnvironmentBakeParams(), PrepareError);
		if (!Scheduler.IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("Settings: %s"), *PrepareError);
			return nullptr;
		}

		MassifEnvironmentSettings::ApplySaved(*Scheduler);
		return MakeShareable(Scheduler.Release());
	}

	/**
	 * Takes the camera away from the screen while it is up, and gives it back after.
	 *
	 * The cursor is not touched: it is shown for the whole game now, and a screen
	 * that turned it off on the way out would take the pointer away from a building
	 * game that is played with one.
	 */
	void HoldCameraStill(UWorld* World, bool bHold)
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

	TSharedRef<SWidget> BuildScreen()
	{
		// The scrim is what makes this read as a screen rather than as a panel dropped
		// on top of one: over a world it dims the terrain, and over the menu it puts
		// the entries it covers plainly out of reach.
		return SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SImage).Image(MassifWidgetStyles::ScrimBrush())
			]
			+ SOverlay::Slot()
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(940.0f)
				.MaxDesiredHeight(760.0f)
				[
					SNew(SMassifEnvironmentSettings)
					.Model(GModel)
					.OnClose(FSimpleDelegate::CreateStatic(&MassifEnvironmentSettingsScreen::Close))
				]
			];
	}
}

bool MassifEnvironmentSettingsScreen::IsOpen()
{
	return GScreenWidget.IsValid();
}

void MassifEnvironmentSettingsScreen::Close()
{
	if (!GScreenWidget.IsValid())
	{
		return;
	}

	if (GViewport.IsValid())
	{
		GViewport->RemoveViewportWidgetContent(GScreenWidget.ToSharedRef());
		HoldCameraStill(GViewport->GetWorld(), false);
	}
	GScreenWidget.Reset();
	GModel.Reset();
	GMenuScheduler.Reset();
	GViewport.Reset();
}

void MassifEnvironmentSettingsScreen::Toggle(UWorld* World)
{
	if (IsOpen())
	{
		Close();
		return;
	}
	if (World == nullptr || World->GetGameViewport() == nullptr)
	{
		return;
	}

	// Over a solved world the screen edits that world. From the menu there is none
	// yet, so it edits what the next generation will be run at instead.
	FMassifEnvironmentScheduler* Scheduler = MassifEnvironmentInspector::FindScheduler(World);
	EMassifSettingsApplyMode ApplyMode = EMassifSettingsApplyMode::Resolve;
	if (Scheduler == nullptr)
	{
		GMenuScheduler = MakeMenuScheduler();
		if (!GMenuScheduler.IsValid())
		{
			return;
		}
		Scheduler = GMenuScheduler.Get();
		ApplyMode = EMassifSettingsApplyMode::RecordForNextGeneration;
	}

	GModel = MakeShared<FMassifEnvironmentSettingsModel>(*Scheduler, ApplyMode);
	GViewport = World->GetGameViewport();
	GScreenWidget = BuildScreen();
	// Over the main menu, under the loading screen.
	GViewport->AddViewportWidgetContent(GScreenWidget.ToSharedRef(), 250);
	HoldCameraStill(World, true);
}

namespace
{
	FAutoConsoleCommandWithWorld GEnvironmentSettingsCommand(
		TEXT("massif.Env.Settings"),
		TEXT("Open the environment simulation settings screen."),
		FConsoleCommandWithWorldDelegate::CreateStatic(&MassifEnvironmentSettingsScreen::Toggle));
}
