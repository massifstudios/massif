#pragma once

#include "CoreMinimal.h"

/** Incremental FNV-1a 64-bit hash for determinism checks on sim and worldgen state. */
struct MASSIFCORE_API FMassifStateHash
{
	void Append(const void* Data, uint64 SizeBytes);

	void AppendFloats(TConstArrayView<float> Values);
	void AppendInt(int64 Value);
	void AppendUInt(uint64 Value);

	uint64 Finalize() const
	{
		return Hash;
	}

private:
	static constexpr uint64 FnvOffsetBasis = 0xcbf29ce484222325ull;
	static constexpr uint64 FnvPrime = 0x100000001b3ull;

	uint64 Hash = FnvOffsetBasis;
};
