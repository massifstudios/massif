#include "Weather/MassifWorldTime.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "MassifSimSubsystem.h"

namespace
{
	/**
	 * Winds the world's clock, which every system reading the hour then follows:
	 * the sun and sky move, and the fog with them.
	 *
	 * A command rather than a write, so it lands on a tick boundary along with
	 * every other mutation and no two readers disagree about the hour inside one
	 * frame.
	 */
	FAutoConsoleCommandWithWorldAndArgs GMassifTimeOfDayCommand(
		TEXT("massif.TimeOfDay"),
		TEXT("Sets the time of day, as an hour from 0 to 24. Affects everything that reads the clock."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args, UWorld* World)
			{
				UGameInstance* GameInstance = World != nullptr ? World->GetGameInstance() : nullptr;
				UMassifSimSubsystem* Sim =
					GameInstance != nullptr ? GameInstance->GetSubsystem<UMassifSimSubsystem>() : nullptr;
				if (Sim == nullptr || Args.Num() < 1)
				{
					UE_LOG(LogTemp, Warning, TEXT("massif.TimeOfDay <hour>, with the simulation running."));
					return;
				}

				const double Hour = FMath::Fmod(FMath::Abs(FCString::Atod(*Args[0])), 24.0);
				Sim->SetTimeOfDayMinutes(FMath::RoundToInt32(Hour * 60.0));
				UE_LOG(LogTemp, Display, TEXT("Time of day set to %02d:%02d."),
					int32(Hour), FMath::RoundToInt32(FMath::Frac(Hour) * 60.0));
			}));
}

namespace MassifWorldTime
{
	FMassifWallClock GetWallClock(const UWorld* World)
	{
		if (World == nullptr)
		{
			return FMassifWallClock();
		}

		const UGameInstance* GameInstance = World->GetGameInstance();
		if (GameInstance == nullptr)
		{
			return FMassifWallClock();
		}

		const UMassifSimSubsystem* Sim = GameInstance->GetSubsystem<UMassifSimSubsystem>();
		if (Sim == nullptr)
		{
			return FMassifWallClock();
		}

		return Sim->GetWallClock();
	}

	double GetDayFraction(const UWorld* World)
	{
		if (World == nullptr)
		{
			return 0.0;
		}

		const UGameInstance* GameInstance = World->GetGameInstance();
		if (GameInstance == nullptr)
		{
			return 0.0;
		}

		const UMassifSimSubsystem* Sim = GameInstance->GetSubsystem<UMassifSimSubsystem>();
		if (Sim == nullptr)
		{
			return 0.0;
		}

		return Sim->GetWallClock().DayFraction;
	}

	FMassifSunPosition GetSunPosition(const UWorld* World, double LatitudeDegrees)
	{
		return MassifSunAtTimeOfDay(LatitudeDegrees, DayOfYear, GetDayFraction(World));
	}
}
