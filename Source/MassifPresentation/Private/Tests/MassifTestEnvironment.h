#pragma once

#include "CoreMinimal.h"
#include "MassifElevationSource.h"
#include "MassifEnvironmentBakeParams.h"
#include "MassifEnvironmentGraph.h"
#include "MassifEnvironmentProcess.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifEnvironmentState.h"
#include "MassifSunProcesses.h"

/**
 * A small analytic world for the verification tools, with two channels the
 * presentation layer has never been told about: a classified one and a vector
 * one. Those are the point of the fixture — the tools have to cover a channel
 * nothing names, which is the whole claim of the registry walk (ARCHITECTURE
 * §5.7).
 */
namespace MassifTestEnvironment
{
	constexpr uint32 Seed = 4711;
	constexpr double WorldSizeMeters = 1024.0;
	constexpr double ExtraCellSizeMeters = 128.0;

	/** Classes the chequerboard cycles through, so a midpoint has two answers to pick between. */
	constexpr int32 PatchClassCount = 3;

	inline const FName& PatchChannel()
	{
		static const FName Name(TEXT("Test.Patchwork"));
		return Name;
	}

	inline const FName& DriftChannel()
	{
		static const FName Name(TEXT("Test.Drift"));
		return Name;
	}

	inline const FName& SeasonChannel()
	{
		static const FName Name(TEXT("Test.Season"));
		return Name;
	}

	inline const FName& ExtraProcess()
	{
		static const FName Name(TEXT("Test.Extra"));
		return Name;
	}

	/** Slices the seasonal test channel holds, and how far its scale swings over them. */
	constexpr int32 SeasonSliceCount = 24;
	constexpr double SeasonQuietScale = 0.5;
	constexpr double SeasonLoudScale = 1.5;

	/** What one slice of the seasonal channel is scaled by. A cosine over the year, quiet at slice 0. */
	inline double SeasonScale(int32 TimeSlice)
	{
		const double Middle = 0.5 * (SeasonQuietScale + SeasonLoudScale);
		const double Half = 0.5 * (SeasonLoudScale - SeasonQuietScale);
		return Middle - Half * FMath::Cos(UE_DOUBLE_TWO_PI * TimeSlice / SeasonSliceCount);
	}

	/** Writes the two channels nothing in presentation knows the name of. */
	class FExtraProcess final : public IMassifEnvironmentProcess
	{
	public:
		FMassifProcessDeclaration Declare() const override
		{
			FMassifProcessDeclaration Declaration;
			Declaration.Name = ExtraProcess();
			Declaration.SupportedFidelities = {EMassifFidelity::Standard};
			Declaration.OutputChannels.Add(MakePatchChannel());
			Declaration.OutputChannels.Add(MakeDriftChannel());
			Declaration.OutputChannels.Add(MakeSeasonChannel());
			return Declaration;
		}

		void Execute(FMassifEnvironmentContext& Context, EMassifFidelity) const override
		{
			WritePatches(Context);
			WriteDrift(Context);
			WriteSeason(Context);
		}

	private:
		static FMassifChannelDescriptor MakePatchChannel()
		{
			FMassifChannelDescriptor Descriptor;
			Descriptor.Name = PatchChannel();
			Descriptor.Type = EMassifChannelType::Classified;
			Descriptor.Encoding = EMassifChannelEncoding::Uint8;
			Descriptor.CellSizeMeters = ExtraCellSizeMeters;
			Descriptor.ColorMap = EMassifChannelColorMap::Categorical;
			return Descriptor;
		}

		static FMassifChannelDescriptor MakeDriftChannel()
		{
			FMassifChannelDescriptor Descriptor;
			Descriptor.Name = DriftChannel();
			Descriptor.Type = EMassifChannelType::Vector2;
			Descriptor.Encoding = EMassifChannelEncoding::Float32;
			Descriptor.CellSizeMeters = ExtraCellSizeMeters;
			Descriptor.Unit = TEXT("m/s");
			return Descriptor;
		}

		/**
		 * One field, scaled slice by slice and otherwise unchanged.
		 *
		 * The shape a year's rainfall shared out over its half-months has, and the
		 * case a view normalised slice by slice cannot draw: every slice would
		 * renormalise to the same picture.
		 */
		static FMassifChannelDescriptor MakeSeasonChannel()
		{
			FMassifChannelDescriptor Descriptor;
			Descriptor.Name = SeasonChannel();
			Descriptor.Encoding = EMassifChannelEncoding::Float32;
			Descriptor.CellSizeMeters = ExtraCellSizeMeters;
			Descriptor.TimeSliceCount = SeasonSliceCount;
			Descriptor.Unit = TEXT("mm");
			return Descriptor;
		}

		static void WritePatches(FMassifEnvironmentContext& Context)
		{
			const TUniquePtr<FMassifChannelWriteScope> Scope = Context.OpenOutput(PatchChannel());
			const int32 Width = Scope->GetGrid().Width;
			TArrayView<double> Classes = Scope->GetComponent(0);
			for (int32 CellIndex = 0; CellIndex < Classes.Num(); ++CellIndex)
			{
				Classes[CellIndex] = (CellIndex % Width + CellIndex / Width) % PatchClassCount;
			}
			Scope->Commit();
		}

		static void WriteSeason(FMassifEnvironmentContext& Context)
		{
			for (int32 TimeSlice = 0; TimeSlice < SeasonSliceCount; ++TimeSlice)
			{
				WriteSeasonSlice(Context, TimeSlice);
			}
		}

		static void WriteSeasonSlice(FMassifEnvironmentContext& Context, int32 TimeSlice)
		{
			const TUniquePtr<FMassifChannelWriteScope> Scope = Context.OpenOutput(SeasonChannel(), TimeSlice);
			const int32 Width = Scope->GetGrid().Width;
			TArrayView<double> Millimetres = Scope->GetComponent(0);
			for (int32 CellIndex = 0; CellIndex < Millimetres.Num(); ++CellIndex)
			{
				Millimetres[CellIndex] = (100.0 + 10.0 * (CellIndex % Width)) * SeasonScale(TimeSlice);
			}
			Scope->Commit();
		}

		static void WriteDrift(FMassifEnvironmentContext& Context)
		{
			const TUniquePtr<FMassifChannelWriteScope> Scope = Context.OpenOutput(DriftChannel());
			const int32 Width = Scope->GetGrid().Width;
			TArrayView<double> East = Scope->GetComponent(0);
			TArrayView<double> North = Scope->GetComponent(1);
			for (int32 CellIndex = 0; CellIndex < East.Num(); ++CellIndex)
			{
				East[CellIndex] = 1.0 + CellIndex % Width;
				North[CellIndex] = -2.0;
			}
			Scope->Commit();
		}
	};

	inline TUniquePtr<FMassifEnvironmentScheduler> MakeScheduler()
	{
		FMassifAnalyticElevationSource::FParams TerrainParams;
		TerrainParams.Shape = EMassifAnalyticTerrain::Cone;
		TerrainParams.WorldSizeMeters = WorldSizeMeters;

		TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MakeUnique<FMassifEnvironmentScheduler>(
			MakeShared<FMassifAnalyticElevationSource>(TerrainParams), FMassifGeoreference(), Seed);

		FMassifEnvironmentBakeParams BakeParams;
		BakeParams.CellSizeMeters = 32.0;
		BakeParams.DropletsPerCell = 0.05;
		BakeParams.MaxDropletSteps = 8;
		MassifRegisterAltitudeProcess(*Scheduler, BakeParams);

		FMassifSunParams SunParams;
		SunParams.CellSizeMeters = ExtraCellSizeMeters;
		SunParams.HorizonAzimuthCount = 4;
		SunParams.HorizonStepCount = 6;
		SunParams.SunPathStepCount = 4;
		MassifRegisterSunProcesses(*Scheduler, SunParams);

		Scheduler->RegisterProcess(MakeShared<FExtraProcess>());

		FString BuildError;
		checkf(Scheduler->BuildSchedule(BuildError), TEXT("%s"), *BuildError);
		Scheduler->Execute();
		return Scheduler;
	}
}
