#pragma once

#include "CoreMinimal.h"
#include "MassifElevationSource.h"
#include "MassifEnvironmentBakeParams.h"
#include "MassifSimState.h"
#include "HAL/CriticalSection.h"

class FMassifEnvironmentScheduler;
class FMassifEnvironmentState;
class FMassifHabitatField;
class FMassifObjectStore;
class FMassifRiverField;
class FMassifRiverNetwork;
class FMassifTerrainField;

/**
 * The steps a new world is built in, in the order they run.
 *
 * Named rather than numbered because the point of them is to be read: a screen
 * that says "solving the environment" for twenty seconds is telling the truth,
 * and a bar with no name on it is not.
 */
enum class EMassifWorldBuildStage : uint8
{
	Elevation,
	Save,
	Environment,
	Rivers,
	Terrain,
	Objects,
	Spawning,
	Complete,
	Failed,
};

struct FMassifWorldBuildProgressSnapshot
{
	EMassifWorldBuildStage Stage = EMassifWorldBuildStage::Elevation;

	/** What inside the stage is running, where the stage has parts worth naming. */
	FString Detail;

	/** How far through this stage alone, 0 to 1. Zero where the stage cannot say. */
	float StageFraction = 0.0f;
};

/**
 * Written by the build thread, read by the loading screen every frame.
 *
 * A lock rather than atomics because the detail string travels with the stage,
 * and a screen that showed one stage's name beside another's detail would be
 * worse than one that showed nothing.
 */
class FMassifWorldBuildProgress
{
public:
	void Report(EMassifWorldBuildStage Stage, FString Detail, float StageFraction);

	/** Entering a stage that has nothing finer to say. */
	void ReportStage(EMassifWorldBuildStage Stage);

	FMassifWorldBuildProgressSnapshot Read() const;

private:
	mutable FCriticalSection Mutex;
	FMassifWorldBuildProgressSnapshot Snapshot;
};

/** Everything the build needs, resolved on the game thread before it starts. */
struct FMassifWorldBuildInputs
{
	FString HeightmapPath;

	/** The model's name without path or suffix, which is what a save records. */
	FString ElevationModelName;

	uint32 MasterSeed = 0;
	FMassifEnvironmentBakeParams Params;
	FMassifGeoreference Georeference;

	/** Read off the settings UObject here, so the worker never touches one. */
	TMap<FName, FString> SavedFidelities;

	/**
	 * A save to load, or empty for a new game.
	 *
	 * Loading is the same build with its expensive steps read rather than run: the
	 * seed, the solved environment and the objects all come off the save. What is
	 * cheap to derive from them — the rivers, the terrain field, the habitat field
	 * — is derived, which is why a save carries no copy of those (ARCHITECTURE §9).
	 */
	FString SaveName;

	/**
	 * Solves the environment even where the save carries one.
	 *
	 * For tuning: a bake is what the graph produced when the save was written, so
	 * with it in place a change to a process or a parameter does nothing to a
	 * loaded world until this is set.
	 */
	bool bRebuildEnvironment = false;
};

/** Everything it produced, handed back to the game thread to spawn a world from. */
struct FMassifWorldBuildResult
{
	TSharedPtr<FMassifEnvironmentScheduler> EnvironmentScheduler;
	TSharedPtr<const FMassifRiverNetwork> RiverNetwork;
	TSharedPtr<const FMassifRiverField> RiverField;
	TSharedPtr<const FMassifTerrainField> TerrainField;
	TSharedPtr<const FMassifHabitatField> HabitatField;
	TSharedPtr<const FMassifObjectStore> ObjectStore;

	/** The station carrying the most water, which is where the water body is baked. */
	FVector2d BusiestNodeMeters = FVector2d::ZeroVector;
	bool bBusiestNodeFound = false;

	/** What the seed turned out to be: the command line's on a new game, the save's on a load. */
	uint32 MasterSeed = 0;

	/** Identifies the ground, so a save written from this world can be refused against other ground. */
	FString ElevationModelName;
	uint64 ElevationModelHash = 0;

	/** Set only on a load. The world the player left, to be put back on the subsystem. */
	TOptional<FMassifSimState> LoadedSimState;

	/** Read off a save and consumed by the solve stage; null once it has been taken. */
	TSharedPtr<FMassifEnvironmentState> SavedEnvironment;

	/** Whether the environment came off the save rather than out of the graph. */
	bool bEnvironmentFromSave = false;

	double EnvironmentSeconds = 0.0;
	double ObjectStoreSeconds = 0.0;

	/** Zero on a load: the trees came out of the save rather than being planted. */
	int32 PlantedTreeCount = 0;

	/** Empty on success; what went wrong otherwise, already logged. */
	FString Error;

	bool Succeeded() const
	{
		return TerrainField.IsValid() && Error.IsEmpty();
	}
};

namespace MassifWorldBuild
{
	/**
	 * Solves a world. Runs off the game thread.
	 *
	 * Everything here is plain C++ over the elevation model — no UObjects, no
	 * actors, no rendering. That is what makes it safe to run on a worker, and it
	 * stops being true the moment an environment process takes a GPU path
	 * (`IMassifEnvironmentProcess::ExecuteOnGpu`), which none does today.
	 */
	FMassifWorldBuildResult Run(const FMassifWorldBuildInputs& Inputs, FMassifWorldBuildProgress& Progress);
}
