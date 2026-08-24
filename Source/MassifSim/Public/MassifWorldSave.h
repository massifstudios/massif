#pragma once

#include "CoreMinimal.h"
#include "MassifObjectStore.h"
#include "MassifSimState.h"

/**
 * What a save says about itself before any of it is read.
 *
 * Separated from the body so the load screen can list a directory of saves
 * without pulling a hundred megabytes of objects off disk for each one.
 */
struct MASSIFSIM_API FMassifWorldSaveHeader
{
	/** User-visible and shareable; the whole world derives from it (ARCHITECTURE §4). */
	int32 MasterSeed = 0;

	/**
	 * Which elevation model the world was built on, and what it hashed to.
	 *
	 * No heightfield is saved (ARCHITECTURE §9): the model is a build asset, and a
	 * save that names one which has since changed is refused rather than loading
	 * a resort onto different ground.
	 */
	FString ElevationModelName;
	uint64 ElevationModelHash = 0;

	FString EngineVersion;

	/** Wall clock at the moment of saving, for ordering the list. Not sim time. */
	FDateTime SavedAtUtc = FDateTime::MinValue();

	void Serialize(FArchive& Ar);
};

/**
 * A whole world on disk: what generation produced, and what play has done to it.
 *
 * The two halves are deliberately different in kind. The object store is bulk
 * immutable output that makes the save self-contained, so retuning the rockfall
 * or the flora table cannot silently rewrite a world someone is playing
 * (OBJECT_STORE.md §6). The sim state is small and mutable and carries the
 * overlay of everything the player has changed.
 *
 * Not here yet, and named so their absence is deliberate rather than forgotten:
 * the response table hash (ARCHITECTURE §5.6.6) and modified resident tiles
 * (§5.4). Neither exists to save.
 */
struct MASSIFSIM_API FMassifWorldSave
{
	FMassifWorldSaveHeader Header;
	FMassifSimState SimState;
	FMassifObjectStore ObjectStore;

	/** One layout for both directions, so reading and writing cannot drift apart. */
	void Serialize(FArchive& Ar);
};

/** Where saves live: Saved/SaveGames, one .massifsave each. */
MASSIFSIM_API FString MassifWorldSaveDirectory();
MASSIFSIM_API FString MassifWorldSavePath(const FString& SaveName);

/** Save names in the directory, without paths or extensions. */
MASSIFSIM_API TArray<FString> MassifListWorldSaves();

/** Non-const because one Serialize serves both directions; nothing here changes the save. */
MASSIFSIM_API bool MassifWriteWorldSave(
	const FString& FilePath, FMassifWorldSave& Save, FString& OutError);

MASSIFSIM_API bool MassifReadWorldSave(
	const FString& FilePath, FMassifWorldSave& OutSave, FString& OutError);

/** Everything before the body, for listing saves. */
MASSIFSIM_API bool MassifReadWorldSaveHeader(
	const FString& FilePath, FMassifWorldSaveHeader& OutHeader, FString& OutError);

/**
 * Whether the save can be loaded against the elevation model this build has.
 * False fills OutError with something the menu can show.
 */
MASSIFSIM_API bool MassifWorldSaveMatchesElevation(
	const FMassifWorldSaveHeader& Header, const FString& ModelName, uint64 ModelHash, FString& OutError);
