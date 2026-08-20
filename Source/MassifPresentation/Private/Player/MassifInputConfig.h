#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "MassifInputConfig.generated.h"

class UInputAction;
class UInputMappingContext;

/**
 * The project's input actions and the contexts that bind keys to them, built in
 * C++ rather than as editor assets so bindings live with the code that reads
 * them and cannot drift into an unmergeable .uasset.
 *
 * Three contexts: one always active for the view switch, one per view mode. The
 * two view contexts are never active at the same time, so the same movement
 * keys mean walking or flying depending on which one the controller has pushed.
 */
UCLASS()
class UMassifInputConfig : public UObject
{
	GENERATED_BODY()

public:
	/** Creates every action and context. Call once, before adding a context to a player. */
	void Build();

	/** Axis2D, X right and Y forward. */
	UPROPERTY()
	TObjectPtr<UInputAction> MoveAction;

	/** Axis2D, X yaw and Y pitch, already negated for a standard mouse. */
	UPROPERTY()
	TObjectPtr<UInputAction> LookAction;

	/** Axis1D, positive up. Flying only: the walker answers to gravity instead. */
	UPROPERTY()
	TObjectPtr<UInputAction> VerticalAction;

	UPROPERTY()
	TObjectPtr<UInputAction> SprintAction;

	UPROPERTY()
	TObjectPtr<UInputAction> JumpAction;

	UPROPERTY()
	TObjectPtr<UInputAction> ToggleViewAction;

	UPROPERTY()
	TObjectPtr<UInputMappingContext> CommonContext;

	UPROPERTY()
	TObjectPtr<UInputMappingContext> FlyContext;

	UPROPERTY()
	TObjectPtr<UInputMappingContext> WalkContext;

private:
	void BuildActions();
	void BuildCommonContext();
	void BuildFlyContext();
	void BuildWalkContext();
};
