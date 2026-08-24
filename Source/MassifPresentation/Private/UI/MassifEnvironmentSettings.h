#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "MassifEnvironmentSettings.generated.h"

class FMassifEnvironmentScheduler;

/**
 * The fidelity each environment system is set to, kept across runs.
 *
 * A per-install setting rather than part of a save: the environment is a pure
 * function of the seed and is rebuilt at load (ARCHITECTURE §5.6), so a save
 * carries no channels and reopens at whatever fidelity the machine is set to.
 * A world therefore differs between two machines set differently, which is the
 * point of the setting and worth knowing before comparing screenshots.
 *
 * Levels are stored as their player-facing names rather than as an enum, so the
 * sim keeps no reflection markup and an ini stays readable.
 */
UCLASS(config = MassifEnvironment)
class UMassifEnvironmentSettings : public UObject
{
	GENERATED_BODY()

public:
	/** Level per process name, e.g. "Wind" to "High". A process not listed keeps the graph's default. */
	UPROPERTY(config)
	TMap<FName, FString> ProcessFidelities;

	static UMassifEnvironmentSettings& Get();
};

namespace MassifEnvironmentSettings
{
	/**
	 * Applies the saved levels to a scheduler whose schedule is built.
	 *
	 * Call before the first solve: a level set afterwards dirties the graph and
	 * solves the whole thing a second time. Returns how many processes moved.
	 */
	int32 ApplySaved(FMassifEnvironmentScheduler& Scheduler);

	/**
	 * The saved levels as plain data, for a build that runs off the game thread.
	 *
	 * The settings live on a UObject, so they are read here and carried across
	 * rather than reached for from a worker.
	 */
	TMap<FName, FString> ReadSaved();

	int32 Apply(FMassifEnvironmentScheduler& Scheduler, const TMap<FName, FString>& Fidelities);

	/** Writes every process's current level. */
	void Save(const FMassifEnvironmentScheduler& Scheduler);
}
