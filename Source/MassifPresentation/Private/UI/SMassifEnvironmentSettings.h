#pragma once

#include "CoreMinimal.h"
#include "MassifEnvironmentProcess.h"
#include "Widgets/SCompoundWidget.h"

class FMassifEnvironmentSettingsModel;
class SVerticalBox;

/**
 * The environment settings screen: one row per system, one button per level.
 *
 * Holds no state of its own. Everything it shows is read back out of the model
 * every frame through attributes, so an Apply that rebuilds the rows cannot
 * leave a button drawn against a row that no longer exists.
 */
class SMassifEnvironmentSettings : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMassifEnvironmentSettings) {}
		SLATE_ARGUMENT(TSharedPtr<FMassifEnvironmentSettingsModel>, Model)
		SLATE_EVENT(FSimpleDelegate, OnClose)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	TSharedRef<SWidget> BuildRows();
	TSharedRef<SWidget> BuildRow(FName ProcessName);
	TSharedRef<SWidget> BuildLevelButton(FName ProcessName, EMassifFidelity Fidelity);
	TSharedRef<SWidget> BuildFooter();

	/** "High -> Medium" where the row is staged to move, and the level alone where it is not. */
	FText GetRowStateText(FName ProcessName) const;
	FText GetAffectedText() const;
	FText GetSolveText() const;

	FSlateColor GetLevelButtonColor(FName ProcessName, EMassifFidelity Fidelity) const;
	bool IsLevelSupported(FName ProcessName, EMassifFidelity Fidelity) const;
	bool HasChanges() const;

	FReply OnLevelClicked(FName ProcessName, EMassifFidelity Fidelity);
	FReply OnApplyClicked();
	FReply OnRevertClicked();
	FReply OnCloseClicked();

	TSharedPtr<FMassifEnvironmentSettingsModel> Model;
	FSimpleDelegate OnClose;

	/** How long the last Apply took, so the cost of a level is visible after paying it. */
	double LastSolveSeconds = 0.0;
	bool bHasApplied = false;
};
