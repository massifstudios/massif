#pragma once

#include "CoreMinimal.h"
#include "MassifWorldBuild.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

/**
 * What a world generation looks like while it runs.
 *
 * Reads the build's progress every paint rather than being pushed to, because the
 * build is on a worker and a screen that had to be told would need a lock across
 * the frame boundary to say anything.
 */
class SMassifLoadingScreen : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMassifLoadingScreen) {}
		SLATE_ARGUMENT(TSharedPtr<FMassifWorldBuildProgress>, Progress)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	TSharedRef<SWidget> BuildBody();

	FText GetStageText() const;
	FText GetDetailText() const;
	FText GetQuipText() const;
	FText GetElapsedText() const;
	TOptional<float> GetOverallFraction() const;

	FMassifWorldBuildProgressSnapshot ReadProgress() const;

	/** Wall clock, only ever shown. Nothing the build does is decided from it. */
	double StartedSeconds = 0.0;

	TSharedPtr<FMassifWorldBuildProgress> Progress;
};
