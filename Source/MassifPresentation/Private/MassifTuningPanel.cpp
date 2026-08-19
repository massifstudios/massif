#include "MassifTuningPanel.h"

#include "HAL/IConsoleManager.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

void SMassifTuningPanel::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SBorder)
		.BorderBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.7f))
		.Padding(12.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(STextBlock).Text(NSLOCTEXT("Massif", "TuningTitle", "Terrain tuning (F1 to close)"))
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				MakeKnobRow(NSLOCTEXT("Massif", "GullyDepth", "Erosion strength (gully depth m)"), TEXT("massif.Gully.Depth"), 0.0f, 800.0f)
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				MakeKnobRow(NSLOCTEXT("Massif", "GullyWavelength", "Gully wavelength (m)"), TEXT("massif.Gully.Wavelength"), 100.0f, 5000.0f)
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				MakeKnobRow(NSLOCTEXT("Massif", "GullyBranch", "Gully branch weight"), TEXT("massif.Gully.BranchStrength"), 0.0f, 2.0f)
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				MakeKnobRow(NSLOCTEXT("Massif", "GullyRidge", "Ridge rounding"), TEXT("massif.Gully.RidgeRounding"), 0.0f, 1.0f)
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				MakeKnobRow(NSLOCTEXT("Massif", "GullyCrease", "Crease rounding"), TEXT("massif.Gully.CreaseRounding"), 0.0f, 1.0f)
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				MakeKnobRow(NSLOCTEXT("Massif", "GullySlopeRef", "Full-depth slope"), TEXT("massif.Gully.SlopeReference"), 0.05f, 1.0f)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				MakeRegenerateButton()
			]
		]
	];
}

TSharedRef<SWidget> SMassifTuningPanel::MakeKnobRow(
	const FText& Label, const TCHAR* ConsoleVariableName, float MinValue, float MaxValue)
{
	IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(ConsoleVariableName);
	check(Variable != nullptr);

	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(0.0f, 2.0f)
		[
			SNew(STextBlock).Text(Label)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 2.0f, 0.0f, 2.0f)
		[
			SNew(SBox).WidthOverride(140.0f)
			[
				SNew(SSpinBox<float>)
				.MinValue(MinValue)
				.MaxValue(MaxValue)
				.MinSliderValue(MinValue)
				.MaxSliderValue(MaxValue)
				.Value_Lambda([Variable]()
				{
					return Variable->GetFloat();
				})
				.OnValueChanged_Lambda([Variable](float NewValue)
				{
					Variable->Set(NewValue, ECVF_SetByConsole);
				})
			]
		];
}

TSharedRef<SWidget> SMassifTuningPanel::MakeRegenerateButton()
{
	return SNew(SButton)
		.HAlign(HAlign_Center)
		.Text(NSLOCTEXT("Massif", "Regenerate", "Regenerate world"))
		.OnClicked_Lambda([]()
		{
			IConsoleManager::Get().FindConsoleObject(TEXT("massif.Regenerate"))->AsCommand()->Execute(
				TArray<FString>(), nullptr, *GLog);
			return FReply::Handled();
		});
}
