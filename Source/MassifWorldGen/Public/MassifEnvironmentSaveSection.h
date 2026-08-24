#pragma once

#include "CoreMinimal.h"
#include "MassifSaveSection.h"

class FMassifEnvironmentState;

/**
 * The solved environment as a block of a save.
 *
 * Writing takes the state a scheduler holds; reading produces a carrier for the
 * loader to hand to a freshly scheduled graph. Nothing here decides whether the
 * bake is used, because only the loader knows what the graph looks like now.
 */
class MASSIFWORLDGEN_API FMassifEnvironmentSaveSection : public IMassifSaveSection
{
public:
	/** Reads into a state of its own. */
	FMassifEnvironmentSaveSection() = default;

	/** Writes the state given. */
	explicit FMassifEnvironmentSaveSection(TSharedRef<FMassifEnvironmentState> InState);

	virtual bool Serialize(FArchive& Ar, FString& OutError) override;

	/** What was read, or null where nothing was. */
	TSharedPtr<FMassifEnvironmentState> GetState() const
	{
		return State;
	}

private:
	TSharedPtr<FMassifEnvironmentState> State;
};
