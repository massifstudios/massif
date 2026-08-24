#include "MassifLoadingScreen.h"

#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "MassifMenuBackdrop.h"
#include "MassifWorldBuild.h"
#include "SMassifLoadingScreen.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/SOverlay.h"

namespace
{
	TSharedPtr<SWidget> GLoadingWidget;
	TWeakObjectPtr<UGameViewportClient> GLoadingViewport;
	TSharedPtr<FMassifMenuBackdrop> GLoadingBackdrop;

	TSharedRef<SWidget> BuildScreen(TSharedRef<FMassifWorldBuildProgress> Progress)
	{
		GLoadingBackdrop = MakeShared<FMassifMenuBackdrop>();

		return SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SImage).Image(GLoadingBackdrop->GetBrush())
			]
			+ SOverlay::Slot()
			[
				SNew(SMassifLoadingScreen).Progress(Progress)
			];
	}
}

bool MassifLoadingScreen::IsOpen()
{
	return GLoadingWidget.IsValid();
}

void MassifLoadingScreen::Close()
{
	if (!GLoadingWidget.IsValid())
	{
		return;
	}

	if (GLoadingViewport.IsValid())
	{
		GLoadingViewport->RemoveViewportWidgetContent(GLoadingWidget.ToSharedRef());
	}
	GLoadingWidget.Reset();
	GLoadingViewport.Reset();
	GLoadingBackdrop.Reset();
}

void MassifLoadingScreen::Open(UWorld* World, TSharedRef<FMassifWorldBuildProgress> Progress)
{
	if (IsOpen() || World == nullptr || World->GetGameViewport() == nullptr)
	{
		return;
	}

	GLoadingViewport = World->GetGameViewport();
	GLoadingWidget = BuildScreen(Progress);

	// Above the menu's layer, so nothing of a screen being torn down shows through.
	GLoadingViewport->AddViewportWidgetContent(GLoadingWidget.ToSharedRef(), 300);
}
