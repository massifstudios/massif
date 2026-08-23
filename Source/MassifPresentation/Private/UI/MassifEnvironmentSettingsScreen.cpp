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

	void ShowMouse(UWorld* World, bool bShow)
	{
		APlayerController* Controller = World != nullptr ? World->GetFirstPlayerController() : nullptr;
		if (Controller == nullptr)
		{
			return;
		}
		Controller->bShowMouseCursor = bShow;
		Controller->SetIgnoreLookInput(bShow);
		Controller->SetIgnoreMoveInput(bShow);
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
		ShowMouse(GViewport->GetWorld(), false);
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
	ShowMouse(World, true);
}

namespace
{
	FAutoConsoleCommandWithWorld GEnvironmentSettingsCommand(
		TEXT("massif.Env.Settings"),
		TEXT("Open the environment simulation settings screen."),
		FConsoleCommandWithWorldDelegate::CreateStatic(&MassifEnvironmentSettingsScreen::Toggle));
}
