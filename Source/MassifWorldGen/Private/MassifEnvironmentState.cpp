#include "MassifEnvironmentState.h"

#include "MassifStateHash.h"

namespace
{
	constexpr double Uint8QuantisationSteps = 255.0;
	constexpr double Uint16QuantisationSteps = 65535.0;

	bool IsEncodingLogarithmic(EMassifChannelEncoding Encoding)
	{
		return Encoding == EMassifChannelEncoding::Logarithmic16;
	}

	/** Log space for quantities spanning orders of magnitude, so precision stays relative. */
	double CompressForQuantisation(double Value, EMassifChannelEncoding Encoding)
	{
		if (!IsEncodingLogarithmic(Encoding))
		{
			return Value;
		}
		return FMath::Loge(1.0 + FMath::Max(Value, 0.0));
	}

	double ExpandFromQuantisation(double Value, EMassifChannelEncoding Encoding)
	{
		if (!IsEncodingLogarithmic(Encoding))
		{
			return Value;
		}
		return FMath::Exp(Value) - 1.0;
	}

	double QuantisationSteps(EMassifChannelEncoding Encoding)
	{
		if (Encoding == EMassifChannelEncoding::Uint8)
		{
			return Uint8QuantisationSteps;
		}
		return Uint16QuantisationSteps;
	}

	int32 EncodingByteCount(EMassifChannelEncoding Encoding)
	{
		switch (Encoding)
		{
			case EMassifChannelEncoding::Uint8: return 1;
			case EMassifChannelEncoding::Uint16: return 2;
			case EMassifChannelEncoding::Logarithmic16: return 2;
			case EMassifChannelEncoding::Float32: return 4;
			case EMassifChannelEncoding::Float64: return 8;
			default: return 2;
		}
	}

	void MeasureRange(TArrayView<const double> Values, double& OutMinimum, double& OutMaximum)
	{
		OutMinimum = 0.0;
		OutMaximum = 0.0;
		if (Values.IsEmpty())
		{
			return;
		}

		OutMinimum = TNumericLimits<double>::Max();
		OutMaximum = TNumericLimits<double>::Lowest();
		for (double Value : Values)
		{
			OutMinimum = FMath::Min(OutMinimum, Value);
			OutMaximum = FMath::Max(OutMaximum, Value);
		}
	}

	void WriteQuantisedEntry(TArray<uint8>& Payload, int32 CellIndex, EMassifChannelEncoding Encoding, double Fraction)
	{
		const int32 Quantised = FMath::RoundToInt32(Fraction * QuantisationSteps(Encoding));
		if (Encoding == EMassifChannelEncoding::Uint8)
		{
			Payload[CellIndex] = static_cast<uint8>(Quantised);
			return;
		}

		const uint16 Entry = static_cast<uint16>(Quantised);
		FMemory::Memcpy(Payload.GetData() + CellIndex * sizeof(uint16), &Entry, sizeof(uint16));
	}

	double ReadQuantisedFraction(const TArray<uint8>& Payload, int32 CellIndex, EMassifChannelEncoding Encoding)
	{
		if (Encoding == EMassifChannelEncoding::Uint8)
		{
			return Payload[CellIndex] / Uint8QuantisationSteps;
		}

		uint16 Entry = 0;
		FMemory::Memcpy(&Entry, Payload.GetData() + CellIndex * sizeof(uint16), sizeof(uint16));
		return Entry / Uint16QuantisationSteps;
	}

	void StoreFloats(TArray<uint8>& Payload, TArrayView<const double> Values)
	{
		Payload.SetNumZeroed(Values.Num() * static_cast<int32>(sizeof(float)));
		for (int32 CellIndex = 0; CellIndex < Values.Num(); ++CellIndex)
		{
			const float Entry = static_cast<float>(Values[CellIndex]);
			FMemory::Memcpy(Payload.GetData() + CellIndex * sizeof(float), &Entry, sizeof(float));
		}
	}

	void StoreDoubles(TArray<uint8>& Payload, TArrayView<const double> Values)
	{
		Payload.SetNumZeroed(Values.Num() * static_cast<int32>(sizeof(double)));
		if (Values.IsEmpty())
		{
			return;
		}
		FMemory::Memcpy(Payload.GetData(), Values.GetData(), Values.Num() * sizeof(double));
	}

	void StoreIntegers(
		TArray<uint8>& Payload, EMassifChannelEncoding Encoding, double MinValue, double MaxValue,
		TArrayView<const double> Values)
	{
		Payload.SetNumZeroed(Values.Num() * EncodingByteCount(Encoding));

		const double CompressedMinimum = CompressForQuantisation(MinValue, Encoding);
		const double CompressedSpan = CompressForQuantisation(MaxValue, Encoding) - CompressedMinimum;
		if (CompressedSpan <= UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			return;
		}

		for (int32 CellIndex = 0; CellIndex < Values.Num(); ++CellIndex)
		{
			const double Compressed = CompressForQuantisation(Values[CellIndex], Encoding);
			const double Fraction = FMath::Clamp((Compressed - CompressedMinimum) / CompressedSpan, 0.0, 1.0);
			WriteQuantisedEntry(Payload, CellIndex, Encoding, Fraction);
		}
	}

	double DecodeEntry(
		const TArray<uint8>& Payload, int32 CellIndex, EMassifChannelEncoding Encoding, double MinValue,
		double MaxValue)
	{
		if (Encoding == EMassifChannelEncoding::Float32)
		{
			float Value = 0.0f;
			FMemory::Memcpy(&Value, Payload.GetData() + CellIndex * sizeof(float), sizeof(float));
			return Value;
		}
		if (Encoding == EMassifChannelEncoding::Float64)
		{
			double Value = 0.0;
			FMemory::Memcpy(&Value, Payload.GetData() + CellIndex * sizeof(double), sizeof(double));
			return Value;
		}

		const double Fraction = ReadQuantisedFraction(Payload, CellIndex, Encoding);
		const double CompressedMinimum = CompressForQuantisation(MinValue, Encoding);
		const double CompressedMaximum = CompressForQuantisation(MaxValue, Encoding);
		return ExpandFromQuantisation(FMath::Lerp(CompressedMinimum, CompressedMaximum, Fraction), Encoding);
	}

	/** Slice-major index of one component of one time slice, INDEX_NONE if either is out of range. */
	int32 ComponentSliceIndex(const FMassifChannelDescriptor& Descriptor, int32 TimeSlice, int32 ComponentIndex)
	{
		const int32 ComponentCount = MassifChannelComponentCount(Descriptor.Type);
		const bool bInRange = TimeSlice >= 0 && TimeSlice < Descriptor.TimeSliceCount
			&& ComponentIndex >= 0 && ComponentIndex < ComponentCount;
		if (!bInRange)
		{
			return INDEX_NONE;
		}
		return TimeSlice * ComponentCount + ComponentIndex;
	}

	/** Continuous cell coordinate of a world position, clamped to the outermost cell centres. */
	FVector2d CellCoordinateAt(const FMassifChannelGrid& Grid, FVector2d PositionMeters)
	{
		const double Column = FMath::Clamp(
			(PositionMeters.X + 0.5 * Grid.WorldSizeXMeters) / Grid.CellSizeMeters - 0.5, 0.0, Grid.Width - 1.0);
		const double Row = FMath::Clamp(
			(0.5 * Grid.WorldSizeYMeters - PositionMeters.Y) / Grid.CellSizeMeters - 0.5, 0.0, Grid.Height - 1.0);
		return FVector2d(Column, Row);
	}
}

FMassifChannelWriteScope::FMassifChannelWriteScope(
	FMassifEnvironmentState& InState, FMassifChannelHandle InChannel, int32 InTimeSlice)
	: State(InState)
	, Channel(InChannel)
	, TimeSlice(InTimeSlice)
	, Grid(InState.GetGrid(InChannel))
{
	const FMassifChannelDescriptor& Descriptor = State.GetDescriptor(Channel);
	checkf(InTimeSlice >= 0 && InTimeSlice < Descriptor.TimeSliceCount,
		TEXT("Time slice %d is outside channel '%s'"), InTimeSlice, *Descriptor.Name.ToString());

	ComponentScratch.SetNum(MassifChannelComponentCount(Descriptor.Type));
	for (TArray<double>& Component : ComponentScratch)
	{
		Component.SetNumZeroed(Grid.CellCount());
	}
}

FMassifChannelWriteScope::~FMassifChannelWriteScope()
{
	checkf(bCommitted, TEXT("Channel '%s' slice %d was written and never committed"),
		*State.GetDescriptor(Channel).Name.ToString(), TimeSlice);
}

TArrayView<double> FMassifChannelWriteScope::GetComponent(int32 ComponentIndex)
{
	checkf(ComponentScratch.IsValidIndex(ComponentIndex), TEXT("Channel '%s' has no component %d"),
		*State.GetDescriptor(Channel).Name.ToString(), ComponentIndex);
	return ComponentScratch[ComponentIndex];
}

const FMassifChannelGrid& FMassifChannelWriteScope::GetGrid() const
{
	return Grid;
}

void FMassifChannelWriteScope::Commit()
{
	checkf(!bCommitted, TEXT("Channel '%s' slice %d was committed twice"),
		*State.GetDescriptor(Channel).Name.ToString(), TimeSlice);
	State.CommitSlice(Channel, TimeSlice, ComponentScratch);
	bCommitted = true;
}

FMassifEnvironmentState::FMassifEnvironmentState(FVector2d InWorldSizeMeters)
	: WorldSizeMeters(InWorldSizeMeters)
{
}

FMassifChannelHandle FMassifEnvironmentState::RegisterChannel(const FMassifChannelDescriptor& Descriptor)
{
	const FMassifChannelHandle Existing = FindChannel(Descriptor.Name);
	if (!Existing.IsValid())
	{
		return AddChannel(Descriptor);
	}

	checkf(GetDescriptor(Existing) == Descriptor,
		TEXT("Channel '%s' is already registered with a different descriptor"), *Descriptor.Name.ToString());
	return Existing;
}

FMassifChannelHandle FMassifEnvironmentState::AddChannel(const FMassifChannelDescriptor& Descriptor)
{
	checkf(!Descriptor.Name.IsNone(), TEXT("A channel must be registered under a name"));
	checkf(Descriptor.TimeSliceCount >= 1, TEXT("Channel '%s' must hold at least one time slice"),
		*Descriptor.Name.ToString());
	checkf(Descriptor.CellSizeMeters > 0.0, TEXT("Channel '%s' must have a positive cell size"),
		*Descriptor.Name.ToString());

	const int32 ComponentCount = MassifChannelComponentCount(Descriptor.Type);
	TUniquePtr<FChannelStorage> Storage = MakeUnique<FChannelStorage>();
	Storage->Descriptor = Descriptor;
	Storage->Grid = FMassifChannelGrid::FromWorldSize(WorldSizeMeters, Descriptor.CellSizeMeters);
	Storage->ComponentSlices.SetNum(Descriptor.TimeSliceCount * ComponentCount);
	Storage->ComponentMinimum.SetNumZeroed(ComponentCount);
	Storage->ComponentMaximum.SetNumZeroed(ComponentCount);

	const int32 ChannelIndex = Channels.Add(MoveTemp(Storage));
	ChannelIndexByName.Add(Descriptor.Name, ChannelIndex);
	return FMassifChannelHandle{ ChannelIndex };
}

FMassifChannelHandle FMassifEnvironmentState::FindChannel(FName Name) const
{
	const int32* ChannelIndex = ChannelIndexByName.Find(Name);
	if (ChannelIndex == nullptr)
	{
		return FMassifChannelHandle();
	}
	return FMassifChannelHandle{ *ChannelIndex };
}

int32 FMassifEnvironmentState::GetChannelCount() const
{
	return Channels.Num();
}

FMassifEnvironmentState::FChannelStorage& FMassifEnvironmentState::GetStorage(FMassifChannelHandle Channel)
{
	checkf(Channels.IsValidIndex(Channel.Index), TEXT("Channel handle %d is not registered"), Channel.Index);
	return *Channels[Channel.Index];
}

const FMassifEnvironmentState::FChannelStorage& FMassifEnvironmentState::GetStorage(
	FMassifChannelHandle Channel) const
{
	checkf(Channels.IsValidIndex(Channel.Index), TEXT("Channel handle %d is not registered"), Channel.Index);
	return *Channels[Channel.Index];
}

const FMassifChannelDescriptor& FMassifEnvironmentState::GetDescriptor(FMassifChannelHandle Channel) const
{
	return GetStorage(Channel).Descriptor;
}

const FMassifChannelGrid& FMassifEnvironmentState::GetGrid(FMassifChannelHandle Channel) const
{
	return GetStorage(Channel).Grid;
}

FVector2d FMassifEnvironmentState::GetWorldSizeMeters() const
{
	return WorldSizeMeters;
}

TArray<FMassifChannelHandle> FMassifEnvironmentState::GetAllChannels() const
{
	TArray<FMassifChannelHandle> Handles;
	Handles.Reserve(Channels.Num());
	for (int32 ChannelIndex = 0; ChannelIndex < Channels.Num(); ++ChannelIndex)
	{
		Handles.Add(FMassifChannelHandle{ ChannelIndex });
	}
	return Handles;
}

TArray<FMassifChannelHandle> FMassifEnvironmentState::GetChannelsByName() const
{
	TArray<FMassifChannelHandle> Handles = GetAllChannels();
	Handles.Sort([this](const FMassifChannelHandle& Left, const FMassifChannelHandle& Right)
	{
		return GetDescriptor(Left).Name.Compare(GetDescriptor(Right).Name) < 0;
	});
	return Handles;
}

FMassifEnvironmentState::FComponentSlice& FMassifEnvironmentState::GetComponentSlice(
	FChannelStorage& Storage, int32 TimeSlice, int32 ComponentIndex)
{
	const int32 SliceIndex = ComponentSliceIndex(Storage.Descriptor, TimeSlice, ComponentIndex);
	checkf(Storage.ComponentSlices.IsValidIndex(SliceIndex),
		TEXT("Channel '%s' has no component %d of slice %d"),
		*Storage.Descriptor.Name.ToString(), ComponentIndex, TimeSlice);
	return Storage.ComponentSlices[SliceIndex];
}

const FMassifEnvironmentState::FComponentSlice& FMassifEnvironmentState::GetComponentSlice(
	const FChannelStorage& Storage, int32 TimeSlice, int32 ComponentIndex)
{
	const int32 SliceIndex = ComponentSliceIndex(Storage.Descriptor, TimeSlice, ComponentIndex);
	checkf(Storage.ComponentSlices.IsValidIndex(SliceIndex),
		TEXT("Channel '%s' has no component %d of slice %d"),
		*Storage.Descriptor.Name.ToString(), ComponentIndex, TimeSlice);
	return Storage.ComponentSlices[SliceIndex];
}

void FMassifEnvironmentState::CommitSlice(
	FMassifChannelHandle Channel, int32 TimeSlice, const TArray<TArray<double>>& ComponentValues)
{
	FChannelStorage& Storage = GetStorage(Channel);
	for (int32 ComponentIndex = 0; ComponentIndex < ComponentValues.Num(); ++ComponentIndex)
	{
		StoreComponentSlice(Storage, TimeSlice, ComponentIndex, ComponentValues[ComponentIndex]);
	}
	Storage.bWritten = true;
}

void FMassifEnvironmentState::StoreComponentSlice(
	FChannelStorage& Storage, int32 TimeSlice, int32 ComponentIndex, TArrayView<const double> Values)
{
	checkf(Values.Num() == Storage.Grid.CellCount(), TEXT("Channel '%s' was given %d values for %d cells"),
		*Storage.Descriptor.Name.ToString(), Values.Num(), Storage.Grid.CellCount());

	FComponentSlice& Slice = GetComponentSlice(Storage, TimeSlice, ComponentIndex);
	MeasureRange(Values, Slice.MinValue, Slice.MaxValue);
	WidenChannelRange(Storage, ComponentIndex, Slice.MinValue, Slice.MaxValue);

	const EMassifChannelEncoding Encoding = Storage.Descriptor.Encoding;
	if (Encoding == EMassifChannelEncoding::Float32)
	{
		StoreFloats(Slice.Payload, Values);
		return;
	}
	if (Encoding == EMassifChannelEncoding::Float64)
	{
		StoreDoubles(Slice.Payload, Values);
		return;
	}
	StoreIntegers(Slice.Payload, Encoding, Slice.MinValue, Slice.MaxValue, Values);
}

void FMassifEnvironmentState::WidenChannelRange(
	FChannelStorage& Storage, int32 ComponentIndex, double Minimum, double Maximum)
{
	if (!Storage.bWritten)
	{
		Storage.ComponentMinimum[ComponentIndex] = Minimum;
		Storage.ComponentMaximum[ComponentIndex] = Maximum;
		return;
	}

	Storage.ComponentMinimum[ComponentIndex] = FMath::Min(Storage.ComponentMinimum[ComponentIndex], Minimum);
	Storage.ComponentMaximum[ComponentIndex] = FMath::Max(Storage.ComponentMaximum[ComponentIndex], Maximum);
}

double FMassifEnvironmentState::DecodeCell(
	const FChannelStorage& Storage, const FComponentSlice& Slice, int32 Column, int32 Row)
{
	const int32 CellIndex = Storage.Grid.CellIndex(Column, Row);
	const int32 ByteCount = EncodingByteCount(Storage.Descriptor.Encoding);
	if (CellIndex == INDEX_NONE || (CellIndex + 1) * ByteCount > Slice.Payload.Num())
	{
		return 0.0;
	}
	return DecodeEntry(Slice.Payload, CellIndex, Storage.Descriptor.Encoding, Slice.MinValue, Slice.MaxValue);
}

double FMassifEnvironmentState::SampleBilinear(
	const FChannelStorage& Storage, const FComponentSlice& Slice, FVector2d CellCoordinate)
{
	const int32 Column = FMath::Min(FMath::FloorToInt32(CellCoordinate.X), Storage.Grid.Width - 1);
	const int32 Row = FMath::Min(FMath::FloorToInt32(CellCoordinate.Y), Storage.Grid.Height - 1);
	const int32 NextColumn = FMath::Min(Column + 1, Storage.Grid.Width - 1);
	const int32 NextRow = FMath::Min(Row + 1, Storage.Grid.Height - 1);

	const double ColumnFraction = CellCoordinate.X - Column;
	const double NorthEdge = FMath::Lerp(
		DecodeCell(Storage, Slice, Column, Row), DecodeCell(Storage, Slice, NextColumn, Row), ColumnFraction);
	const double SouthEdge = FMath::Lerp(
		DecodeCell(Storage, Slice, Column, NextRow), DecodeCell(Storage, Slice, NextColumn, NextRow), ColumnFraction);
	return FMath::Lerp(NorthEdge, SouthEdge, CellCoordinate.Y - Row);
}

double FMassifEnvironmentState::SampleComponent(
	const FChannelStorage& Storage, int32 ComponentIndex, FVector2d PositionMeters, int32 TimeSlice)
{
	if (!Storage.bWritten)
	{
		return 0.0;
	}

	const FComponentSlice& Slice = GetComponentSlice(Storage, TimeSlice, ComponentIndex);
	const FVector2d CellCoordinate = CellCoordinateAt(Storage.Grid, PositionMeters);
	if (Storage.Descriptor.Type == EMassifChannelType::Classified)
	{
		return DecodeCell(
			Storage, Slice, FMath::RoundToInt32(CellCoordinate.X), FMath::RoundToInt32(CellCoordinate.Y));
	}
	return SampleBilinear(Storage, Slice, CellCoordinate);
}

double FMassifEnvironmentState::SampleScalar(
	FMassifChannelHandle Channel, FVector2d PositionMeters, int32 TimeSlice) const
{
	return SampleComponent(GetStorage(Channel), 0, PositionMeters, TimeSlice);
}

FVector2d FMassifEnvironmentState::SampleVector(
	FMassifChannelHandle Channel, FVector2d PositionMeters, int32 TimeSlice) const
{
	const FChannelStorage& Storage = GetStorage(Channel);
	checkf(Storage.Descriptor.Type == EMassifChannelType::Vector2,
		TEXT("Channel '%s' is %s, not a vector"),
		*Storage.Descriptor.Name.ToString(), MassifChannelTypeName(Storage.Descriptor.Type));
	return FVector2d(
		SampleComponent(Storage, 0, PositionMeters, TimeSlice),
		SampleComponent(Storage, 1, PositionMeters, TimeSlice));
}

int32 FMassifEnvironmentState::SampleClass(
	FMassifChannelHandle Channel, FVector2d PositionMeters, int32 TimeSlice) const
{
	return FMath::RoundToInt32(SampleComponent(GetStorage(Channel), 0, PositionMeters, TimeSlice));
}

double FMassifEnvironmentState::ReadCell(
	FMassifChannelHandle Channel, int32 Column, int32 Row, int32 ComponentIndex, int32 TimeSlice) const
{
	const FChannelStorage& Storage = GetStorage(Channel);
	const int32 ClampedColumn = FMath::Clamp(Column, 0, Storage.Grid.Width - 1);
	const int32 ClampedRow = FMath::Clamp(Row, 0, Storage.Grid.Height - 1);
	return DecodeCell(Storage, GetComponentSlice(Storage, TimeSlice, ComponentIndex), ClampedColumn, ClampedRow);
}

double FMassifEnvironmentState::GetChannelMinimum(FMassifChannelHandle Channel, int32 ComponentIndex) const
{
	const FChannelStorage& Storage = GetStorage(Channel);
	checkf(Storage.ComponentMinimum.IsValidIndex(ComponentIndex), TEXT("Channel '%s' has no component %d"),
		*Storage.Descriptor.Name.ToString(), ComponentIndex);
	return Storage.ComponentMinimum[ComponentIndex];
}

double FMassifEnvironmentState::GetChannelMaximum(FMassifChannelHandle Channel, int32 ComponentIndex) const
{
	const FChannelStorage& Storage = GetStorage(Channel);
	checkf(Storage.ComponentMaximum.IsValidIndex(ComponentIndex), TEXT("Channel '%s' has no component %d"),
		*Storage.Descriptor.Name.ToString(), ComponentIndex);
	return Storage.ComponentMaximum[ComponentIndex];
}

bool FMassifEnvironmentState::IsChannelWritten(FMassifChannelHandle Channel) const
{
	return GetStorage(Channel).bWritten;
}

uint64 FMassifEnvironmentState::ComputeContentHash() const
{
	FMassifStateHash Hash;
	for (FMassifChannelHandle Channel : GetChannelsByName())
	{
		AppendChannelToHash(Hash, GetStorage(Channel));
	}
	return Hash.Finalize();
}

void FMassifEnvironmentState::AppendChannelToHash(FMassifStateHash& Hash, const FChannelStorage& Storage)
{
	const FString NameString = Storage.Descriptor.Name.ToString();
	Hash.Append(*NameString, NameString.Len() * sizeof(TCHAR));
	Hash.AppendInt(static_cast<int64>(Storage.Descriptor.Type));
	Hash.AppendInt(static_cast<int64>(Storage.Descriptor.Encoding));
	Hash.AppendInt(Storage.Descriptor.TimeSliceCount);
	Hash.AppendInt(Storage.Grid.Width);
	Hash.AppendInt(Storage.Grid.Height);

	for (const FComponentSlice& Slice : Storage.ComponentSlices)
	{
		Hash.Append(Slice.Payload.GetData(), Slice.Payload.Num());
		Hash.AppendInt(FMath::RoundToInt64(Slice.MinValue * 1000.0));
		Hash.AppendInt(FMath::RoundToInt64(Slice.MaxValue * 1000.0));
	}
}

int64 FMassifEnvironmentState::GetResidentByteCount() const
{
	int64 TotalBytes = 0;
	for (const TUniquePtr<FChannelStorage>& Storage : Channels)
	{
		TotalBytes += ChannelByteCount(*Storage);
	}
	return TotalBytes;
}

int64 FMassifEnvironmentState::ChannelByteCount(const FChannelStorage& Storage)
{
	int64 TotalBytes = 0;
	for (const FComponentSlice& Slice : Storage.ComponentSlices)
	{
		TotalBytes += Slice.Payload.Num();
	}
	return TotalBytes;
}
