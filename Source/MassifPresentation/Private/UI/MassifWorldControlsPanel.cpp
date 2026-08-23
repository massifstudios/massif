#include "MassifWorldControlsPanel.h"

#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "SMassifWorldControls.h"
#include "Widgets/Layout/SBox.h"

namespace
{
	TSharedPtr<SWidget> GControlsWidget;
	TWeakObjectPtr<UGameViewportClient> GControlsViewport;

	/**
	 * Handle on world teardown, so the panel comes off before Slate does.
	 *
	 * This one is up for the whole session rather than while a key is held, so
	 * unlike the screens it would still be holding a widget when the viewport it
	 * was added to went away, and shutdown would stall on it.
	 */
	FDelegateHandle GControlsTeardownHandle;

	/** Clear of the channel legend, which the debug views draw in the same corner. */
	constexpr float ControlsTopMargin = 12.0f;
	constexpr float ControlsSideMargin = 12.0f;

	/** Under the settings screen, so opening that covers this rather than fighting it. */
	constexpr int32 ControlsPanelZOrder = 50;
}

bool MassifWorldControlsPanel::IsVisible()
{
	return GControlsWidget.IsValid();
}

void MassifWorldControlsPanel::Hide()
{
	if (!GControlsWidget.IsValid())
	{
		return;
	}

	if (GControlsViewport.IsValid())
	{
		GControlsViewport->RemoveViewportWidgetContent(GControlsWidget.ToSharedRef());
	}

	if (GControlsTeardownHandle.IsValid())
	{
		FWorldDelegates::OnWorldBeginTearDown.Remove(GControlsTeardownHandle);
		GControlsTeardownHandle.Reset();
	}

	GControlsWidget.Reset();
	GControlsViewport.Reset();
}

void MassifWorldControlsPanel::Show(UWorld* World)
{
	if (IsVisible() || World == nullptr || World->GetGameViewport() == nullptr)
	{
		return;
	}

	GControlsViewport = World->GetGameViewport();
	GControlsWidget = SNew(SBox)
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Top)
		.Padding(ControlsSideMargin, ControlsTopMargin, ControlsSideMargin, 0.0f)
		[
			SNew(SMassifWorldControls).World(World)
		];

	GControlsViewport->AddViewportWidgetContent(GControlsWidget.ToSharedRef(), ControlsPanelZOrder);
	GControlsTeardownHandle =
		FWorldDelegates::OnWorldBeginTearDown.AddLambda([](UWorld*) { MassifWorldControlsPanel::Hide(); });
}

void MassifWorldControlsPanel::Toggle(UWorld* World)
{
	if (IsVisible())
	{
		Hide();
		return;
	}
	Show(World);
}

namespace
{
	FAutoConsoleCommandWithWorld GWorldControlsCommand(
		TEXT("massif.Controls"),
		TEXT("Shows and hides the corner panel for the time of day and the fog."),
		FConsoleCommandWithWorldDelegate::CreateStatic(&MassifWorldControlsPanel::Toggle));
}
