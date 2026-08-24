#include "MassifEnvironmentPointSet.h"

namespace
{
	/** As for a channel descriptor: two callers may spell the same text from different literals. */
	bool PointSetTextMatches(const TCHAR* Left, const TCHAR* Right)
	{
		if (Left == nullptr || Right == nullptr)
		{
			return Left == Right;
		}
		return FCString::Strcmp(Left, Right) == 0;
	}
}

bool FMassifPointSetDescriptor::operator==(const FMassifPointSetDescriptor& Other) const
{
	const bool bStorageMatches = Name == Other.Name && TileSizeMeters == Other.TileSizeMeters
		&& RadiusRangeMeters == Other.RadiusRangeMeters;
	if (!bStorageMatches || ClassNames.Num() != Other.ClassNames.Num())
	{
		return false;
	}

	for (int32 ClassIndex = 0; ClassIndex < ClassNames.Num(); ++ClassIndex)
	{
		if (ClassNames[ClassIndex] != Other.ClassNames[ClassIndex])
		{
			return false;
		}
	}
	return PointSetTextMatches(Description, Other.Description);
}
