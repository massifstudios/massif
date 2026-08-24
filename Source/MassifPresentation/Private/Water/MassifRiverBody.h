#pragma once

#include "CoreMinimal.h"
#include "WaterBodyRiverActor.h"
#include "WaterBodyRiverComponent.h"

#include "MassifRiverBody.generated.h"

/**
 * A river body whose shape is built after the world exists rather than placed in
 * a level.
 *
 * The water plugin is written for bodies drawn by hand into a map and saved with
 * it: a body only rebuilds its surface away from the editor if its component
 * says it is dynamic, and a placed river never is. Massif has no placed rivers
 * at all — the network comes out of world generation against a seed — so every
 * body here is built at runtime and every one of them is dynamic.
 */
UCLASS()
class UMassifRiverBodyComponent : public UWaterBodyRiverComponent
{
	GENERATED_BODY()

public:
	virtual bool IsBodyDynamic() const override
	{
		return true;
	}
};

/** A river actor carrying the component above in place of the plugin's own. */
UCLASS()
class AMassifRiverBody : public AWaterBodyRiver
{
	GENERATED_BODY()

public:
	AMassifRiverBody(const FObjectInitializer& ObjectInitializer);
};
