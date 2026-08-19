#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

/**
 * Dev overlay with sliders for the live worldgen knobs. Each slider writes its
 * massif.* console variable and nothing else; the game mode's cvar polling is
 * what rebuilds the terrain, so the panel and the console stay interchangeable.
 * Purely presentation: owns no state beyond the widgets.
 */
class SMassifTuningPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMassifTuningPanel)
	{
	}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	TSharedRef<SWidget> MakeKnobRow(const FText& Label, const TCHAR* ConsoleVariableName, float MinValue, float MaxValue);
	TSharedRef<SWidget> MakeRegenerateButton();
};
