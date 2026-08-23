#include "MassifEnvironmentSettingsScreen.h"

#include "Debug/MassifEnvironmentInspector.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "MassifEnvironmentSettingsModel.h"
#include "SMassifEnvironmentSettings.h"
#include "Widgets/Layout/SBox.h"

namespace
{
	/** The screen, while it is up. One at a time: it edits the one scheduler the world has. */
	TSharedPtr<SWidget> GScreenWidget;
	TSharedPtr<FMassifEnvironmentSettingsModel> GModel;
	TWeakObjectPtr<UGameViewportClient> GViewport;

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
		return SNew(SBox)
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
	GViewport.Reset();
}

void MassifEnvironmentSettingsScreen::Toggle(UWorld* World)
{
	if (IsOpen())
	{
		Close();
		return;
	}

	FMassifEnvironmentScheduler* Scheduler = MassifEnvironmentInspector::FindScheduler(World);
	if (Scheduler == nullptr || World->GetGameViewport() == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("massif.Env.Settings: no environment has been solved yet"));
		return;
	}

	GModel = MakeShared<FMassifEnvironmentSettingsModel>(*Scheduler);
	GViewport = World->GetGameViewport();
	GScreenWidget = BuildScreen();
	GViewport->AddViewportWidgetContent(GScreenWidget.ToSharedRef(), 100);
	HoldCameraStill(World, true);
}

namespace
{
	FAutoConsoleCommandWithWorld GEnvironmentSettingsCommand(
		TEXT("massif.Env.Settings"),
		TEXT("Open the environment simulation settings screen."),
		FConsoleCommandWithWorldDelegate::CreateStatic(&MassifEnvironmentSettingsScreen::Toggle));
}
