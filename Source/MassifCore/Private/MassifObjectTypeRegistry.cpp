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

	/**
	 * One tree species.
	 *
	 * The band is crown radius at the smallest and largest the species is planted
	 * at, which is what the record's size byte quantises over; the mesh is the
	 * growth-form placeholder the species is drawn with until its model exists.
	 * Both are the same numbers the flora table carries, and a test in the
	 * presentation module asserts they have not drifted apart.
	 */
	FMassifObjectTypeInfo MakeTree(
		const TCHAR* Name, const TCHAR* PlaceholderMeshPath, FVector2d CrownRadiusBandMeters)
	{
		FMassifObjectTypeInfo Info;
		Info.Name = Name;
		Info.Category = EMassifObjectCategory::Tree;
		Info.PlaceholderMeshPath = FSoftObjectPath(PlaceholderMeshPath);
		Info.SizeBandMeters = CrownRadiusBandMeters;

		// Trees do not collide yet: nothing walks the massif, and a hundred thousand
		// collision primitives would be paid for before anything could hit one.
		Info.bCollides = false;
		return Info;
	}

	const TCHAR* ConiferSpire = TEXT("/Game/Models/Placeholder/SM_PhConiferSpire");
	const TCHAR* ConiferBroad = TEXT("/Game/Models/Placeholder/SM_PhConiferBroad");
	const TCHAR* ConiferOpen = TEXT("/Game/Models/Placeholder/SM_PhConiferOpen");
	const TCHAR* BroadleafRound = TEXT("/Game/Models/Placeholder/SM_PhBroadleafRound");
	const TCHAR* TreeSmall = TEXT("/Game/Models/Placeholder/SM_PhTreeSmall");

	void RegisterTrees(FMassifObjectTypeRegistry& Registry)
	{
		Registry.Register(MassifObjectTypes::PiceaAbies,
			MakeTree(TEXT("Tree.PiceaAbies"), ConiferSpire, FVector2d(1.870, 3.520)));
		Registry.Register(MassifObjectTypes::LarixDecidua,
			MakeTree(TEXT("Tree.LarixDecidua"), ConiferOpen, FVector2d(1.920, 3.600)));
		Registry.Register(MassifObjectTypes::PinusSylvestris,
			MakeTree(TEXT("Tree.PinusSylvestris"), ConiferBroad, FVector2d(2.210, 3.640)));
		Registry.Register(MassifObjectTypes::AbiesAlba,
			MakeTree(TEXT("Tree.AbiesAlba"), ConiferSpire, FVector2d(2.070, 3.565)));
		Registry.Register(MassifObjectTypes::AcerPseudoplatanus,
			MakeTree(TEXT("Tree.AcerPseudoplatanus"), BroadleafRound, FVector2d(3.060, 5.220)));
		Registry.Register(MassifObjectTypes::SorbusAucuparia,
			MakeTree(TEXT("Tree.SorbusAucuparia"), BroadleafRound, FVector2d(1.260, 2.340)));
		Registry.Register(MassifObjectTypes::BetulaPendula,
			MakeTree(TEXT("Tree.BetulaPendula"), BroadleafRound, FVector2d(1.650, 2.970)));
		Registry.Register(MassifObjectTypes::PopulusTremula,
			MakeTree(TEXT("Tree.PopulusTremula"), BroadleafRound, FVector2d(1.800, 3.120)));
		Registry.Register(MassifObjectTypes::PinusCembra,
			MakeTree(TEXT("Tree.PinusCembra"), ConiferBroad, FVector2d(1.960, 3.920)));
		Registry.Register(MassifObjectTypes::AlnusIncana,
			MakeTree(TEXT("Tree.AlnusIncana"), TreeSmall, FVector2d(1.500, 2.600)));
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
	RegisterTrees(Registry);
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
