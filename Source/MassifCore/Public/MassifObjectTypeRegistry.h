#pragma once

#include "CoreMinimal.h"
#include "MassifObjectRecord.h"
#include "UObject/SoftObjectPath.h"

/** What a type is, where that changes how its record is read. */
enum class EMassifObjectCategory : uint8
{
	/** Rockfall output. Leaves the planting date and vigour unused. */
	Boulder,

	/** A tree from the vegetation pass, which is the only tier that ages. */
	Tree,
};

/** Everything about a type that is not in the record, because it is the same for every instance of it. */
struct FMassifObjectTypeInfo
{
	FName Name;

	EMassifObjectCategory Category = EMassifObjectCategory::Boulder;

	/** Final asset. Empty until it is generated, at which point PlaceholderMeshPath stops being used. */
	FSoftObjectPath MeshPath;
	FSoftObjectPath PlaceholderMeshPath;

	/** Radius band the record's single size byte quantises over. */
	FVector2d SizeBandMeters = FVector2d(0.5, 4.0);

	/** How many meshes the variant nibble selects between, 1 to 16. */
	int32 VariantCount = 1;

	bool bCollides = false;
};

/**
 * Type ids, which are save data.
 *
 * Once an id has shipped in a save it can never be reused or renumbered, and
 * retiring a type leaves a hole rather than closing the gap. That rules out
 * handing out ids in registration order — reordering two lines of code would
 * then rewrite every save — so ids are written down explicitly and a collision
 * is fatal rather than resolved.
 *
 * Reserved ranges, so a tier can grow without reaching into another's numbers:
 *
 * |    range | tier |
 * |---------:|---|
 * |        0 | invalid |
 * |    1-255 | boulders |
 * |  256-511 | trees |
 * | 512-1023 | reserved for the next stored tier |
 */
class MASSIFCORE_API FMassifObjectTypeRegistry
{
public:
	/** The process-wide registry, holding the builtin table. Tests build their own instead. */
	static FMassifObjectTypeRegistry& Get();

	/** Fills a registry with the builtin types. Public so a test can populate a local one. */
	static void RegisterBuiltinTypes(FMassifObjectTypeRegistry& Registry);

	/** Fatal on a duplicate id or a duplicate name: both mean two types would share a save slot. */
	void Register(uint16 TypeId, const FMassifObjectTypeInfo& Info);

	/** Null for an id nothing registered, which is what a retired type looks like to a loader. */
	const FMassifObjectTypeInfo* FindTypeInfo(uint16 TypeId) const;

	/** MassifInvalidObjectTypeId if the name is unknown. */
	uint16 FindTypeId(FName Name) const;

	/** Registered ids in ascending order. */
	TArray<uint16> GetRegisteredTypeIds() const;

	int32 GetTypeCount() const;

private:
	TMap<uint16, FMassifObjectTypeInfo> TypesById;
	TMap<FName, uint16> IdsByName;
};

/** Ids of the builtin types, so nothing has to spell a name to place one. */
namespace MassifObjectTypes
{
	/**
	 * The rockfall's blocks. One type for now, drawn at whatever size the fall
	 * gave it; once boulder meshes exist this splits by lithology, which is why
	 * the band is the whole range the solve can produce rather than a subset.
	 */
	inline constexpr uint16 GraniteBoulder = 1;

	/**
	 * The trees the vegetation pass plants, one type per species.
	 *
	 * Trees are stored because a player can fell one, which is the test that puts
	 * an object in the store at all (OBJECT_STORE.md §2). Everything shorter — the
	 * shrubs, grasses and flowers of the same table — is scattered at view time
	 * from the seed and never written anywhere.
	 *
	 * These ids are save data and are written down rather than counted out, so
	 * adding an eleventh species cannot renumber the ten already in saves. Growth
	 * stages, when they exist, take further ids in this range rather than becoming
	 * a field on these.
	 */
	inline constexpr uint16 PiceaAbies = 256;
	inline constexpr uint16 LarixDecidua = 257;
	inline constexpr uint16 PinusSylvestris = 258;
	inline constexpr uint16 AbiesAlba = 259;
	inline constexpr uint16 AcerPseudoplatanus = 260;
	inline constexpr uint16 SorbusAucuparia = 261;
	inline constexpr uint16 BetulaPendula = 262;
	inline constexpr uint16 PopulusTremula = 263;
	inline constexpr uint16 PinusCembra = 264;
	inline constexpr uint16 AlnusIncana = 265;
}
