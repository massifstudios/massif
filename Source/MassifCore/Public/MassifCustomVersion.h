#pragma once

#include "CoreMinimal.h"

/**
 * Serialization version for everything Massif writes to disk, per
 * ARCHITECTURE §9. One GUID for the whole project rather than one per system:
 * a save is loaded as a unit, so there is no case where the object store is at
 * one version and the sim state at another.
 *
 * Every format change appends an entry. During development an incompatible
 * save is invalidated rather than migrated, unless migration is asked for.
 */
struct MASSIFCORE_API FMassifCustomVersion
{
	enum Type
	{
		BeforeCustomVersionWasAdded = 0,

		/** Chunked object store of 24-byte records (OBJECT_STORE.md §3, §6). */
		ObjectStore,

		/** Solved environment channels carried in the save rather than re-solved (ARCHITECTURE §9). */
		BakedEnvironment,

		VersionPlusOne,
		LatestVersion = VersionPlusOne - 1
	};

	static const FGuid GUID;

private:
	FMassifCustomVersion() = default;
};
