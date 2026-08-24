#include "MassifObjectTypeRegistry.h"

namespace
{
	FMassifObjectTypeInfo MakeGraniteBoulder()
	{
		FMassifObjectTypeInfo Info;
		Info.Name = TEXT("Rock.Boulder.Granite");
		Info.Category = EMassifObjectCategory::Boulder;
		Info.PlaceholderMeshPath = FSoftObjectPath(TEXT("/Engine/BasicShapes/Sphere"));

		// The whole range the rockfall can produce (FMassifRockfallParams block
		// radii). A band narrower than the solve's would silently clamp blocks
		// rather than fail, so the two are checked against each other where the
		// placements are drained rather than trusted to stay in step here.
		Info.SizeBandMeters = FVector2d(0.5, 4.0);
		Info.bCollides = true;
		return Info;
	}
}

FMassifObjectTypeRegistry& FMassifObjectTypeRegistry::Get()
{
	static FMassifObjectTypeRegistry Registry = []
	{
		FMassifObjectTypeRegistry Builtin;
		RegisterBuiltinTypes(Builtin);
		return Builtin;
	}();
	return Registry;
}

void FMassifObjectTypeRegistry::RegisterBuiltinTypes(FMassifObjectTypeRegistry& Registry)
{
	Registry.Register(MassifObjectTypes::GraniteBoulder, MakeGraniteBoulder());
}

void FMassifObjectTypeRegistry::Register(uint16 TypeId, const FMassifObjectTypeInfo& Info)
{
	checkf(TypeId != MassifInvalidObjectTypeId, TEXT("Object type id 0 is reserved for 'no object'."));
	checkf(!TypesById.Contains(TypeId), TEXT("Object type id %u is already registered as '%s'."),
		TypeId, *TypesById[TypeId].Name.ToString());
	checkf(!IdsByName.Contains(Info.Name), TEXT("Object type '%s' is already registered as id %u."),
		*Info.Name.ToString(), IdsByName[Info.Name]);
	checkf(Info.VariantCount >= 1 && Info.VariantCount <= MassifObjectFlags::VariantMask + 1,
		TEXT("Object type '%s' wants %d variants; the record's nibble holds 16."),
		*Info.Name.ToString(), Info.VariantCount);
	checkf(Info.SizeBandMeters.Y > Info.SizeBandMeters.X,
		TEXT("Object type '%s' has an empty size band; every instance would be the same size."),
		*Info.Name.ToString());

	TypesById.Add(TypeId, Info);
	IdsByName.Add(Info.Name, TypeId);
}

const FMassifObjectTypeInfo* FMassifObjectTypeRegistry::FindTypeInfo(uint16 TypeId) const
{
	return TypesById.Find(TypeId);
}

uint16 FMassifObjectTypeRegistry::FindTypeId(FName Name) const
{
	const uint16* TypeId = IdsByName.Find(Name);
	if (TypeId == nullptr)
	{
		return MassifInvalidObjectTypeId;
	}
	return *TypeId;
}

TArray<uint16> FMassifObjectTypeRegistry::GetRegisteredTypeIds() const
{
	TArray<uint16> TypeIds;
	TypesById.GetKeys(TypeIds);
	TypeIds.Sort();
	return TypeIds;
}

int32 FMassifObjectTypeRegistry::GetTypeCount() const
{
	return TypesById.Num();
}
