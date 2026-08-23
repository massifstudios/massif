#include "Engine/GameInstance.h"
#include "MassifSimClock.h"
#include "MassifSimCommand.h"
#include "MassifSimState.h"
#include "MassifSimSubsystem.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	constexpr int32 SimTestSeed = 20260823;
	constexpr int32 SimTestOtherSeed = 20260824;

	/**
	 * Frame lengths the accumulator converts to ticks without rounding.
	 *
	 * The determinism contract is in ticks, not seconds (ARCHITECTURE §4): the state
	 * after N steps is one value, and how much real time it took to get there is not
	 * part of it. Frame time reaches the sim only through a running total, and a
	 * total of many inexact frame lengths can land a hair either side of a tick
	 * boundary — eighty frames of a hundredth of a second are not exactly one
	 * second. These lengths halve or double the tick exactly, so a test can assert a
	 * tick count without asserting anything about floating point.
	 */
	constexpr double SimTestFramePerTick = MassifSimTickSeconds;
	constexpr double SimTestFrameHalfTick = MassifSimTickSeconds / 2.0;
	constexpr double SimTestFrameTwoTicks = MassifSimTickSeconds * 2.0;

	/** A quarter second, which at four times speed is a whole one and ten steps. */
	constexpr double SimTestQuarterSecond = 0.25;

	uint64 HashAfterTicks(int32 MasterSeed, int32 TickCount)
	{
		FMassifSimState State;
		MassifResetSimState(State, MasterSeed);
		for (int32 TickIndex = 0; TickIndex < TickCount; ++TickIndex)
		{
			MassifAdvanceSimTick(State, TConstArrayView<FMassifSimCommand>());
		}
		return MassifComputeSimStateHash(State);
	}

	/** Ticks one frame of the given length is worth at a speed. */
	int32 TicksInOneFrame(EMassifGameSpeed Speed, double FrameSeconds)
	{
		FMassifSimClock Clock;
		Clock.Speed = Speed;
		return Clock.ConsumeTicks(FrameSeconds);
	}

	TStrongObjectPtr<UMassifSimSubsystem> MakeStartedSubsystem(int32 MasterSeed)
	{
		// A game instance subsystem may only be built inside a game instance, so the
		// test makes the smallest owner that satisfies that rather than standing a
		// running game up. Holding the subsystem keeps its outer alive with it.
		UGameInstance* Owner = NewObject<UGameInstance>();
		TStrongObjectPtr<UMassifSimSubsystem> Subsystem(NewObject<UMassifSimSubsystem>(Owner));
		Subsystem->StartNewGame(MasterSeed);
		return Subsystem;
	}

	void AdvanceFrames(UMassifSimSubsystem& Subsystem, int32 FrameCount, double FrameSeconds)
	{
		for (int32 FrameIndex = 0; FrameIndex < FrameCount; ++FrameIndex)
		{
			Subsystem.AdvanceFrame(FrameSeconds);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifSimDeterminismTest,
	"Massif.Sim.Core.Determinism",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifSimDeterminismTest::RunTest(const FString& Parameters)
{
	// The contract the whole simulation is built on (ARCHITECTURE §4): the state
	// after a given number of steps from a given seed is one value, not a value per
	// run. Nothing here is load-bearing yet, and that is the point of writing it
	// now — every system that lands afterwards is measured against a test that was
	// already passing.
	constexpr int32 TickCount = 500;

	TestEqual(
		TEXT("The same seed reaches the same state"),
		HashAfterTicks(SimTestSeed, TickCount), HashAfterTicks(SimTestSeed, TickCount));
	TestNotEqual(
		TEXT("A different seed does not"),
		HashAfterTicks(SimTestSeed, TickCount), HashAfterTicks(SimTestOtherSeed, TickCount));
	TestNotEqual(
		TEXT("Nor does the same seed a different distance along"),
		HashAfterTicks(SimTestSeed, TickCount), HashAfterTicks(SimTestSeed, TickCount + 1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifSimCommandsLandOnTickBoundaryTest,
	"Massif.Sim.Core.CommandsLandOnTickBoundary",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifSimCommandsLandOnTickBoundaryTest::RunTest(const FString& Parameters)
{
	// A command that took effect the moment presentation raised it would make the
	// state depend on where in a frame the click happened, which no frame rate
	// reproduces twice. Queued and applied at the start of a step is what removes
	// that, and it is the same boundary a client would send across (§3).
	const TStrongObjectPtr<UMassifSimSubsystem> Subsystem = MakeStartedSubsystem(SimTestSeed);
	Subsystem->EnqueueCommand(FMassifSimCommand(EMassifSimCommandType::AddToCounter, 7));

	TestEqual(TEXT("Enqueuing alone changes nothing"), Subsystem->GetState().CommandCounter, int64(0));
	TestEqual(TEXT("The command is waiting"), Subsystem->GetPendingCommandCount(), 1);

	// Most of a step's worth of real time, which is not a step.
	Subsystem->AdvanceFrame(MassifSimTickSeconds * 0.9);
	TestEqual(TEXT("Nor does a frame too short to step"), Subsystem->GetState().CommandCounter, int64(0));
	TestEqual(TEXT("And it is still waiting"), Subsystem->GetPendingCommandCount(), 1);

	Subsystem->AdvanceFrame(MassifSimTickSeconds * 0.2);
	TestEqual(TEXT("The step that follows applies it"), Subsystem->GetState().CommandCounter, int64(7));
	TestEqual(TEXT("Exactly one step has run"), Subsystem->GetState().TickCount, int64(1));
	TestEqual(TEXT("And the queue is empty"), Subsystem->GetPendingCommandCount(), 0);

	// Draining the queue whole is what stops a command being applied twice by the
	// steps that follow it in the same frame.
	Subsystem->AdvanceFrame(MassifSimTickSeconds * 5.0);
	TestEqual(TEXT("Later steps do not apply it again"), Subsystem->GetState().CommandCounter, int64(7));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifSimGameSpeedChangesTickCountTest,
	"Massif.Sim.Core.GameSpeedChangesTickCount",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifSimGameSpeedChangesTickCountTest::RunTest(const FString& Parameters)
{
	// Speed is how many steps a second is worth and never how long a step is. The
	// second half is the one worth testing: if fast forward stretched the step
	// instead, the same world would come out differently depending on what speed it
	// had been watched at.
	TestEqual(
		TEXT("A second at normal speed is ten steps, which is the fixed rate"),
		TicksInOneFrame(EMassifGameSpeed::Normal, 1.0), 10);
	TestEqual(
		TEXT("A quarter of one at fast is the same ten"),
		TicksInOneFrame(EMassifGameSpeed::Fast, SimTestQuarterSecond), 10);
	TestEqual(
		TEXT("And no length of frame is worth a step while paused"),
		TicksInOneFrame(EMassifGameSpeed::Paused, 10.0), 0);

	const TStrongObjectPtr<UMassifSimSubsystem> Normal = MakeStartedSubsystem(SimTestSeed);
	const TStrongObjectPtr<UMassifSimSubsystem> Fast = MakeStartedSubsystem(SimTestSeed);
	Fast->SetGameSpeed(EMassifGameSpeed::Fast);

	// Four seconds against one: different amounts of real time, the same number of
	// steps, and therefore the same state.
	AdvanceFrames(*Normal, 40, SimTestFramePerTick);
	AdvanceFrames(*Fast, 40, SimTestFramePerTick / 4.0);

	TestEqual(TEXT("Both have run forty steps"), Normal->GetState().TickCount, int64(40));
	TestEqual(TEXT("Both have run forty steps"), Fast->GetState().TickCount, int64(40));
	TestEqual(
		TEXT("So both are in the same state, whatever speed got them there"),
		Normal->ComputeStateHash(), Fast->ComputeStateHash());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifSimTicksAreFrameRateIndependentTest,
	"Massif.Sim.Core.TicksAreFrameRateIndependent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifSimTicksAreFrameRateIndependentTest::RunTest(const FString& Parameters)
{
	// The accumulator exists so that machines drawing frames at different rates
	// simulate the same world. Four seconds here, cut three ways: two frames to a
	// step, one to a step, and one frame worth two.
	const TStrongObjectPtr<UMassifSimSubsystem> Smooth = MakeStartedSubsystem(SimTestSeed);
	const TStrongObjectPtr<UMassifSimSubsystem> Even = MakeStartedSubsystem(SimTestSeed);
	const TStrongObjectPtr<UMassifSimSubsystem> Choppy = MakeStartedSubsystem(SimTestSeed);

	AdvanceFrames(*Smooth, 80, SimTestFrameHalfTick);
	AdvanceFrames(*Even, 40, SimTestFramePerTick);
	AdvanceFrames(*Choppy, 20, SimTestFrameTwoTicks);

	TestEqual(TEXT("Two frames to a step reaches forty"), Smooth->GetState().TickCount, int64(40));
	TestEqual(TEXT("One frame to a step reaches forty"), Even->GetState().TickCount, int64(40));
	TestEqual(TEXT("One frame worth two reaches forty"), Choppy->GetState().TickCount, int64(40));

	TestEqual(
		TEXT("So the smooth machine and the even one hold the same world"),
		Smooth->ComputeStateHash(), Even->ComputeStateHash());
	TestEqual(
		TEXT("And so does the choppy one"),
		Choppy->ComputeStateHash(), Even->ComputeStateHash());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifSimClockCapsCatchUpTest,
	"Massif.Sim.Core.ClockCapsCatchUp",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifSimClockCapsCatchUpTest::RunTest(const FString& Parameters)
{
	// A frame that ran long owes steps, and running them all makes the next frame
	// run longer still. The cap breaks that spiral by giving up on real time it
	// cannot simulate — never by shortening a step, which would cost determinism
	// instead of wall time.
	FMassifSimClock Clock;
	const int32 Ticks = Clock.ConsumeTicks(60.0);

	TestEqual(TEXT("A minute-long hitch runs at most the cap"), Ticks, MassifMaxSimTicksPerFrame);
	TestEqual(
		TEXT("And the debt is dropped rather than carried into the next frame"),
		Clock.GetAccumulatorSeconds(), 0.0);

	// What the cap must not do is leave a fraction of a step behind, which would
	// make the next frame's step count depend on this frame's length.
	FMassifSimClock Steady;
	Steady.ConsumeTicks(MassifSimTickSeconds * 2.5);
	TestTrue(
		FString::Printf(TEXT("A normal frame carries under one step (%.4f s)"), Steady.GetAccumulatorSeconds()),
		Steady.GetAccumulatorSeconds() < MassifSimTickSeconds);
	return true;
}
