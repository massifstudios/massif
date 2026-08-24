#pragma once

#include "CoreMinimal.h"
#include "MassifSimClock.h"
#include "MassifSimCommand.h"
#include "MassifSimState.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "MassifSimSubsystem.generated.h"

/**
 * Where the simulation lives (ARCHITECTURE §3).
 *
 * Holds the one root state struct, the command queue feeding it, and the clock
 * that decides how many fixed steps a frame is worth. Nothing else owns sim
 * state: no actor, no component, no widget, no material.
 *
 * Presentation reads the state freely and never writes it. Everything it wants
 * changed goes through EnqueueCommand and lands at the start of a tick, which is
 * what keeps the two sides separable and what a client would send to a server
 * instead of applying locally.
 */
UCLASS()
class MASSIFSIM_API UMassifSimSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	/** Resets to tick zero on the given seed and starts advancing. */
	void StartNewGame(int32 MasterSeed);

	/**
	 * Puts a loaded world back and starts advancing from where it stopped.
	 *
	 * Queued commands are dropped rather than carried: they were issued against the
	 * session that saved, and applying them to a restored state would advance it past
	 * what the save says it is.
	 */
	void RestoreGame(FMassifSimState LoadedState);

	/** Queued now, applied at the start of the next sim tick. Never immediately. */
	void EnqueueCommand(const FMassifSimCommand& Command);

	const FMassifSimState& GetState() const
	{
		return State;
	}

	uint64 ComputeStateHash() const
	{
		return MassifComputeSimStateHash(State);
	}

	/** Winds the wall clock to a minute of the day. Queued like any other mutation. */
	void SetTimeOfDayMinutes(int32 MinuteOfDay);

	/** Where the clock stands right now, offset included. */
	FMassifWallClock GetWallClock() const
	{
		return MassifWallClockOf(State);
	}

	void SetGameSpeed(EMassifGameSpeed Speed);

	EMassifGameSpeed GetGameSpeed() const
	{
		return Clock.Speed;
	}

	/** Commands waiting for the next tick. Nothing has seen them yet. */
	int32 GetPendingCommandCount() const
	{
		return PendingCommands.Num();
	}

	/**
	 * Runs however many whole sim ticks this frame's real time is worth.
	 *
	 * Separate from Tick so the simulation can be driven without an engine frame,
	 * which is what lets the headless suite advance it a known number of steps.
	 */
	void AdvanceFrame(double DeltaSeconds);

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/**
	 * A subsystem with no game instance behind it has no frames to run against,
	 * which is also what keeps one built for a test out of the engine's tick.
	 */
	virtual bool IsTickable() const override
	{
		return bSimulationStarted && GetTickableGameObjectWorld() != nullptr;
	}

	virtual UWorld* GetTickableGameObjectWorld() const override;

private:
	/**
	 * One fixed step: everything queued is applied, then the state advances.
	 *
	 * The queue is taken whole and cleared before anything runs, so a command
	 * enqueued while this tick is running waits for the next one instead of being
	 * seen halfway through this one.
	 */
	void AdvanceSimTick();

	FMassifSimState State;
	FMassifSimClock Clock;
	TArray<FMassifSimCommand> PendingCommands;

	bool bSimulationStarted = false;
};
