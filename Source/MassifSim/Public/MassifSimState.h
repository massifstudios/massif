#pragma once

#include "CoreMinimal.h"
#include "MassifSimCommand.h"
#include "Math/RandomStream.h"
#include "MassifSimState.generated.h"

/**
 * The channel the sim's own stream is derived from. Adding another channel
 * beside it never shifts this one (ARCHITECTURE §4).
 */
#define MASSIF_SIM_CORE_SEED_CHANNEL TEXT("Sim.Core")

/**
 * Everything the simulation is, as plain data.
 *
 * One root struct rather than state spread across actors and components: it is
 * what gets hashed for the determinism contract, what gets written to a save,
 * and what a server would snapshot. None of the three is possible if the truth
 * lives in a dozen UObjects.
 *
 * Random streams are owned here and serialized with it, so a loaded game resumes
 * the sequence it was in rather than restarting it.
 */
USTRUCT()
struct MASSIFSIM_API FMassifSimState
{
	GENERATED_BODY()

	/** Ticks advanced since the world began, and the sim's whole notion of time. */
	UPROPERTY()
	int64 TickCount = 0;

	/** User-visible and shareable; every derived stream hangs off it. */
	UPROPERTY()
	int32 MasterSeed = 0;

	UPROPERTY()
	FRandomStream RandomStream;

	/** What the placeholder command writes. Goes when the first real system lands. */
	UPROPERTY()
	int64 CommandCounter = 0;
};

/** Puts the state back to the start of a new game on the given seed. */
MASSIFSIM_API void MassifResetSimState(FMassifSimState& State, int32 MasterSeed);

/** Folds one command into the state. Called only from the start of a sim tick. */
MASSIFSIM_API void MassifApplySimCommand(FMassifSimState& State, const FMassifSimCommand& Command);

/**
 * Advances the simulation by exactly one fixed step.
 *
 * Commands land first and whole, so every one of them is visible to the step
 * that follows and none of them is half-applied when it runs. There is no delta
 * parameter and there will not be one: a step is always the same length of
 * simulated time, and game speed changes how many steps run per real second
 * rather than how long a step is (ARCHITECTURE §3).
 */
MASSIFSIM_API void MassifAdvanceSimTick(
	FMassifSimState& State, TConstArrayView<FMassifSimCommand> Commands);

/**
 * Hash over the whole state, for the determinism contract (ARCHITECTURE §4).
 *
 * Every field is folded in explicitly rather than hashing the struct's bytes,
 * because padding between members is uninitialised and would make the hash
 * differ between two runs that simulated identically.
 */
MASSIFSIM_API uint64 MassifComputeSimStateHash(const FMassifSimState& State);
