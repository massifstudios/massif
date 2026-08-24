#include "MassifWorldSave.h"

#include "HAL/FileManager.h"
#include "MassifCustomVersion.h"
#include "Misc/Paths.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "UObject/Class.h"

namespace
{
	/** First four bytes of the file, so opening something else fails as that rather than as garbage. */
	constexpr uint32 WorldSaveMagic = 0x4653414D;  // 'MASF'

	constexpr const TCHAR* WorldSaveExtension = TEXT(".massifsave");

	/**
	 * The version is written out rather than left to the archive's custom version
	 * container: that container is serialized by package archives, and a save is
	 * a plain file. Setting it on load is what lets everything below query the
	 * version the ordinary way.
	 */
	bool SerializeFormatVersion(FArchive& Ar, FString& OutError)
	{
		uint32 Magic = WorldSaveMagic;
		Ar << Magic;
		if (Magic != WorldSaveMagic)
		{
			OutError = TEXT("Not a Massif save: the file does not start with one.");
			return false;
		}

		int32 FormatVersion = FMassifCustomVersion::LatestVersion;
		Ar << FormatVersion;
		if (FormatVersion > FMassifCustomVersion::LatestVersion)
		{
			OutError = FString::Printf(
				TEXT("Save is format %d and this build reads up to %d."),
				FormatVersion, static_cast<int32>(FMassifCustomVersion::LatestVersion));
			return false;
		}

		Ar.SetCustomVersion(FMassifCustomVersion::GUID, FormatVersion, TEXT("MassifVersion"));
		return true;
	}

	/** A file archive set up the same way for reading and writing, or the two disagree about layout. */
	void ConfigureSaveArchive(FArchive& Ar)
	{
		Ar.SetIsPersistent(true);
		Ar.ArNoDelta = true;
	}

	TUniquePtr<FArchive> OpenSaveReader(const FString& FilePath, FString& OutError)
	{
		TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*FilePath));
		if (!Reader.IsValid())
		{
			OutError = FString::Printf(TEXT("Cannot open '%s'."), *FilePath);
			return nullptr;
		}
		ConfigureSaveArchive(*Reader);
		return Reader;
	}
}

void FMassifWorldSaveHeader::Serialize(FArchive& Ar)
{
	Ar << MasterSeed;
	Ar << ElevationModelName;
	Ar << ElevationModelHash;
	Ar << EngineVersion;
	Ar << SavedAtUtc;
}

void FMassifWorldSave::Serialize(FArchive& Ar)
{
	Header.Serialize(Ar);

	// Through the struct's own reflection rather than field by field: the sim
	// state is one root struct that will grow, and a save that has to be edited
	// alongside every new member is a save that will one day not be.
	//
	// The proxy is not optional. Reflected serialization tags every property with
	// its FName, and a plain file archive drops names on the floor, which desyncs
	// the stream rather than failing.
	FObjectAndNameAsStringProxyArchive NamedAr(Ar, false);
	NamedAr.ArNoDelta = true;
	FMassifSimState::StaticStruct()->SerializeItem(NamedAr, &SimState, nullptr);
	if (Ar.IsLoading())
	{
		SimState.ObjectOverlay.RebuildChunkCounts();
	}

	ObjectStore.Serialize(Ar);
}

FString MassifWorldSaveDirectory()
{
	return FPaths::ProjectSavedDir() / TEXT("SaveGames");
}

FString MassifWorldSavePath(const FString& SaveName)
{
	return MassifWorldSaveDirectory() / SaveName + WorldSaveExtension;
}

TArray<FString> MassifListWorldSaves()
{
	TArray<FString> FileNames;
	IFileManager::Get().FindFiles(FileNames, *(MassifWorldSaveDirectory() / TEXT("*")), true, false);

	TArray<FString> SaveNames;
	for (const FString& FileName : FileNames)
	{
		if (FPaths::GetExtension(FileName, true) == WorldSaveExtension)
		{
			SaveNames.Add(FPaths::GetBaseFilename(FileName));
		}
	}
	SaveNames.Sort();
	return SaveNames;
}

bool MassifWriteWorldSave(const FString& FilePath, FMassifWorldSave& Save, FString& OutError)
{
	TUniquePtr<FArchive> Writer(IFileManager::Get().CreateFileWriter(*FilePath));
	if (!Writer.IsValid())
	{
		OutError = FString::Printf(TEXT("Cannot write '%s'."), *FilePath);
		return false;
	}
	ConfigureSaveArchive(*Writer);

	if (!SerializeFormatVersion(*Writer, OutError))
	{
		return false;
	}

	Save.Serialize(*Writer);

	Writer->Close();
	if (!Writer->IsError())
	{
		return true;
	}

	OutError = FString::Printf(TEXT("Failed while writing '%s'."), *FilePath);
	return false;
}

bool MassifReadWorldSaveHeader(const FString& FilePath, FMassifWorldSaveHeader& OutHeader, FString& OutError)
{
	TUniquePtr<FArchive> Reader = OpenSaveReader(FilePath, OutError);
	if (!Reader.IsValid() || !SerializeFormatVersion(*Reader, OutError))
	{
		return false;
	}

	OutHeader.Serialize(*Reader);
	return !Reader->IsError();
}

bool MassifReadWorldSave(const FString& FilePath, FMassifWorldSave& OutSave, FString& OutError)
{
	TUniquePtr<FArchive> Reader = OpenSaveReader(FilePath, OutError);
	if (!Reader.IsValid() || !SerializeFormatVersion(*Reader, OutError))
	{
		return false;
	}

	OutSave.Serialize(*Reader);
	if (!Reader->IsError())
	{
		return true;
	}

	OutError = FString::Printf(TEXT("Failed while reading '%s'."), *FilePath);
	return false;
}

bool MassifWorldSaveMatchesElevation(
	const FMassifWorldSaveHeader& Header, const FString& ModelName, uint64 ModelHash, FString& OutError)
{
	if (Header.ElevationModelName != ModelName)
	{
		OutError = FString::Printf(
			TEXT("Save was built on elevation model '%s', not '%s'."), *Header.ElevationModelName, *ModelName);
		return false;
	}

	if (Header.ElevationModelHash != ModelHash)
	{
		OutError = FString::Printf(
			TEXT("Elevation model '%s' has changed since the save was made; the resort would sit on ")
			TEXT("different ground."), *ModelName);
		return false;
	}
	return true;
}
