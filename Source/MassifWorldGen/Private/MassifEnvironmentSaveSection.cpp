#include "MassifEnvironmentSaveSection.h"

#include "MassifEnvironmentState.h"

FMassifEnvironmentSaveSection::FMassifEnvironmentSaveSection(TSharedRef<FMassifEnvironmentState> InState)
	: State(MoveTemp(InState))
{
}

bool FMassifEnvironmentSaveSection::Serialize(FArchive& Ar, FString& OutError)
{
	if (Ar.IsLoading())
	{
		// The extent is the first thing the block carries, so the carrier is built
		// with none and takes the saved one.
		State = MakeShared<FMassifEnvironmentState>(FVector2d::ZeroVector);
	}

	if (!State.IsValid())
	{
		OutError = TEXT("There is no solved environment to write.");
		return false;
	}

	State->Serialize(Ar);
	if (!Ar.IsError())
	{
		return true;
	}

	OutError = TEXT("The saved environment could not be read.");
	State.Reset();
	return false;
}
