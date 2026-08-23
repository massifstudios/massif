#pragma once

#include "CoreMinimal.h"
#include "MassifSimCommand.generated.h"

/**
 * What a command asks the simulation to do.
 *
 * Tagged data rather than a class hierarchy, because a command has to survive a
 * round trip through a save file and, later, through the wire: commands are the
 * replication boundary (ARCHITECTURE §3), and a polymorphic type is not plain
 * serializable data.
 */
UENUM()
enum class EMassifSimCommandType : uint8
{
	None,

	/**
	 * Adds its argument to the state's counter.
	 *
	 * A placeholder, and the only command until lift and slope placement land.
	 * It exists so the queue has something to carry that the state hash can see,
	 * which is what makes "applied on a tick boundary, never mid-frame" a
	 * property a test can hold rather than an intention.
	 */
	AddToCounter,

	/**
	 * Moves the wall clock to a minute of the day, given in `Argument`.
	 *
	 * A command rather than a setter, because the time of day is sim state: every
	 * system that reads the clock — fog now, melt and lift hours later — has to see
	 * it move on a tick boundary along with everything else, or two of them
	 * disagree about what hour it is within one frame.
	 *
	 * It moves the clock by an offset rather than the tick count, so the tick count
	 * stays what it is: it is the sim's measure of how much has happened, and
	 * winding the sun back should not unhappen any of it.
	 */
	SetTimeOfDayMinutes,
};

/**
 * One player-initiated mutation, enqueued by presentation and applied at the
 * start of a sim tick.
 *
 * Presentation never writes sim state directly. Everything it wants to change
 * arrives here, which is what keeps the two sides separable and what a future
 * client would send to a server instead of applying locally.
 */
USTRUCT()
struct MASSIFSIM_API FMassifSimCommand
{
	GENERATED_BODY()

	UPROPERTY()
	EMassifSimCommandType Type = EMassifSimCommandType::None;

	UPROPERTY()
	int32 Argument = 0;

	FMassifSimCommand() = default;

	FMassifSimCommand(EMassifSimCommandType InType, int32 InArgument)
		: Type(InType)
		, Argument(InArgument)
	{
	}
};
