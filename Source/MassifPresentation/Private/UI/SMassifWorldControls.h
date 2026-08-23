#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class IConsoleVariable;
class SWidget;
class UWorld;

/**
 * The corner panel: what time it is, what day of the year it is, and what the
 * fog is doing.
 *
 * Always up rather than behind a key, because all of these are things you set
 * while looking at what they change. The time of day goes through the
 * simulation as a command like any other mutation, so the sun, the sky and the
 * fog all follow one clock; the day of the year and the fog rows write console
 * variables, which is where anything read only by what is drawn belongs.
 *
 * Presentation only: it reads the clock and writes commands and cvars, and holds
 * nothing.
 */
class SMassifWorldControls : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMassifWorldControls) {}
		SLATE_ARGUMENT(TWeakObjectPtr<UWorld>, World)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	/** One labelled slider over a console variable, with its value written beside it. */
	TSharedRef<SWidget> MakeVariableRow(
		const TCHAR* Label, const TCHAR* VariableName, float Minimum, float Maximum);

	TSharedRef<SWidget> MakeTimeRow();
	TSharedRef<SWidget> MakeDayOfYearRow();
	TSharedRef<SWidget> MakeSectionLabel(const TCHAR* Label) const;

	/** Hours past midnight, from the simulation's own clock. */
	float GetHourOfDay() const;
	void SetHourOfDay(float Hour);
	FText GetClockText() const;

	/**
	 * Day of the year the rivers are drawn at. Not the simulation's: the year was
	 * solved at world gen and this picks which half-month of it to look at.
	 */
	static float GetDayOfYear();
	static void SetDayOfYear(float Day);
	static FText GetDateText();

	static float GetVariableValue(const TCHAR* VariableName);
	static void SetVariableValue(const TCHAR* VariableName, float Value);

	TWeakObjectPtr<UWorld> World;
};
