#include "Debug/MassifEnvironmentInspector.h"
#include "MassifElevationSource.h"
#include "MassifEnvironmentGraph.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifSunProcesses.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr uint32 InspectorTestSeed = 4711;

	/** A small analytic world: the inspector is being tested, not the solves under it. */
	TUniquePtr<FMassifEnvironmentScheduler> MakeInspectorScheduler()
	{
		FMassifAnalyticElevationSource::FParams TerrainParams;
		TerrainParams.Shape = EMassifAnalyticTerrain::Cone;
		TerrainParams.WorldSizeMeters = 1024.0;

		TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MakeUnique<FMassifEnvironmentScheduler>(
			MakeShared<FMassifAnalyticElevationSource>(TerrainParams), FMassifGeoreference(), InspectorTestSeed);

		FMassifEnvironmentBakeParams WaterParams;
		WaterParams.CellSizeMeters = 32.0;
		WaterParams.DropletsPerCell = 0.05;
		WaterParams.MaxDropletSteps = 8;
		MassifRegisterAltitudeProcess(*Scheduler, WaterParams);

		FMassifSunParams SunParams;
		SunParams.CellSizeMeters = 128.0;
		SunParams.HorizonAzimuthCount = 4;
		SunParams.HorizonStepCount = 6;
		SunParams.SunPathStepCount = 4;
		MassifRegisterSunProcesses(*Scheduler, SunParams);

		FString Error;
		checkf(Scheduler->BuildSchedule(Error), TEXT("%s"), *Error);
		Scheduler->Execute();
		return Scheduler;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentInspectorParseTest,
	"Massif.Presentation.EnvironmentInspector.Parse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentInspectorParseTest::RunTest(const FString& Parameters)
{
	const EMassifFidelity Levels[] = {
		EMassifFidelity::Off, EMassifFidelity::Cheap, EMassifFidelity::Standard, EMassifFidelity::Reference};
	for (EMassifFidelity Level : Levels)
	{
		// The console takes what the panel prints, so the two spellings must agree.
		const TOptional<EMassifFidelity> Parsed =
			MassifEnvironmentInspector::ParseFidelity(MassifFidelityName(Level));
		TestTrue(FString::Printf(TEXT("%s round-trips"), MassifFidelityName(Level)), Parsed.IsSet());
		TestTrue(FString::Printf(TEXT("%s parses back to itself"), MassifFidelityName(Level)), *Parsed == Level);
	}

	TestTrue(
		TEXT("Case does not matter"), MassifEnvironmentInspector::ParseFidelity(TEXT("standard")).IsSet());
	TestFalse(
		TEXT("A name that is not a level is refused"),
		MassifEnvironmentInspector::ParseFidelity(TEXT("Ludicrous")).IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentInspectorSnapshotTest,
	"Massif.Presentation.EnvironmentInspector.Snapshot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentInspectorSnapshotTest::RunTest(const FString& Parameters)
{
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MakeInspectorScheduler();
	const FMassifInspectorSnapshot Snapshot = MassifEnvironmentInspector::Snapshot(*Scheduler);

	TestEqual(
		TEXT("Every registered process is listed"), Snapshot.Processes.Num(), Scheduler->GetProcessCount());
	TestTrue(TEXT("The listing is in execution order"),
		Snapshot.Processes[0].Name == Scheduler->GetExecutionOrder()[0]);
	TestTrue(TEXT("Every channel is listed"), Snapshot.Channels.Num() >= 4);
	TestTrue(TEXT("Nothing is dirty straight after a solve"), !Snapshot.Processes[0].bDirty);
	TestTrue(TEXT("The state reports what it is holding"), Snapshot.ResidentByteCount > 0);

	for (const FMassifInspectorChannelRow& Row : Snapshot.Channels)
	{
		TestTrue(
			FString::Printf(TEXT("%s was written by the solve"), *Row.Name.ToString()), Row.bWritten);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentInspectorFidelityTest,
	"Massif.Presentation.EnvironmentInspector.Fidelity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentInspectorFidelityTest::RunTest(const FString& Parameters)
{
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MakeInspectorScheduler();
	const uint64 StandardHash = Scheduler->GetState().ComputeContentHash();

	TestTrue(
		TEXT("A level the process implements is applied and re-solved"),
		MassifEnvironmentInspector::SetFidelityAndResolve(
			*Scheduler, MassifEnvironmentProcesses::Insolation(), EMassifFidelity::Off));
	TestTrue(
		TEXT("Re-solving leaves nothing dirty"),
		!Scheduler->IsProcessDirty(MassifEnvironmentProcesses::Insolation()));
	TestTrue(
		TEXT("Turning the sun off changed what the graph holds"),
		Scheduler->GetState().ComputeContentHash() != StandardHash);

	TestFalse(
		TEXT("A level the process does not implement is refused"),
		MassifEnvironmentInspector::SetFidelityAndResolve(
			*Scheduler, MassifEnvironmentProcesses::Altitude(), EMassifFidelity::Reference));

	// Switching back has to reproduce the world exactly, or a fidelity toggle is a
	// one-way door and nothing seen through it can be trusted.
	MassifEnvironmentInspector::SetFidelityAndResolve(
		*Scheduler, MassifEnvironmentProcesses::Insolation(), EMassifFidelity::Standard);
	TestTrue(
		TEXT("Switching back reproduces the original world"),
		Scheduler->GetState().ComputeContentHash() == StandardHash);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentInspectorPresetTest,
	"Massif.Presentation.EnvironmentInspector.Preset",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentInspectorPresetTest::RunTest(const FString& Parameters)
{
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MakeInspectorScheduler();

	// Off is the one level every process implements, so a preset of Off must reach
	// all of them rather than skipping the ones with no cheaper model.
	const int32 ChangedCount = MassifEnvironmentInspector::SetPresetAndResolve(*Scheduler, EMassifFidelity::Off);
	TestEqual(TEXT("An Off preset reaches every process"), ChangedCount, Scheduler->GetProcessCount());

	for (FName ProcessName : Scheduler->GetExecutionOrder())
	{
		TestTrue(
			FString::Printf(TEXT("%s is off"), *ProcessName.ToString()),
			Scheduler->GetFidelity(ProcessName) == EMassifFidelity::Off);
	}

	// Reference is implemented by some and not others, so the count is a subset and
	// the ones that cannot do it keep the level they had rather than failing.
	const int32 ReferenceCount =
		MassifEnvironmentInspector::SetPresetAndResolve(*Scheduler, EMassifFidelity::Reference);
	TestTrue(
		FString::Printf(TEXT("A Reference preset reaches only what implements it (%d of %d)"),
			ReferenceCount, Scheduler->GetProcessCount()),
		ReferenceCount > 0 && ReferenceCount < Scheduler->GetProcessCount());
	return true;
}
