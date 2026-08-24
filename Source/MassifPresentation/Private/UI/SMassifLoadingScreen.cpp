#include "SMassifLoadingScreen.h"

#include "Style/MassifDesignTokens.h"
#include "Style/MassifWidgetStyles.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	constexpr float LoadingColumnWidth = 620.0f;
	constexpr float ProgressBarHeight = 3.0f;

	/** How long one quip holds before the next takes over. */
	constexpr double QuipSeconds = 4.0;

	struct FStageDescription
	{
		const TCHAR* Label;
		const TCHAR* const* Quips;
		int32 QuipCount;

		/** Share of the whole build, from the measured cost of each step. Sums to one. */
		float Weight;
	};

	const TCHAR* ElevationQuips[] = {
		TEXT("Unrolling Tirol."),
		TEXT("Asking the laserscan where the mountain is."),
	};

	const TCHAR* SaveQuips[] = {
		TEXT("Opening the box you left it in."),
		TEXT("Counting the boulders back in."),
	};

	const TCHAR* EnvironmentQuips[] = {
		TEXT("Deciding where it rains."),
		TEXT("Teaching the wind about ridges."),
		TEXT("Waiting for the sun to get over the ridge."),
		TEXT("Melting last winter."),
		TEXT("Letting the trees and the wind argue it out."),
		TEXT("Working out which slopes hold snow."),
	};

	const TCHAR* RiverQuips[] = {
		TEXT("Following water downhill, as one does."),
		TEXT("Ranking streams by how much they carry."),
	};

	const TCHAR* TerrainQuips[] = {
		TEXT("Cutting channels into bedrock."),
		TEXT("Grading a riverbed nobody will look at."),
	};

	const TCHAR* ObjectQuips[] = {
		TEXT("Rolling boulders down scree slopes."),
		TEXT("Planting trees at four hundred a second."),
	};

	const TCHAR* SpawnQuips[] = {
		TEXT("Handing the mountain to the renderer."),
		TEXT("Putting the sun in the sky."),
	};

	const TCHAR* FailureQuips[] = {
		TEXT("The mountain declined."),
	};

	const FStageDescription StageDescriptions[] = {
		{ TEXT("READING THE ELEVATION MODEL"), ElevationQuips, UE_ARRAY_COUNT(ElevationQuips), 0.04f },
		{ TEXT("READING THE SAVE"), SaveQuips, UE_ARRAY_COUNT(SaveQuips), 0.04f },
		{ TEXT("SOLVING THE ENVIRONMENT"), EnvironmentQuips, UE_ARRAY_COUNT(EnvironmentQuips), 0.74f },
		{ TEXT("TRACING RIVERS"), RiverQuips, UE_ARRAY_COUNT(RiverQuips), 0.06f },
		{ TEXT("CUTTING THE TERRAIN FIELD"), TerrainQuips, UE_ARRAY_COUNT(TerrainQuips), 0.04f },
		{ TEXT("PLACING OBJECTS"), ObjectQuips, UE_ARRAY_COUNT(ObjectQuips), 0.05f },
		{ TEXT("BUILDING THE WORLD"), SpawnQuips, UE_ARRAY_COUNT(SpawnQuips), 0.03f },
		{ TEXT("READY"), SpawnQuips, UE_ARRAY_COUNT(SpawnQuips), 0.0f },
		{ TEXT("GENERATION FAILED"), FailureQuips, UE_ARRAY_COUNT(FailureQuips), 0.0f },
	};

	const FStageDescription& Describe(EMassifWorldBuildStage Stage)
	{
		return StageDescriptions[static_cast<int32>(Stage)];
	}

	/** Everything before this stage, so a stage's own fraction lands inside its own band. */
	float FractionBeforeStage(EMassifWorldBuildStage Stage)
	{
		float Fraction = 0.0f;
		for (int32 Index = 0; Index < static_cast<int32>(Stage); ++Index)
		{
			Fraction += StageDescriptions[Index].Weight;
		}
		return Fraction;
	}
}

void SMassifLoadingScreen::Construct(const FArguments& InArgs)
{
	Progress = InArgs._Progress;
	StartedSeconds = FPlatformTime::Seconds();

	ChildSlot
	[
		SNew(SBox)
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Bottom)
		.Padding(
			MassifDesignTokens::Spacing::Page * 2.0f, 0.0f, 0.0f, MassifDesignTokens::Spacing::Page * 2.0f)
		[
			SNew(SBox).WidthOverride(LoadingColumnWidth)[BuildBody()]
		]
	];
}

TSharedRef<SWidget> SMassifLoadingScreen::BuildBody()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, MassifDesignTokens::Spacing::Hair)
		[
			SNew(STextBlock)
			.TextStyle(&MassifWidgetStyles::Label())
			.Text(this, &SMassifLoadingScreen::GetStageText)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, MassifDesignTokens::Spacing::Snug)
		[
			SNew(STextBlock)
			.TextStyle(&MassifWidgetStyles::Title())
			.Text(this, &SMassifLoadingScreen::GetQuipText)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBox)
			.HeightOverride(ProgressBarHeight)
			[
				SNew(SProgressBar)
				.Style(&MassifWidgetStyles::ProgressBar())
				.Percent(this, &SMassifLoadingScreen::GetOverallFraction)
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, MassifDesignTokens::Spacing::Tight, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(STextBlock)
				.TextStyle(&MassifWidgetStyles::Muted())
				.Text(this, &SMassifLoadingScreen::GetDetailText)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(STextBlock)
				.TextStyle(&MassifWidgetStyles::Readout())
				.Text(this, &SMassifLoadingScreen::GetElapsedText)
			]
		];
}

FMassifWorldBuildProgressSnapshot SMassifLoadingScreen::ReadProgress() const
{
	if (!Progress.IsValid())
	{
		return FMassifWorldBuildProgressSnapshot();
	}
	return Progress->Read();
}

FText SMassifLoadingScreen::GetStageText() const
{
	return FText::FromString(Describe(ReadProgress().Stage).Label);
}

FText SMassifLoadingScreen::GetDetailText() const
{
	return FText::FromString(ReadProgress().Detail);
}

FText SMassifLoadingScreen::GetQuipText() const
{
	const FStageDescription& Stage = Describe(ReadProgress().Stage);
	const int32 Step = static_cast<int32>((FPlatformTime::Seconds() - StartedSeconds) / QuipSeconds);
	return FText::FromString(Stage.Quips[Step % Stage.QuipCount]);
}

FText SMassifLoadingScreen::GetElapsedText() const
{
	return FText::FromString(
		FString::Printf(TEXT("%.1f s"), FPlatformTime::Seconds() - StartedSeconds));
}

TOptional<float> SMassifLoadingScreen::GetOverallFraction() const
{
	const FMassifWorldBuildProgressSnapshot Snapshot = ReadProgress();
	const FStageDescription& Stage = Describe(Snapshot.Stage);
	const float Fraction =
		FractionBeforeStage(Snapshot.Stage) + Stage.Weight * Snapshot.StageFraction;
	return FMath::Clamp(Fraction, 0.0f, 1.0f);
}
