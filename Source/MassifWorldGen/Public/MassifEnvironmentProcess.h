#pragma once

#include "CoreMinimal.h"
#include "MassifElevationSource.h"
#include "MassifEnvironmentChannel.h"
#include "MassifEnvironmentPointSet.h"

class FMassifChannelWriteScope;
class FMassifEnvironmentState;
class FMassifPointSetWriteScope;

/**
 * How faithfully a process models what it stands for, switchable at runtime
 * (ARCHITECTURE §5.6.3).
 *
 * `Off` is a level like any other, not an absence: a process at `Off` still
 * writes every channel it declares, filled with a constant or a trivial
 * analytic value. Wind off means uniform synoptic wind and unit exposure, not a
 * missing channel — so no code downstream ever has to ask whether a system is
 * enabled, which is what stops the graph rotting into scattered availability
 * checks as systems are added.
 */
enum class EMassifFidelity : uint8
{
	Off,

	/** One pass, local, no iteration. */
	Cheap,

	/** The intended model. */
	Standard,

	/** Slow and physical. What the cheaper levels are validated against. */
	Reference,
};

MASSIFWORLDGEN_API const TCHAR* MassifFidelityName(EMassifFidelity Fidelity);

/** What a process consumes, what it produces, and how well it can produce it. */
struct FMassifProcessDeclaration
{
	/** Dotted and stable, e.g. "Wind.TerrainExposure". Derives the process's random streams. */
	FName Name;

	/**
	 * What to call this in front of a player, e.g. "Wind Simulation".
	 *
	 * Declared here rather than looked up by a settings screen, for the same reason
	 * a channel carries its own unit and description: a system added to the graph
	 * has to appear everywhere that lists systems without anything downstream being
	 * taught its name (ARCHITECTURE §5.7). Empty falls back to the process name.
	 */
	FString DisplayName;

	/** Channels read. Reading anything else is rejected, so the graph's edges are the real ones. */
	TArray<FName> InputChannels;

	/**
	 * Channels read where something produces them, and absent otherwise.
	 *
	 * This is what lets a system couple back to one downstream of it without
	 * making that system compulsory: wind is slowed by a canopy when vegetation is
	 * in the graph, and is simply the wind the terrain shaped when it is not. The
	 * scheduler adds the dependency edge only where a producer exists, so the two
	 * arrangements are both valid graphs rather than one being a broken version of
	 * the other.
	 */
	TArray<FName> OptionalInputChannels;

	/** Channels written. Registered with the state when the schedule is built. */
	TArray<FMassifChannelDescriptor> OutputChannels;

	/**
	 * Discrete objects written, for what a grid cannot hold.
	 *
	 * A second kind of output rather than a channel encoding: two boulders in one
	 * cell are two boulders, and the one that rolled further is not an average of
	 * the pair. Nothing reads a point set as a process input — the grid reduction
	 * beside it is what downstream systems coupled to, so the discrete set stays
	 * a leaf of the graph and cannot join a relaxation group.
	 */
	TArray<FMassifPointSetDescriptor> OutputPointSets;

	/** Levels implemented beyond `Off`, which every process supports by definition. */
	TArray<EMassifFidelity> SupportedFidelities;

	/**
	 * Set where the process reads a channel it also writes, directly or around
	 * through others. The scheduler puts such processes in a relaxation group
	 * instead of failing to order them.
	 */
	bool bParticipatesInFeedback = false;

	bool SupportsFidelity(EMassifFidelity Fidelity) const;
};

/**
 * Everything a process is allowed to touch while it executes.
 *
 * The restriction is the point. A process reaches no global, no elevation model
 * beyond the interface, and no other process — so independent branches of the
 * graph can run at the same time, and the determinism guarantee is checkable
 * rather than hoped for. Inputs and outputs resolve only to channels the
 * process declared, so the dependency graph the scheduler ordered is the same
 * one the code actually walks.
 */
class MASSIFWORLDGEN_API FMassifEnvironmentContext
{
public:
	FMassifEnvironmentContext(
		FMassifEnvironmentState& InState,
		const IMassifElevationSource& InElevation,
		const FMassifGeoreference& InGeoreference,
		uint32 InMasterSeed,
		const FMassifProcessDeclaration& InDeclaration);

	FMassifEnvironmentContext(const FMassifEnvironmentContext&) = delete;
	FMassifEnvironmentContext& operator=(const FMassifEnvironmentContext&) = delete;

	const IMassifElevationSource& GetElevation() const
	{
		return Elevation;
	}

	const FMassifGeoreference& GetGeoreference() const
	{
		return Georeference;
	}

	/** Reads. Writes go through OpenOutput, so a process cannot overwrite what it only reads. */
	const FMassifEnvironmentState& GetState() const
	{
		return State;
	}

	/** Handle of a declared input. An undeclared name is a failure, not an empty handle. */
	FMassifChannelHandle GetInput(FName ChannelName) const;

	/** Handle of a declared optional input, invalid where nothing in this graph produces it. */
	FMassifChannelHandle FindOptionalInput(FName ChannelName) const;

	/** Handle of a declared output, for reading back what this process has already written. */
	FMassifChannelHandle GetOutput(FName ChannelName) const;

	/** Float scratch for one slice of a declared output. Quantised into the state on commit. */
	TUniquePtr<FMassifChannelWriteScope> OpenOutput(FName ChannelName, int32 TimeSlice = 0) const;

	/** Slot scratch for a declared point-set output. Bucketed into the state on commit. */
	TUniquePtr<FMassifPointSetWriteScope> OpenPointSetOutput(FName PointSetName) const;

	/**
	 * A stream derived from the master seed, the process name and the purpose, so
	 * adding a process cannot shift the sequence another one draws.
	 */
	FRandomStream MakeStream(const TCHAR* Purpose) const;
	uint32 MakeSeed(const TCHAR* Purpose) const;

private:
	FString MakeStreamName(const TCHAR* Purpose) const;

	FMassifEnvironmentState& State;
	const IMassifElevationSource& Elevation;
	const FMassifGeoreference& Georeference;
	const FMassifProcessDeclaration& Declaration;
	uint32 MasterSeed = 0;
};

/** Writes one value over every cell of one slice of a declared output. */
MASSIFWORLDGEN_API void MassifFillChannelSlice(
	const FMassifEnvironmentContext& Context, FName ChannelName, int32 TimeSlice, double Value);

/**
 * One simulated system in the environment graph (ARCHITECTURE §5.6.3).
 *
 * Adding a system to the environment is one implementation of this registered
 * with the scheduler: no enum to extend, no pipeline to edit, no debug view to
 * teach about it, and no change anywhere downstream.
 *
 * Implementations must be stateless and thread-safe. Everything a run needs
 * comes in through the context and everything it produces goes out through the
 * context's channels, which is what lets the scheduler run independent
 * branches at once and re-run a single process when its parameters change.
 */
class MASSIFWORLDGEN_API IMassifEnvironmentProcess
{
public:
	virtual ~IMassifEnvironmentProcess() = default;

	virtual FMassifProcessDeclaration Declare() const = 0;

	/** The reference implementation, what the automation suite runs, and the headless path. */
	virtual void Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const = 0;

	/**
	 * The same algorithm arranged for the hardware. Returns false where no compute
	 * path exists or none can run, and the scheduler falls back to Execute.
	 */
	virtual bool ExecuteOnGpu(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const;
};
