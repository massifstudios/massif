#include "Algo/Reverse.h"
#include "MassifElevationSource.h"
#include "MassifEnvironmentScheduler.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr uint32 SchedulerTestSeed = 4711;
	constexpr double SchedulerTestWorldMeters = 2000.0;
	constexpr double SchedulerTestCellMeters = 64.0;

	TSharedRef<const IMassifElevationSource> MakeTestElevation()
	{
		FMassifAnalyticElevationSource::FParams Params;
		Params.Shape = EMassifAnalyticTerrain::Cone;
		Params.WorldSizeMeters = SchedulerTestWorldMeters;
		return MakeShared<FMassifAnalyticElevationSource>(Params);
	}

	FMassifChannelDescriptor MakeTestChannel(FName Name)
	{
		FMassifChannelDescriptor Descriptor;
		Descriptor.Name = Name;
		Descriptor.Type = EMassifChannelType::Scalar;
		Descriptor.Encoding = EMassifChannelEncoding::Float32;
		Descriptor.CellSizeMeters = SchedulerTestCellMeters;
		Descriptor.Unit = TEXT("test");
		return Descriptor;
	}

	/** What the processes did, in the order they did it. */
	struct FRunLog
	{
		TArray<FName> RunOrder;

		int32 CountRunsOf(FName ProcessName) const
		{
			int32 RunCount = 0;
			for (FName Name : RunOrder)
			{
				RunCount += static_cast<int32>(Name == ProcessName);
			}
			return RunCount;
		}
	};

	/** A process that does nothing but record that it ran and fill its outputs with a marker. */
	class FTestProcess final : public IMassifEnvironmentProcess
	{
	public:
		FTestProcess(FName InName, TArray<FName> InInputs, TArray<FName> InOutputs, TSharedRef<FRunLog> InLog)
			: Name(InName)
			, Inputs(MoveTemp(InInputs))
			, Outputs(MoveTemp(InOutputs))
			, Log(InLog)
		{
		}

		void SetParticipatesInFeedback(bool bInParticipates)
		{
			bParticipatesInFeedback = bInParticipates;
		}

		FMassifProcessDeclaration Declare() const override
		{
			FMassifProcessDeclaration Declaration;
			Declaration.Name = Name;
			Declaration.InputChannels = Inputs;
			Declaration.SupportedFidelities = {EMassifFidelity::Cheap, EMassifFidelity::Standard};
			Declaration.bParticipatesInFeedback = bParticipatesInFeedback;
			for (FName OutputName : Outputs)
			{
				Declaration.OutputChannels.Add(MakeTestChannel(OutputName));
			}
			return Declaration;
		}

		void Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const override
		{
			Log->RunOrder.Add(Name);
			for (FName OutputName : Outputs)
			{
				WriteMarker(Context, OutputName, Fidelity);
			}
		}

	private:
		/** Fidelity reaches the channel, so a fidelity change is visible in the content hash. */
		void WriteMarker(FMassifEnvironmentContext& Context, FName OutputName, EMassifFidelity Fidelity) const
		{
			const TUniquePtr<FMassifChannelWriteScope> Scope = Context.OpenOutput(OutputName);
			TArrayView<double> Values = Scope->GetComponent(0);
			const double Marker = 1.0 + static_cast<double>(Fidelity);
			for (int32 CellIndex = 0; CellIndex < Values.Num(); ++CellIndex)
			{
				Values[CellIndex] = Marker;
			}
			Scope->Commit();
		}

		FName Name;
		TArray<FName> Inputs;
		TArray<FName> Outputs;
		TSharedRef<FRunLog> Log;
		bool bParticipatesInFeedback = false;
	};

	TSharedRef<FTestProcess> MakeProcess(
		const TCHAR* Name, TArray<FName> Inputs, TArray<FName> Outputs, TSharedRef<FRunLog> Log)
	{
		return MakeShared<FTestProcess>(FName(Name), MoveTemp(Inputs), MoveTemp(Outputs), Log);
	}

	TUniquePtr<FMassifEnvironmentScheduler> MakeScheduler()
	{
		return MakeUnique<FMassifEnvironmentScheduler>(
			MakeTestElevation(), FMassifGeoreference(), SchedulerTestSeed);
	}

	/** Rain feeds flow feeds soil, with a light process off to one side of all three. */
	void RegisterChain(FMassifEnvironmentScheduler& Scheduler, TSharedRef<FRunLog> Log, bool bReversed)
	{
		TArray<TSharedRef<FTestProcess>> Processes;
		Processes.Add(MakeProcess(TEXT("Climate.Rain"), {}, {TEXT("Rain")}, Log));
		Processes.Add(MakeProcess(TEXT("Hydrology.Flow"), {TEXT("Rain")}, {TEXT("Flow")}, Log));
		Processes.Add(MakeProcess(TEXT("Soil.Depth"), {TEXT("Flow")}, {TEXT("Soil")}, Log));
		Processes.Add(MakeProcess(TEXT("Sun.Insolation"), {}, {TEXT("Insolation")}, Log));

		if (bReversed)
		{
			Algo::Reverse(Processes);
		}
		for (const TSharedRef<FTestProcess>& Process : Processes)
		{
			Scheduler.RegisterProcess(Process);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentSchedulerOrderTest,
	"Massif.WorldGen.EnvironmentScheduler.Order",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentSchedulerOrderTest::RunTest(const FString& Parameters)
{
	// The schedule is what the declarations imply, not the order anything was
	// added in, or dropping a system in would mean deciding where it belongs.
	const TSharedRef<FRunLog> ForwardLog = MakeShared<FRunLog>();
	const TSharedRef<FRunLog> ReversedLog = MakeShared<FRunLog>();

	const TUniquePtr<FMassifEnvironmentScheduler> Forward = MakeScheduler();
	const TUniquePtr<FMassifEnvironmentScheduler> Reversed = MakeScheduler();
	RegisterChain(*Forward, ForwardLog, false);
	RegisterChain(*Reversed, ReversedLog, true);

	FString ForwardError;
	FString ReversedError;
	TestTrue(TEXT("The forward schedule builds"), Forward->BuildSchedule(ForwardError));
	TestTrue(TEXT("The reversed schedule builds"), Reversed->BuildSchedule(ReversedError));

	Forward->Execute();
	Reversed->Execute();

	TestTrue(
		TEXT("Registration order does not reach the execution order"),
		Forward->GetExecutionOrder() == Reversed->GetExecutionOrder());
	TestTrue(TEXT("Both runs produced the same channels"),
		Forward->GetState().ComputeContentHash() == Reversed->GetState().ComputeContentHash());

	const TArray<FName> Order = Forward->GetExecutionOrder();
	TestTrue(
		TEXT("Rain runs before the flow that reads it"),
		Order.IndexOfByKey(FName(TEXT("Climate.Rain"))) < Order.IndexOfByKey(FName(TEXT("Hydrology.Flow"))));
	TestTrue(
		TEXT("Flow runs before the soil that reads it"),
		Order.IndexOfByKey(FName(TEXT("Hydrology.Flow"))) < Order.IndexOfByKey(FName(TEXT("Soil.Depth"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentSchedulerRejectionTest,
	"Massif.WorldGen.EnvironmentScheduler.Rejection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentSchedulerRejectionTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FRunLog> Log = MakeShared<FRunLog>();

	const TUniquePtr<FMassifEnvironmentScheduler> Unproduced = MakeScheduler();
	Unproduced->RegisterProcess(MakeProcess(TEXT("Soil.Depth"), {TEXT("Flow")}, {TEXT("Soil")}, Log));
	FString UnproducedError;
	TestFalse(TEXT("A channel nobody produces is refused"), Unproduced->BuildSchedule(UnproducedError));
	TestTrue(TEXT("The refusal names the channel"), UnproducedError.Contains(TEXT("Flow")));

	const TUniquePtr<FMassifEnvironmentScheduler> Contested = MakeScheduler();
	Contested->RegisterProcess(MakeProcess(TEXT("Climate.Rain"), {}, {TEXT("Rain")}, Log));
	Contested->RegisterProcess(MakeProcess(TEXT("Climate.OtherRain"), {}, {TEXT("Rain")}, Log));
	FString ContestedError;
	TestFalse(TEXT("Two producers of one channel are refused"), Contested->BuildSchedule(ContestedError));

	const TUniquePtr<FMassifEnvironmentScheduler> Undeclared = MakeScheduler();
	Undeclared->RegisterProcess(MakeProcess(TEXT("Wind.Field"), {TEXT("Canopy")}, {TEXT("Wind")}, Log));
	Undeclared->RegisterProcess(MakeProcess(TEXT("Vegetation.Canopy"), {TEXT("Wind")}, {TEXT("Canopy")}, Log));
	FString UndeclaredError;
	TestFalse(TEXT("An undeclared cycle is refused"), Undeclared->BuildSchedule(UndeclaredError));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentSchedulerFeedbackTest,
	"Massif.WorldGen.EnvironmentScheduler.Feedback",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentSchedulerFeedbackTest::RunTest(const FString& Parameters)
{
	// Canopy slows the wind that redistributes the snow that decides where canopy
	// grows: a declared cycle becomes one group solved a fixed number of times.
	const TSharedRef<FRunLog> Log = MakeShared<FRunLog>();
	const TSharedRef<FTestProcess> Wind = MakeProcess(TEXT("Wind.Field"), {TEXT("Canopy")}, {TEXT("Wind")}, Log);
	const TSharedRef<FTestProcess> Canopy =
		MakeProcess(TEXT("Vegetation.Canopy"), {TEXT("Wind")}, {TEXT("Canopy")}, Log);
	Wind->SetParticipatesInFeedback(true);
	Canopy->SetParticipatesInFeedback(true);

	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MakeScheduler();
	Scheduler->SetRelaxationIterationCount(3);
	Scheduler->RegisterProcess(Wind);
	Scheduler->RegisterProcess(Canopy);
	Scheduler->RegisterProcess(MakeProcess(TEXT("Sun.Insolation"), {}, {TEXT("Insolation")}, Log));

	FString Error;
	TestTrue(TEXT("A declared cycle schedules"), Scheduler->BuildSchedule(Error));

	const TArray<TArray<FName>> FeedbackGroups = Scheduler->GetFeedbackGroups();
	TestEqual(TEXT("Exactly one feedback group was found"), FeedbackGroups.Num(), 1);
	TestEqual(TEXT("Both cycle members are in it"), FeedbackGroups[0].Num(), 2);

	Scheduler->Execute();
	TestEqual(TEXT("The loop ran its fixed iteration count"), Log->CountRunsOf(FName(TEXT("Wind.Field"))), 3);
	TestEqual(TEXT("Both members ran the same number of times"),
		Log->CountRunsOf(FName(TEXT("Vegetation.Canopy"))), 3);
	TestEqual(TEXT("A process outside the loop ran once"), Log->CountRunsOf(FName(TEXT("Sun.Insolation"))), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentSchedulerDirtyTest,
	"Massif.WorldGen.EnvironmentScheduler.Dirty",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentSchedulerDirtyTest::RunTest(const FString& Parameters)
{
	// Re-running the whole graph for one changed parameter is what makes
	// interactive fidelity switching unaffordable.
	const TSharedRef<FRunLog> Log = MakeShared<FRunLog>();
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MakeScheduler();
	RegisterChain(*Scheduler, Log, false);

	FString Error;
	TestTrue(TEXT("The schedule builds"), Scheduler->BuildSchedule(Error));
	Scheduler->Execute();

	Scheduler->Execute();
	TestEqual(TEXT("A second run with nothing dirty does nothing"), Log->RunOrder.Num(), 4);

	Log->RunOrder.Empty();
	Scheduler->MarkDirty(FName(TEXT("Hydrology.Flow")));
	Scheduler->Execute();

	TestEqual(TEXT("The dirty process re-ran"), Log->CountRunsOf(FName(TEXT("Hydrology.Flow"))), 1);
	TestEqual(TEXT("So did everything downstream of it"), Log->CountRunsOf(FName(TEXT("Soil.Depth"))), 1);
	TestEqual(TEXT("Its own input did not"), Log->CountRunsOf(FName(TEXT("Climate.Rain"))), 0);
	TestEqual(TEXT("Nor did an unrelated branch"), Log->CountRunsOf(FName(TEXT("Sun.Insolation"))), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentSchedulerFidelityTest,
	"Massif.WorldGen.EnvironmentScheduler.Fidelity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentSchedulerFidelityTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FRunLog> Log = MakeShared<FRunLog>();
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MakeScheduler();
	RegisterChain(*Scheduler, Log, false);

	FString Error;
	TestTrue(TEXT("The schedule builds"), Scheduler->BuildSchedule(Error));
	Scheduler->Execute();
	const uint64 StandardHash = Scheduler->GetState().ComputeContentHash();

	TestFalse(
		TEXT("A fidelity the process does not implement is refused"),
		Scheduler->SetFidelity(FName(TEXT("Hydrology.Flow")), EMassifFidelity::Reference));
	TestTrue(
		TEXT("The refused change left the fidelity alone"),
		Scheduler->GetFidelity(FName(TEXT("Hydrology.Flow"))) == EMassifFidelity::Standard);

	TestTrue(
		TEXT("An implemented fidelity is accepted"),
		Scheduler->SetFidelity(FName(TEXT("Hydrology.Flow")), EMassifFidelity::Cheap));
	TestTrue(
		TEXT("Changing it marks the process dirty"), Scheduler->IsProcessDirty(FName(TEXT("Hydrology.Flow"))));

	Log->RunOrder.Empty();
	Scheduler->Execute();
	TestEqual(TEXT("Only the changed process and its dependents ran"), Log->RunOrder.Num(), 2);
	TestTrue(
		TEXT("The lower fidelity changed what the graph produced"),
		Scheduler->GetState().ComputeContentHash() != StandardHash);

	Scheduler->SetFidelity(FName(TEXT("Hydrology.Flow")), EMassifFidelity::Standard);
	Scheduler->Execute();
	TestTrue(
		TEXT("Switching back reproduces the original result"),
		Scheduler->GetState().ComputeContentHash() == StandardHash);
	return true;
}
