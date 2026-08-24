#include "SMassifEnvironmentSettings.h"

#include "MassifEnvironmentSettingsModel.h"
#include "MassifFidelityLabels.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	constexpr float NameColumnWidth = 220.0f;
	constexpr float StateColumnWidth = 150.0f;
	constexpr float LevelButtonWidth = 82.0f;

	const FSlateColor SelectedLevelColor(FLinearColor(0.16f, 0.44f, 0.72f));
	const FSlateColor StagedLevelColor(FLinearColor(0.72f, 0.52f, 0.14f));
	const FSlateColor PlainLevelColor(FLinearColor(0.10f, 0.10f, 0.11f));

	FText LevelText(EMassifFidelity Fidelity)
	{
		return FText::FromString(MassifFidelityLabels::Label(Fidelity));
	}
}

void SMassifEnvironmentSettings::Construct(const FArguments& InArgs)
{
	Model = InArgs._Model;
	OnClose = InArgs._OnClose;

	ChildSlot
	[
		SNew(SBorder)
		.Padding(16.0f)
		.BorderBackgroundColor(FLinearColor(0.02f, 0.02f, 0.03f, 0.94f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(STextBlock)
				.Text(NSLOCTEXT("Massif", "SettingsTitle", "Environment Simulation"))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				SNew(STextBlock)
				.Text(NSLOCTEXT("Massif", "SettingsSubtitle",
					"Each system is solved from the seed. Changing one re-solves everything downstream of it."))
				.ColorAndOpacity(FLinearColor(0.6f, 0.6f, 0.62f))
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					BuildRows()
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 12.0f, 0.0f, 0.0f)
			[
				BuildFooter()
			]
		]
	];
}

TSharedRef<SWidget> SMassifEnvironmentSettings::BuildRows()
{
	TSharedRef<SVerticalBox> Column = SNew(SVerticalBox);
	for (const FMassifFidelityRow& Row : Model->GetRows())
	{
		Column->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 2.0f)
			[
				BuildRow(Row.ProcessName)
			];
	}
	return Column;
}

TSharedRef<SWidget> SMassifEnvironmentSettings::BuildRow(FName ProcessName)
{
	TSharedRef<SHorizontalBox> RowBox = SNew(SHorizontalBox);
	RowBox->AddSlot()
		.AutoWidth()
		[
			SNew(SBox)
			.WidthOverride(NameColumnWidth)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(FText::FromString(Model->FindRow(ProcessName)->DisplayName))
			]
		];

	for (EMassifFidelity Fidelity : MassifFidelityLabels::AllLevels())
	{
		RowBox->AddSlot().AutoWidth().Padding(2.0f, 0.0f)[BuildLevelButton(ProcessName, Fidelity)];
	}

	RowBox->AddSlot()
		.AutoWidth()
		.Padding(12.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SBox)
			.WidthOverride(StateColumnWidth)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SMassifEnvironmentSettings::GetRowStateText, ProcessName)
				.ColorAndOpacity(FLinearColor(0.72f, 0.52f, 0.14f))
			]
		];
	return RowBox;
}

TSharedRef<SWidget> SMassifEnvironmentSettings::BuildLevelButton(
	FName ProcessName, EMassifFidelity Fidelity)
{
	// Unsupported levels stay on the row rather than being dropped from it, so the
	// ladders line up down the screen and a system that cannot go higher says so.
	return SNew(SBox)
		.WidthOverride(LevelButtonWidth)
		[
			SNew(SButton)
			.HAlign(HAlign_Center)
			.Text(LevelText(Fidelity))
			.IsEnabled(this, &SMassifEnvironmentSettings::IsLevelSupported, ProcessName, Fidelity)
			.ButtonColorAndOpacity(this, &SMassifEnvironmentSettings::GetLevelButtonColor, ProcessName, Fidelity)
			.OnClicked(this, &SMassifEnvironmentSettings::OnLevelClicked, ProcessName, Fidelity)
		];
}

TSharedRef<SWidget> SMassifEnvironmentSettings::BuildFooter()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			SNew(STextBlock)
			.Text(this, &SMassifEnvironmentSettings::GetAffectedText)
			.AutoWrapText(true)
			.ColorAndOpacity(FLinearColor(0.62f, 0.68f, 0.78f))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SMassifEnvironmentSettings::GetSolveText)
				.ColorAndOpacity(FLinearColor(0.6f, 0.6f, 0.62f))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(4.0f, 0.0f)
			[
				SNew(SButton)
				.Text(NSLOCTEXT("Massif", "SettingsApply", "Apply"))
				.IsEnabled(this, &SMassifEnvironmentSettings::HasChanges)
				.OnClicked(this, &SMassifEnvironmentSettings::OnApplyClicked)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(4.0f, 0.0f)
			[
				SNew(SButton)
				.Text(NSLOCTEXT("Massif", "SettingsRevert", "Revert"))
				.IsEnabled(this, &SMassifEnvironmentSettings::HasChanges)
				.OnClicked(this, &SMassifEnvironmentSettings::OnRevertClicked)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(4.0f, 0.0f)
			[
				SNew(SButton)
				.Text(NSLOCTEXT("Massif", "SettingsClose", "Close"))
				.OnClicked(this, &SMassifEnvironmentSettings::OnCloseClicked)
			]
		];
}

FText SMassifEnvironmentSettings::GetRowStateText(FName ProcessName) const
{
	const FMassifFidelityRow* Row = Model->FindRow(ProcessName);
	if (Row == nullptr || !Row->IsChanged())
	{
		return FText::GetEmpty();
	}
	return FText::FromString(FString::Printf(TEXT("%s -> %s"),
		MassifFidelityLabels::Label(Row->Current), MassifFidelityLabels::Label(Row->Staged)));
}

FText SMassifEnvironmentSettings::GetAffectedText() const
{
	const TArray<FString> Affected = Model->GetAffectedDisplayNames();
	if (Affected.IsEmpty())
	{
		return NSLOCTEXT("Massif", "SettingsNoChanges", "No changes staged.");
	}
	if (Model->GetApplyMode() == EMassifSettingsApplyMode::RecordForNextGeneration)
	{
		return NSLOCTEXT("Massif", "SettingsNextGeneration",
			"Applies to the next world generated.");
	}
	return FText::FromString(
		FString::Printf(TEXT("Applying re-solves: %s"), *FString::Join(Affected, TEXT(", "))));
}

FText SMassifEnvironmentSettings::GetSolveText() const
{
	if (!bHasApplied)
	{
		return FText::GetEmpty();
	}
	return FText::FromString(FString::Printf(TEXT("Last solve: %.2f s"), LastSolveSeconds));
}

bool SMassifEnvironmentSettings::IsLevelSupported(FName ProcessName, EMassifFidelity Fidelity) const
{
	const FMassifFidelityRow* Row = Model->FindRow(ProcessName);
	return Row != nullptr && Row->Supports(Fidelity);
}

FSlateColor SMassifEnvironmentSettings::GetLevelButtonColor(
	FName ProcessName, EMassifFidelity Fidelity) const
{
	const FMassifFidelityRow* Row = Model->FindRow(ProcessName);
	if (Row == nullptr || Row->Staged != Fidelity)
	{
		return PlainLevelColor;
	}
	if (Row->IsChanged())
	{
		return StagedLevelColor;
	}
	return SelectedLevelColor;
}

bool SMassifEnvironmentSettings::HasChanges() const
{
	return Model->HasChanges();
}

FReply SMassifEnvironmentSettings::OnLevelClicked(FName ProcessName, EMassifFidelity Fidelity)
{
	Model->Stage(ProcessName, Fidelity);
	return FReply::Handled();
}

FReply SMassifEnvironmentSettings::OnApplyClicked()
{
	LastSolveSeconds = Model->Apply();
	bHasApplied = true;
	return FReply::Handled();
}

FReply SMassifEnvironmentSettings::OnRevertClicked()
{
	Model->Revert();
	return FReply::Handled();
}

FReply SMassifEnvironmentSettings::OnCloseClicked()
{
	OnClose.ExecuteIfBound();
	return FReply::Handled();
}
