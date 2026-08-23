#include "MassifInputConfig.h"


#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"

namespace
{
	UInputAction* MakeAction(UObject* Outer, const TCHAR* Name, EInputActionValueType ValueType)
	{
		UInputAction* Action = NewObject<UInputAction>(Outer, Name);
		Action->ValueType = ValueType;
		return Action;
	}

	UInputModifierNegate* MakeNegate(UObject* Outer, bool bX, bool bY, bool bZ)
	{
		UInputModifierNegate* Negate = NewObject<UInputModifierNegate>(Outer);
		Negate->bX = bX;
		Negate->bY = bY;
		Negate->bZ = bZ;
		return Negate;
	}

	/** Moves a single-axis key's value onto Y, which is the forward axis of a move. */
	UInputModifierSwizzleAxis* MakeForwardSwizzle(UObject* Outer)
	{
		UInputModifierSwizzleAxis* Swizzle = NewObject<UInputModifierSwizzleAxis>(Outer);
		Swizzle->Order = EInputAxisSwizzle::YXZ;
		return Swizzle;
	}

	/** WASD onto one Axis2D: X to the right, Y forward. */
	void MapMoveKeys(UInputMappingContext& Context, UInputAction* Action)
	{
		UObject* Outer = Context.GetOuter();
		Context.MapKey(Action, EKeys::D);
		Context.MapKey(Action, EKeys::A).Modifiers.Add(MakeNegate(Outer, true, true, true));

		FEnhancedActionKeyMapping& Forward = Context.MapKey(Action, EKeys::W);
		Forward.Modifiers.Add(MakeForwardSwizzle(Outer));

		FEnhancedActionKeyMapping& Back = Context.MapKey(Action, EKeys::S);
		Back.Modifiers.Add(MakeForwardSwizzle(Outer));
		Back.Modifiers.Add(MakeNegate(Outer, true, true, true));
	}

	/** Mouse onto yaw and pitch, with pitch negated so pushing the mouse forward looks up. */
	void MapLookKeys(UInputMappingContext& Context, UInputAction* Action)
	{
		Context.MapKey(Action, EKeys::Mouse2D).Modifiers.Add(MakeNegate(Context.GetOuter(), false, true, false));
	}
}

void UMassifInputConfig::Build()
{
	BuildActions();
	BuildCommonContext();
	BuildFlyContext();
	BuildWalkContext();
}

void UMassifInputConfig::BuildActions()
{
	MoveAction = MakeAction(this, TEXT("IA_Move"), EInputActionValueType::Axis2D);
	LookAction = MakeAction(this, TEXT("IA_Look"), EInputActionValueType::Axis2D);
	VerticalAction = MakeAction(this, TEXT("IA_Vertical"), EInputActionValueType::Axis1D);
	SprintAction = MakeAction(this, TEXT("IA_Sprint"), EInputActionValueType::Boolean);
	JumpAction = MakeAction(this, TEXT("IA_Jump"), EInputActionValueType::Boolean);
	ToggleViewAction = MakeAction(this, TEXT("IA_ToggleView"), EInputActionValueType::Boolean);

	ToggleViewPickerAction = MakeAction(this, TEXT("IA_ToggleViewPicker"), EInputActionValueType::Boolean);
	ToggleSettingsAction = MakeAction(this, TEXT("IA_ToggleSettings"), EInputActionValueType::Boolean);
}

void UMassifInputConfig::BuildCommonContext()
{
	CommonContext = NewObject<UInputMappingContext>(this, TEXT("IMC_Common"));
	CommonContext->MapKey(ToggleViewAction, EKeys::V);
	CommonContext->MapKey(ToggleViewPickerAction, EKeys::One);
	CommonContext->MapKey(ToggleSettingsAction, EKeys::F1);
}

void UMassifInputConfig::BuildFlyContext()
{
	FlyContext = NewObject<UInputMappingContext>(this, TEXT("IMC_Fly"));
	MapMoveKeys(*FlyContext, MoveAction);
	MapLookKeys(*FlyContext, LookAction);
	FlyContext->MapKey(VerticalAction, EKeys::E);
	FlyContext->MapKey(VerticalAction, EKeys::Q).Modifiers.Add(MakeNegate(this, true, true, true));
	FlyContext->MapKey(SprintAction, EKeys::LeftShift);
}

void UMassifInputConfig::BuildWalkContext()
{
	WalkContext = NewObject<UInputMappingContext>(this, TEXT("IMC_Walk"));
	MapMoveKeys(*WalkContext, MoveAction);
	MapLookKeys(*WalkContext, LookAction);
	WalkContext->MapKey(JumpAction, EKeys::SpaceBar);
	WalkContext->MapKey(SprintAction, EKeys::LeftShift);
}
