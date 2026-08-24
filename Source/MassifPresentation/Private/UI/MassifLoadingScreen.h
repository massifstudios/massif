#pragma once

#include "CoreMinimal.h"

class FMassifWorldBuildProgress;
class UWorld;

/**
 * Covers the viewport while a world is generated, reading the build's progress.
 *
 * Up from the moment the menu hands off until the world is spawned, so there is no
 * frame where a run shows an empty level and no explanation.
 */
namespace MassifLoadingScreen
{
	void Open(UWorld* World, TSharedRef<FMassifWorldBuildProgress> Progress);

	void Close();

	bool IsOpen();
}
