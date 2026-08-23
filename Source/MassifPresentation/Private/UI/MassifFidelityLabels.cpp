#include "MassifFidelityLabels.h"

namespace
{
	constexpr EMassifFidelity GLevelsLowestFirst[] = {
		EMassifFidelity::Off, EMassifFidelity::Cheap, EMassifFidelity::Standard, EMassifFidelity::Reference};
}

const TCHAR* MassifFidelityLabels::Label(EMassifFidelity Fidelity)
{
	switch (Fidelity)
	{
		case EMassifFidelity::Off: return TEXT("Off");
		case EMassifFidelity::Cheap: return TEXT("Low");
		case EMassifFidelity::Standard: return TEXT("Medium");
		case EMassifFidelity::Reference: return TEXT("High");
	}
	return TEXT("Off");
}

TOptional<EMassifFidelity> MassifFidelityLabels::Parse(const FString& Text)
{
	for (EMassifFidelity Fidelity : GLevelsLowestFirst)
	{
		if (Text.Equals(Label(Fidelity), ESearchCase::IgnoreCase))
		{
			return Fidelity;
		}
	}
	return {};
}

TArrayView<const EMassifFidelity> MassifFidelityLabels::AllLevels()
{
	return GLevelsLowestFirst;
}
