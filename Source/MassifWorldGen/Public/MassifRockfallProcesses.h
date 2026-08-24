#pragma once

#include "CoreMinimal.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifRockfallParams.h"

/** What the cliffs shed, and where it ended up. */
namespace MassifEnvironmentChannels
{
	/** Loose blocks lying on the ground, in metres of debris, where the falling stopped. */
	MASSIFWORLDGEN_API const FName& RockfallDeposit();

	/** Mean radius of the blocks that stopped in a cell, in metres. Zero where none did. */
	MASSIFWORLDGEN_API const FName& RockfallBlockRadius();
}

/** The blocks themselves, one entry per boulder, in absolute world metres. */
namespace MassifEnvironmentPointSets
{
	MASSIFWORLDGEN_API const FName& RockPlacements();
}

namespace MassifEnvironmentProcesses
{
	MASSIFWORLDGEN_API const FName& Rockfall();
}

MASSIFWORLDGEN_API void MassifRegisterRockfallProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifRockfallParams& Params);
