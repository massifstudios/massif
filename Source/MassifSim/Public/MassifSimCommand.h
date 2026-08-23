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
