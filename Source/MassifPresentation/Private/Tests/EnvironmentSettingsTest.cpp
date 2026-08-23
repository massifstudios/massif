#include "MassifTestEnvironment.h"
#include "Misc/AutomationTest.h"
#include "UI/MassifEnvironmentSettingsModel.h"
#include "UI/MassifFidelityLabels.h"
#include "UI/SMassifEnvironmentSettings.h"

namespace
{
	/** A process the fixture registers that implements more than one level. */
	FName FindAdjustableProcess(const FMassifEnvironmentSettingsModel& Model)
	{
		for (const FMassifFidelityRow& Row : Model.GetRows())
		{
			if (Row.SupportedLevels.Num() > 1)
			{
				return Row.ProcessName;
			}
		}
		return NAME_None;
	}

	const FMassifFidelityRow* FindOtherLevel(const FMassifFidelityRow& Row, EMassifFidelity& OutOther)
	{
		for (EMassifFidelity Fidelity : Row.SupportedLevels)
		{
			if (Fidelity != Row.Current)
			{
				OutOther = Fidelity;
				return &Row;
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentSettingsStagingTest,
	"Massif.Presentation.EnvironmentSettings.Staging",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentSettingsStagingTest::RunTest(const FString& Parameters)
{
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MassifTestEnvironment::MakeScheduler();
	FMassifEnvironmentSettingsModel Model(*Scheduler);

	TestEqual(TEXT("Every scheduled process gets a row"),
		Model.GetRows().Num(), Scheduler->GetExecutionOrder().Num());
	TestFalse(TEXT("Nothing is staged to begin with"), Model.HasChanges());
	TestEqual(TEXT("And nothing would be re-solved"), Model.GetAffectedDisplayNames().Num(), 0);

	const FName ProcessName = FindAdjustableProcess(Model);
	if (!TestTrue(TEXT("The fixture has a process with a choice to make"), ProcessName != NAME_None))
	{
		return false;
	}

	EMassifFidelity Other = EMassifFidelity::Off;
	FindOtherLevel(*Model.FindRow(ProcessName), Other);
	Model.Stage(ProcessName, Other);

	TestTrue(TEXT("Staging a level is a change"), Model.HasChanges());
	TestEqual(TEXT("But the graph has not moved yet"), Scheduler->GetFidelity(ProcessName),
		Model.FindRow(ProcessName)->Current);

	Model.Revert();
	TestFalse(TEXT("Reverting drops it"), Model.HasChanges());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentSettingsUnsupportedTest,
	"Massif.Presentation.EnvironmentSettings.Unsupported",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentSettingsUnsupportedTest::RunTest(const FString& Parameters)
{
	// A level a process does not implement cannot be staged: the scheduler would
	// refuse it on Apply, and a screen that let it be picked would show a change
	// that silently does not happen.
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MassifTestEnvironment::MakeScheduler();
	FMassifEnvironmentSettingsModel Model(*Scheduler);

	int32 RefusedCount = 0;
	for (const FMassifFidelityRow& Row : Model.GetRows())
	{
		for (EMassifFidelity Fidelity : MassifFidelityLabels::AllLevels())
		{
			if (!Row.Supports(Fidelity))
			{
				Model.Stage(Row.ProcessName, Fidelity);
				RefusedCount += static_cast<int32>(Model.FindRow(Row.ProcessName)->Staged != Fidelity);
			}
		}
	}
	TestTrue(TEXT("The fixture offers at least one level nothing implements"), RefusedCount > 0);
	TestFalse(TEXT("None of them staged"), Model.HasChanges());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentSettingsAffectedTest,
	"Massif.Presentation.EnvironmentSettings.Affected",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentSettingsAffectedTest::RunTest(const FString& Parameters)
{
	// The list of what a change costs comes out of the graph, so it has to name the
	// process itself and everything the scheduler would actually re-run.
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MassifTestEnvironment::MakeScheduler();
	FMassifEnvironmentSettingsModel Model(*Scheduler);

	const FName ProcessName = FindAdjustableProcess(Model);
	if (!TestTrue(TEXT("The fixture has a process with a choice to make"), ProcessName != NAME_None))
	{
		return false;
	}

	EMassifFidelity Other = EMassifFidelity::Off;
	FindOtherLevel(*Model.FindRow(ProcessName), Other);
	Model.Stage(ProcessName, Other);

	const TArray<FString> Affected = Model.GetAffectedDisplayNames();
	TestTrue(TEXT("A staged change re-solves something"), Affected.Num() > 0);
	TestTrue(TEXT("...starting with the process that changed"),
		Affected.Contains(Scheduler->GetDisplayName(ProcessName)));
	TestEqual(TEXT("...and exactly what the scheduler says it would run"),
		Affected.Num(), Scheduler->GetAffectedProcesses(ProcessName).Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentSettingsApplyTest,
	"Massif.Presentation.EnvironmentSettings.Apply",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentSettingsApplyTest::RunTest(const FString& Parameters)
{
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MassifTestEnvironment::MakeScheduler();
	FMassifEnvironmentSettingsModel Model(*Scheduler);

	const FName ProcessName = FindAdjustableProcess(Model);
	if (!TestTrue(TEXT("The fixture has a process with a choice to make"), ProcessName != NAME_None))
	{
		return false;
	}

	EMassifFidelity Other = EMassifFidelity::Off;
	FindOtherLevel(*Model.FindRow(ProcessName), Other);
	Model.Stage(ProcessName, Other);
	Model.Apply();

	TestEqual(TEXT("Apply moves the graph"), Scheduler->GetFidelity(ProcessName), Other);
	TestFalse(TEXT("And leaves nothing staged"), Model.HasChanges());
	TestEqual(TEXT("The row now reads as the level applied"), Model.FindRow(ProcessName)->Current, Other);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentSettingsLabelTest,
	"Massif.Presentation.EnvironmentSettings.Labels",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentSettingsLabelTest::RunTest(const FString& Parameters)
{
	// The labels are what gets written to the ini, so they have to survive a round
	// trip: a level saved under one name and parsed back as another would silently
	// move a system on the next launch.
	for (EMassifFidelity Fidelity : MassifFidelityLabels::AllLevels())
	{
		const FString Label = MassifFidelityLabels::Label(Fidelity);
		const TOptional<EMassifFidelity> Parsed = MassifFidelityLabels::Parse(Label);
		TestTrue(FString::Printf(TEXT("%s parses back"), *Label), Parsed.IsSet() && *Parsed == Fidelity);
	}

	TestFalse(TEXT("An unknown level parses to nothing"),
		MassifFidelityLabels::Parse(TEXT("Ultra")).IsSet());
	TestEqual(TEXT("Every level has a label"),
		MassifFidelityLabels::AllLevels().Num(), static_cast<int32>(EMassifFidelity::Reference) + 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentSettingsDisplayNameTest,
	"Massif.Presentation.EnvironmentSettings.DisplayNames",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentSettingsDisplayNameTest::RunTest(const FString& Parameters)
{
	// Nothing in the presentation layer names a process, so a system added to the
	// graph appears here labelled by itself. A process that declares nothing still
	// gets a row, under its own name.
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MassifTestEnvironment::MakeScheduler();
	const FMassifEnvironmentSettingsModel Model(*Scheduler);

	for (const FMassifFidelityRow& Row : Model.GetRows())
	{
		TestFalse(FString::Printf(TEXT("%s has a label"), *Row.ProcessName.ToString()),
			Row.DisplayName.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentSettingsWidgetTest,
	"Massif.Presentation.EnvironmentSettings.Widget",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentSettingsWidgetTest::RunTest(const FString& Parameters)
{
	// The screen builds a button per level per process against a live model, and
	// binds every one of them back to it. Constructing it is what catches a row
	// bound to something the model no longer holds; nothing here draws a pixel.
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MassifTestEnvironment::MakeScheduler();
	const TSharedRef<FMassifEnvironmentSettingsModel> Model =
		MakeShared<FMassifEnvironmentSettingsModel>(*Scheduler);

	bool bClosed = false;
	const TSharedRef<SMassifEnvironmentSettings> Screen = SNew(SMassifEnvironmentSettings)
		.Model(Model)
		.OnClose(FSimpleDelegate::CreateLambda([&bClosed]() { bClosed = true; }));

	TestTrue(TEXT("The screen builds over a solved graph"), Screen->GetChildren()->Num() > 0);

	// Applying rebuilds the rows the widget was built against, which is the case
	// that would leave a button pointing at a row that has been thrown away.
	const FName ProcessName = FindAdjustableProcess(*Model);
	if (TestTrue(TEXT("The fixture has a process with a choice to make"), ProcessName != NAME_None))
	{
		EMassifFidelity Other = EMassifFidelity::Off;
		FindOtherLevel(*Model->FindRow(ProcessName), Other);
		Model->Stage(ProcessName, Other);
		Model->Apply();
		TestEqual(TEXT("And the screen still reads the rebuilt rows"),
			Model->FindRow(ProcessName)->Current, Other);
	}
	return true;
}
