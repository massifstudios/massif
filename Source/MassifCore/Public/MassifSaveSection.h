#pragma once

#include "CoreMinimal.h"

/**
 * A block of a save whose contents the save format does not know.
 *
 * The world save lives in MassifSim and the solved environment in
 * MassifWorldGen, and neither module depends on the other. A section lets the
 * save carry one without naming its type: whoever owns the type supplies the
 * section. The block is written behind its own length, so a reader with no
 * section for it — or one that refuses what it finds — steps over the block
 * rather than reading the rest of the file out of the middle of it.
 */
class IMassifSaveSection
{
public:
	virtual ~IMassifSaveSection() = default;

	/** False fills OutError and leaves the stream anywhere inside the block. */
	virtual bool Serialize(FArchive& Ar, FString& OutError) = 0;
};
