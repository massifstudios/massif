#include "MassifWorldBuild.h"

#include "MassifEnvironmentGraph.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifEnvironmentState.h"
#include "MassifHabitat.h"
#include "MassifHeightmap.h"
#include "MassifObjectStore.h"
#include "MassifObjectStoreBuilder.h"
#include "MassifObjectTypeRegistry.h"
#include "MassifRiverField.h"
#include "MassifRiverNetwork.h"
#include "MassifTerrainField.h"
#include "MassifWorldSave.h"
#include "UI/MassifEnvironmentSettings.h"

namespace
{
	/** The station carrying the most water in one reach, which is where its channel is widest. */
	const FMassifRiverNode* BusiestNodeOfReach(const FMassifRiverReach& Reach)
	{
		const FMassifRiverNode* Busiest = nullptr;
		for (const FMassifRiverNode& Node : Reach.Nodes)
		{
			if (Busiest != nullptr
				&& Node.DischargeCubicMetersPerSecond <= Busiest->DischargeCubicMetersPerSecond)
			{
				continue;
			}
			Busiest = &Node;
		}
		return Busiest;
	}

	const FMassifRiverNode* BusiestNode(const FMassifRiverNetwork& Rivers)
	{
		const FMassifRiverNode* Busiest = nullptr;
		for (const FMassifRiverReach& Reach : Rivers.GetReaches())
		{
			const FMassifRiverNode* Node = BusiestNodeOfReach(Reach);
			if (Node == nullptr)
			{
				continue;
			}
			if (Busiest != nullptr
				&& Node->DischargeCubicMetersPerSecond <= Busiest->DischargeCubicMetersPerSecond)
			{
				continue;
			}
			Busiest = Node;
		}
		return Busiest;
	}

	void RecordBusiestNode(const FMassifRiverNetwork& Rivers, FMassifWorldBuildResult& OutResult)
	{
		const FMassifRiverNode* Node = BusiestNode(Rivers);
		if (Node == nullptr)
		{
			return;
		}
		OutResult.BusiestNodeMeters = Node->PositionMeters;
		OutResult.bBusiestNodeFound = true;
	}

	TSharedPtr<const FMassifHeightmap> LoadElevationModel(
		const FMassifWorldBuildInputs& Inputs, FMassifWorldBuildProgress& Progress,
		FMassifWorldBuildResult& OutResult)
	{
		Progress.Report(EMassifWorldBuildStage::Elevation,
			FPaths::GetBaseFilename(Inputs.HeightmapPath), 0.0f);

		const double StartSeconds = FPlatformTime::Seconds();
		FString LoadError;
		TSharedPtr<const FMassifHeightmap> Heightmap =
			FMassifHeightmap::LoadFromFile(Inputs.HeightmapPath, LoadError);
		if (!Heightmap.IsValid())
		{
			OutResult.Error = FString::Printf(
				TEXT("%s. Fetch one with Tools/fetch_tirol_dem.py."), *LoadError);
			return nullptr;
		}

		UE_LOG(LogTemp, Display, TEXT("Terrain: %s, altitude %.0f-%.0f m, %d mips, loaded in %.2f s"),
			*Heightmap->GetSourceInfo().Label, Heightmap->GetMinAltitudeMeters(),
			Heightmap->GetMaxAltitudeMeters(), Heightmap->GetMipCount(),
			FPlatformTime::Seconds() - StartSeconds);
		return Heightmap;
	}

	/** Names the process the solve is inside, so the longest stage says what it is doing. */
	FMassifSolveProgressReporter MakeSolveReporter(
		const FMassifEnvironmentScheduler& Scheduler, FMassifWorldBuildProgress& Progress)
	{
		return [&Scheduler, &Progress](FName ProcessName, int32 CompletedRunCount, int32 TotalRunCount)
		{
			float Fraction = 0.0f;
			if (TotalRunCount > 0)
			{
				Fraction = static_cast<float>(CompletedRunCount) / static_cast<float>(TotalRunCount);
			}
			Progress.Report(
				EMassifWorldBuildStage::Environment, Scheduler.GetDisplayName(ProcessName), Fraction);
		};
	}

	bool SolveEnvironment(
		TSharedRef<const FMassifHeightmap> Heightmap, const FMassifWorldBuildInputs& Inputs,
		FMassifWorldBuildProgress& Progress, FMassifWorldBuildResult& OutResult)
	{
		Progress.ReportStage(EMassifWorldBuildStage::Environment);

		const double StartSeconds = FPlatformTime::Seconds();
		FString BuildError;
		TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MassifPrepareEnvironment(
			MakeShared<const FMassifHeightmapElevationSource>(MoveTemp(Heightmap)),
			Inputs.Georeference, OutResult.MasterSeed, Inputs.Params, BuildError);
		if (!Scheduler.IsValid())
		{
			OutResult.Error = FString::Printf(
				TEXT("%s. Nothing downstream of the solve is built."), *BuildError);
			return false;
		}

		// Before the first solve, so a world set to something other than the defaults
		// is solved once at what it is set to rather than twice.
		MassifEnvironmentSettings::Apply(*Scheduler, Inputs.SavedFidelities);
		Scheduler->SetProgressReporter(MakeSolveReporter(*Scheduler, Progress));
		Scheduler->Execute();
		Scheduler->SetProgressReporter(nullptr);

		OutResult.EnvironmentScheduler = MakeShareable(Scheduler.Release());
		OutResult.EnvironmentSeconds = FPlatformTime::Seconds() - StartSeconds;
		return true;
	}

	void TraceRivers(
		const FMassifWorldBuildInputs& Inputs, FMassifWorldBuildProgress& Progress,
		FMassifWorldBuildResult& OutResult)
	{
		Progress.ReportStage(EMassifWorldBuildStage::Rivers);

		const FMassifEnvironmentScheduler& Scheduler = *OutResult.EnvironmentScheduler;
		const TSharedRef<const FMassifRiverNetwork> Rivers = MassifExtractRivers(
			Scheduler.GetState(), Scheduler.GetElevation(), Inputs.Params.Rivers);

		OutResult.RiverNetwork = Rivers;
		OutResult.RiverField = FMassifRiverField::Build(Rivers.Get(), MassifDefaultBankReachMeters);
		RecordBusiestNode(Rivers.Get(), OutResult);
	}

	void CutTerrainField(
		TSharedRef<const FMassifHeightmap> Heightmap, FMassifWorldBuildProgress& Progress,
		FMassifWorldBuildResult& OutResult)
	{
		Progress.ReportStage(EMassifWorldBuildStage::Terrain);

		OutResult.TerrainField = MakeShared<const FMassifTerrainField>(
			MoveTemp(Heightmap), OutResult.RiverField.ToSharedRef());

		// One habitat field for everything that reads the world: the terrain's
		// textures, the plants standing on them and the debug views all have to
		// answer the same about a given point, and they do so by construction when
		// they read the same field.
		OutResult.HabitatField = MakeShared<const FMassifHabitatField>(
			OutResult.TerrainField.ToSharedRef(),
			OutResult.EnvironmentScheduler->GetSharedState(),
			OutResult.MasterSeed);
	}

	void FillObjectStore(FMassifWorldBuildProgress& Progress, FMassifWorldBuildResult& OutResult)
	{
		// A load already has its objects: they are what the save is mostly made of, and
		// regenerating them would put the world back under whatever the generator does
		// today rather than what it did when the save was written.
		if (OutResult.ObjectStore.IsValid())
		{
			return;
		}

		Progress.ReportStage(EMassifWorldBuildStage::Objects);

		const double StartSeconds = FPlatformTime::Seconds();
		OutResult.ObjectStore = MakeShared<const FMassifObjectStore>(
			MassifObjectStoreBuilder::BuildFromEnvironment(
				OutResult.EnvironmentScheduler->GetState(), FMassifObjectTypeRegistry::Get()));
		OutResult.ObjectStoreSeconds = FPlatformTime::Seconds() - StartSeconds;
	}

	/**
	 * Reads the save, if this is a load.
	 *
	 * The seed and the objects come out of it; the ground is checked against what
	 * this build has before any of it is used, because a resort restored onto a
	 * different mountain is worse than a refused load (ARCHITECTURE §9).
	 */
	bool ReadSave(
		const FMassifWorldBuildInputs& Inputs, uint64 ElevationModelHash,
		FMassifWorldBuildProgress& Progress, FMassifWorldBuildResult& OutResult)
	{
		if (Inputs.SaveName.IsEmpty())
		{
			return true;
		}

		Progress.Report(EMassifWorldBuildStage::Save, Inputs.SaveName, 0.0f);

		FMassifWorldSave Save;
		FString ReadError;
		if (!MassifReadWorldSave(MassifWorldSavePath(Inputs.SaveName), Save, ReadError))
		{
			OutResult.Error = ReadError;
			return false;
		}
		if (!MassifWorldSaveMatchesElevation(
			Save.Header, Inputs.ElevationModelName, ElevationModelHash, ReadError))
		{
			OutResult.Error = ReadError;
			return false;
		}

		OutResult.MasterSeed = static_cast<uint32>(Save.Header.MasterSeed);
		OutResult.ObjectStore = MakeShared<const FMassifObjectStore>(MoveTemp(Save.ObjectStore));
		OutResult.LoadedSimState = MoveTemp(Save.SimState);
		return true;
	}
}

void FMassifWorldBuildProgress::Report(
	EMassifWorldBuildStage Stage, FString Detail, float StageFraction)
{
	FScopeLock Lock(&Mutex);
	Snapshot.Stage = Stage;
	Snapshot.Detail = MoveTemp(Detail);
	Snapshot.StageFraction = StageFraction;
}

void FMassifWorldBuildProgress::ReportStage(EMassifWorldBuildStage Stage)
{
	Report(Stage, FString(), 0.0f);
}

FMassifWorldBuildProgressSnapshot FMassifWorldBuildProgress::Read() const
{
	FScopeLock Lock(&Mutex);
	return Snapshot;
}

FMassifWorldBuildResult MassifWorldBuild::Run(
	const FMassifWorldBuildInputs& Inputs, FMassifWorldBuildProgress& Progress)
{
	FMassifWorldBuildResult Result;
	Result.MasterSeed = Inputs.MasterSeed;
	Result.ElevationModelName = Inputs.ElevationModelName;

	TSharedPtr<const FMassifHeightmap> Heightmap = LoadElevationModel(Inputs, Progress, Result);
	if (!Heightmap.IsValid())
	{
		Progress.ReportStage(EMassifWorldBuildStage::Failed);
		return Result;
	}
	Result.ElevationModelHash = Heightmap->GetContentHash();

	// Before the solve, because the save is where the seed comes from on a load and
	// everything downstream is a function of it.
	if (!ReadSave(Inputs, Result.ElevationModelHash, Progress, Result))
	{
		Progress.ReportStage(EMassifWorldBuildStage::Failed);
		return Result;
	}

	// The environment reads the elevation model, and the terrain field reads the
	// rivers traced out of it. That order is what keeps the carve out of the solve
	// that produced it.
	if (!SolveEnvironment(Heightmap.ToSharedRef(), Inputs, Progress, Result))
	{
		Progress.ReportStage(EMassifWorldBuildStage::Failed);
		return Result;
	}

	TraceRivers(Inputs, Progress, Result);
	CutTerrainField(Heightmap.ToSharedRef(), Progress, Result);
	FillObjectStore(Progress, Result);

	Progress.ReportStage(EMassifWorldBuildStage::Spawning);
	return Result;
}
