#include "MassifStateHash.h"

void FMassifStateHash::Append(const void* Data, uint64 SizeBytes)
{
	const uint8* Bytes = static_cast<const uint8*>(Data);
	for (uint64 Index = 0; Index < SizeBytes; ++Index)
	{
		Hash ^= Bytes[Index];
		Hash *= FnvPrime;
	}
}

void FMassifStateHash::AppendFloats(TConstArrayView<float> Values)
{
	Append(Values.GetData(), Values.Num() * sizeof(float));
}

void FMassifStateHash::AppendInt(int64 Value)
{
	Append(&Value, sizeof(Value));
}

void FMassifStateHash::AppendUInt(uint64 Value)
{
	Append(&Value, sizeof(Value));
}
