#pragma once

#include "CoreMinimal.h"
#include "MassifEnvironmentProcess.h"

/**
 * What the fidelity levels are called in front of a player.
 *
 * Separate from MassifFidelityName, which spells the levels the way the graph
 * and the console commands do. The internal names say what a level costs to the
 * person writing the process; these say where it sits on a ladder to the person
 * choosing one, and the two should be free to disagree.
 */
namespace MassifFidelityLabels
{
	/** Off, Low, Medium, High. */
	const TCHAR* Label(EMassifFidelity Fidelity);

	/** Unset where the text names no level. Matches the labels, not the internal names. */
	TOptional<EMassifFidelity> Parse(const FString& Text);

	/** Every level, lowest first, which is also the order a row of buttons is drawn in. */
	TArrayView<const EMassifFidelity> AllLevels();
}
