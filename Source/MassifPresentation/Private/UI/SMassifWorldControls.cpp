#include "SMassifWorldControls.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "MassifSimSubsystem.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	constexpr float ControlsPanelWidth = 300.0f;
	constexpr float ControlsLabelWidth = 96.0f;
	constexpr float ControlsValueWidth = 52.0f;
	constexpr float ControlsRowPadding = 3.0f;

	const FLinearColor ControlsPanelBackground(0.02f, 0.03f, 0.05f, 0.82f);
	const FLinearColor ControlsSectionColour(0.55f, 0.72f, 0.95f, 1.0f);

	/** Days before the first of each month, so a day number reads back as a date. */
	constexpr int32 DaysBeforeMonth[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };

	const TCHAR* MonthNames[12] = {
		TEXT("Jan"), TEXT("Feb"), TEXT("Mar"), TEXT("Apr"), TEXT("May"), TEXT("Jun"),
		TEXT("Jul"), TEXT("Aug"), TEXT("Sep"), TEXT("Oct"), TEXT("Nov"), TEXT("Dec") };

	UMassifSimSubsystem* FindControlsSim(const TWeakObjectPtr<UWorld>& World)
	{
		if (!World.IsValid())
		{
			return nullptr;
		}

		UGameInstance* GameInstance = World->GetGameInstance();
		if (GameInstance == nullptr)
		{
			return nullptr;
		}

		return GameInstance->GetSubsystem<UMassifSimSubsystem>();
	}
}

float SMassifWorldControls::GetVariableValue(const TCHAR* VariableName)
{
	const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(VariableName);
	if (Variable == nullptr)
	{
		return 0.0f;
	}
	return Variable->GetFloat();
}

void SMassifWorldControls::SetVariableValue(const TCHAR* VariableName, float Value)
{
	IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(VariableName);
	if (Variable == nullptr)
	{
		return;
	}
	Variable->Set(Value, ECVF_SetByConsole);
}

float SMassifWorldControls::GetHourOfDay() const
{
	const UMassifSimSubsystem* Sim = FindControlsSim(World);
	if (Sim == nullptr)
	{
		return 0.0f;
	}
	return static_cast<float>(Sim->GetWallClock().DayFraction * 24.0);
}

void SMassifWorldControls::SetHourOfDay(float Hour)
{
	UMassifSimSubsystem* Sim = FindControlsSim(World);
	if (Sim == nullptr)
	{
		return;
	}
	Sim->SetTimeOfDayMinutes(FMath::RoundToInt32(Hour * 60.0f));
}

FText SMassifWorldControls::GetClockText() const
{
	const UMassifSimSubsystem* Sim = FindControlsSim(World);
	if (Sim == nullptr)
	{
		return FText::FromString(TEXT("--:--"));
	}

	const FMassifWallClock Clock = Sim->GetWallClock();
	return FText::FromString(FString::Printf(TEXT("%02d:%02d"), Clock.GetHour(), Clock.GetMinute()));
}

float SMassifWorldControls::GetDayOfYear()
{
	const IConsoleVariable* Variable =
		IConsoleManager::Get().FindConsoleVariable(TEXT("massif.Water.DayOfYear"));
	if (Variable == nullptr)
	{
		return 1.0f;
	}
	return static_cast<float>(Variable->GetInt());
}

void SMassifWorldControls::SetDayOfYear(float Day)
{
	IConsoleVariable* Variable =
		IConsoleManager::Get().FindConsoleVariable(TEXT("massif.Water.DayOfYear"));
	if (Variable == nullptr)
	{
		return;
	}
	Variable->Set(FMath::RoundToInt32(Day), ECVF_SetByConsole);
}

FText SMassifWorldControls::GetDateText()
{
	const int32 DayOfYear = FMath::Clamp(FMath::RoundToInt32(GetDayOfYear()), 1, 365);
	int32 MonthIndex = 11;
	while (MonthIndex > 0 && DaysBeforeMonth[MonthIndex] >= DayOfYear)
	{
		MonthIndex -= 1;
	}
	return FText::FromString(
		FString::Printf(TEXT("%d %s"), DayOfYear - DaysBeforeMonth[MonthIndex], MonthNames[MonthIndex]));
}

TSharedRef<SWidget> SMassifWorldControls::MakeDayOfYearRow()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SBox)
			.WidthOverride(ControlsLabelWidth)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Day of year")))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
			]
		]
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(SSlider)
			.MinValue(1.0f)
			.MaxValue(365.0f)
			.Value_Static(&SMassifWorldControls::GetDayOfYear)
			.OnValueChanged_Static(&SMassifWorldControls::SetDayOfYear)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SBox)
			.WidthOverride(ControlsValueWidth)
			[
				SNew(STextBlock)
				.Text_Static(&SMassifWorldControls::GetDateText)
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
			]
		];
}

TSharedRef<SWidget> SMassifWorldControls::MakeSectionLabel(const TCHAR* Label) const
{
	return SNew(STextBlock)
		.Text(FText::FromString(Label))
		.ColorAndOpacity(FSlateColor(ControlsSectionColour))
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9));
}

TSharedRef<SWidget> SMassifWorldControls::MakeTimeRow()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SBox)
			.WidthOverride(ControlsLabelWidth)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Time of day")))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
			]
		]
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(SSlider)
			.MinValue(0.0f)
			.MaxValue(24.0f)
			.Value(this, &SMassifWorldControls::GetHourOfDay)
			.OnValueChanged(this, &SMassifWorldControls::SetHourOfDay)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SBox)
			.WidthOverride(ControlsValueWidth)
			[
				SNew(STextBlock)
				.Text(this, &SMassifWorldControls::GetClockText)
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
			]
		];
}

TSharedRef<SWidget> SMassifWorldControls::MakeVariableRow(
	const TCHAR* Label, const TCHAR* VariableName, float Minimum, float Maximum)
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SBox)
			.WidthOverride(ControlsLabelWidth)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Label))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
			]
		]
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(SSlider)
			.MinValue(Minimum)
			.MaxValue(Maximum)
			.Value_Lambda([VariableName]() { return GetVariableValue(VariableName); })
			.OnValueChanged_Lambda([VariableName](float Value) { SetVariableValue(VariableName, Value); })
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SBox)
			.WidthOverride(ControlsValueWidth)
			[
				SNew(STextBlock)
				.Text_Lambda([VariableName]()
				{
					return FText::FromString(FString::Printf(TEXT("%.2f"), GetVariableValue(VariableName)));
				})
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
			]
		];
}

void SMassifWorldControls::Construct(const FArguments& InArgs)
{
	World = InArgs._World;

	ChildSlot
	[
		SNew(SBox)
		.WidthOverride(ControlsPanelWidth)
		[
			SNew(SBorder)
			.BorderBackgroundColor(FSlateColor(ControlsPanelBackground))
			.Padding(10.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, ControlsRowPadding)
				[
					MakeSectionLabel(TEXT("WORLD"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, ControlsRowPadding)
				[
					MakeTimeRow()
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, ControlsRowPadding)
				[
					MakeDayOfYearRow()
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, ControlsRowPadding)
				[
					MakeSectionLabel(TEXT("FOG"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, ControlsRowPadding)
				[
					MakeVariableRow(TEXT("Volumes"), TEXT("massif.Fog.Volumes"), 0.0f, 1.0f)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, ControlsRowPadding)
				[
					MakeVariableRow(TEXT("Extinction"), TEXT("massif.Fog.Extinction"), 0.0f, 1.5f)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, ControlsRowPadding)
				[
					MakeVariableRow(TEXT("Reach"), TEXT("massif.Fog.Reach"), 1.0f, 12.0f)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, ControlsRowPadding)
				[
					MakeVariableRow(TEXT("Falloff"), TEXT("massif.Fog.Falloff"), 100.0f, 6000.0f)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, ControlsRowPadding)
				[
					MakeVariableRow(TEXT("Radial"), TEXT("massif.Fog.RadialShare"), 0.0f, 1.0f)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, ControlsRowPadding)
				[
					MakeVariableRow(TEXT("Cell (m)"), TEXT("massif.Fog.CellMeters"), 60.0f, 1200.0f)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, ControlsRowPadding)
				[
					MakeVariableRow(TEXT("Phase"), TEXT("massif.Fog.PhaseG"), 0.0f, 0.9f)
				]
			]
		]
	];
}
