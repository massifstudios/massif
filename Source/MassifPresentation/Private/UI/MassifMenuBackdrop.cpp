#include "MassifMenuBackdrop.h"

#include "Engine/Texture2D.h"
#include "Style/MassifWidgetStyles.h"
#include "Styling/SlateBrush.h"

const TCHAR* FMassifMenuBackdrop::GetTexturePath()
{
	return TEXT("/Game/Textures/T_MainMenuBackdrop.T_MainMenuBackdrop");
}

const FSlateBrush* FMassifMenuBackdrop::GetBrush()
{
	if (Brush.IsValid())
	{
		return Brush.Get();
	}

	UTexture2D* Loaded = LoadObject<UTexture2D>(nullptr, GetTexturePath());
	if (Loaded == nullptr)
	{
		return MassifWidgetStyles::BackdropBrush();
	}

	Texture.Reset(Loaded);
	Brush = MakeShared<FSlateBrush>();
	Brush->SetResourceObject(Loaded);
	Brush->ImageSize = FVector2f(Loaded->GetSizeX(), Loaded->GetSizeY());
	return Brush.Get();
}
