#pragma once

#include "CoreMinimal.h"
#include "MassifEnvironmentChannel.h"

struct FMassifStateHash;

class FMassifEnvironmentState;

/**
 * Scratch for one slice of one channel, encoded into the state on Commit.
 *
 * The scratch is double: a process solves in double and the channel's encoding
 * decides what is kept, so precision is a property of the channel rather than of
 * every process that writes one. Processes never see the encoding.
 */
class MASSIFWORLDGEN_API FMassifChannelWriteScope
{
public:
	FMassifChannelWriteScope(FMassifEnvironmentState& InState, FMassifChannelHandle InChannel, int32 InTimeSlice = 0);
	~FMassifChannelWriteScope();

	FMassifChannelWriteScope(const FMassifChannelWriteScope&) = delete;
	FMassifChannelWriteScope& operator=(const FMassifChannelWriteScope&) = delete;

	/** Scratch for one component, CellCount() long. Component 0 is the only one for scalars. */
	TArrayView<double> GetComponent(int32 ComponentIndex);

	const FMassifChannelGrid& GetGrid() const;

	/** Encodes the scratch into the state and measures the range. */
	void Commit();

private:
	FMassifEnvironmentState& State;
	FMassifChannelHandle Channel;
	int32 TimeSlice = 0;
	FMassifChannelGrid Grid;
	TArray<TArray<double>> ComponentScratch;
	bool bCommitted = false;
};

/**
 * Every channel the environment graph has produced.
 *
 * Each channel holds its own grid, so a 64 m climate channel costs a
 * sixty-fourth of an 8 m hydrology channel, and its own quantisation, so a
 * quantity spanning orders of magnitude is stored logarithmically while the
 * getters still speak its unit. Channels of different resolutions cover the
 * same world box and sample consistently at the same world position.
 *
 * Thread-safe for reads once written: sampling touches only immutable state.
 * Writes go through FMassifChannelWriteScope, one scope per channel slice.
 *
 * Never serialized: a pure function of the source data, the georeference and
 * the seed, so it is rebuilt at load (ARCHITECTURE §5.6).
 */
class MASSIFWORLDGEN_API FMassifEnvironmentState
{
public:
	explicit FMassifEnvironmentState(FVector2d InWorldSizeMeters);

	FMassifEnvironmentState(const FMassifEnvironmentState&) = delete;
	FMassifEnvironmentState& operator=(const FMassifEnvironmentState&) = delete;

	/**
	 * Registers a channel, or returns the existing handle if the name is already registered.
	 * Registering the same name with a different descriptor is a check-failure: two processes
	 * disagreeing about what a channel is must not silently pick one.
	 */
	FMassifChannelHandle RegisterChannel(const FMassifChannelDescriptor& Descriptor);

	/** Invalid handle if nothing is registered under the name. */
	FMassifChannelHandle FindChannel(FName Name) const;

	int32 GetChannelCount() const;
	const FMassifChannelDescriptor& GetDescriptor(FMassifChannelHandle Channel) const;
	const FMassifChannelGrid& GetGrid(FMassifChannelHandle Channel) const;
	FVector2d GetWorldSizeMeters() const;

	/** Handles in registration order. */
	TArray<FMassifChannelHandle> GetAllChannels() const;

	/** Handles sorted by name, for hashing and for stable debug listings. */
	TArray<FMassifChannelHandle> GetChannelsByName() const;

	/** Bilinear across cell centres. Positions outside the world clamp to its border. */
	double SampleScalar(FMassifChannelHandle Channel, FVector2d PositionMeters, int32 TimeSlice = 0) const;
	FVector2d SampleVector(FMassifChannelHandle Channel, FVector2d PositionMeters, int32 TimeSlice = 0) const;

	/** Nearest cell. Class indices have no meaningful interpolation. */
	int32 SampleClass(FMassifChannelHandle Channel, FVector2d PositionMeters, int32 TimeSlice = 0) const;

	/** One cell, unfiltered. For debug textures and whole-grid passes. */
	double ReadCell(
		FMassifChannelHandle Channel, int32 Column, int32 Row, int32 ComponentIndex = 0, int32 TimeSlice = 0) const;

	/** Lowest and highest value committed, for normalising debug views. */
	double GetChannelMinimum(FMassifChannelHandle Channel, int32 ComponentIndex = 0) const;
	double GetChannelMaximum(FMassifChannelHandle Channel, int32 ComponentIndex = 0) const;

	/** Whether anything has been committed to the channel yet. */
	bool IsChannelWritten(FMassifChannelHandle Channel) const;

	/** Hash over every channel, walked in name order so registration order cannot change it. */
	uint64 ComputeContentHash() const;

	/** Bytes held by all channel storage. For the debug panel. */
	int64 GetResidentByteCount() const;

private:
	friend class FMassifChannelWriteScope;

	/**
	 * One component of one time slice, quantised over the range that slice covers.
	 * Per-slice ranges keep a late wide slice from coarsening the slices before it.
	 */
	struct FComponentSlice
	{
		TArray<uint8> Payload;
		double MinValue = 0.0;
		double MaxValue = 0.0;
	};

	struct FChannelStorage
	{
		FMassifChannelDescriptor Descriptor;
		FMassifChannelGrid Grid;

		/** TimeSliceCount * ComponentCount entries, slice-major. */
		TArray<FComponentSlice> ComponentSlices;

		/** Range across every slice committed so far, so a scrubbed series keeps one legend. */
		TArray<double> ComponentMinimum;
		TArray<double> ComponentMaximum;

		bool bWritten = false;
	};

	FMassifChannelHandle AddChannel(const FMassifChannelDescriptor& Descriptor);

	FChannelStorage& GetStorage(FMassifChannelHandle Channel);
	const FChannelStorage& GetStorage(FMassifChannelHandle Channel) const;

	static FComponentSlice& GetComponentSlice(FChannelStorage& Storage, int32 TimeSlice, int32 ComponentIndex);
	static const FComponentSlice& GetComponentSlice(
		const FChannelStorage& Storage, int32 TimeSlice, int32 ComponentIndex);

	/** Encodes every component of one slice out of the write scope's scratch. */
	void CommitSlice(FMassifChannelHandle Channel, int32 TimeSlice, const TArray<TArray<double>>& ComponentValues);

	static void StoreComponentSlice(
		FChannelStorage& Storage, int32 TimeSlice, int32 ComponentIndex, TArrayView<const double> Values);
	static void WidenChannelRange(FChannelStorage& Storage, int32 ComponentIndex, double Minimum, double Maximum);

	static double DecodeCell(const FChannelStorage& Storage, const FComponentSlice& Slice, int32 Column, int32 Row);
	static double SampleBilinear(
		const FChannelStorage& Storage, const FComponentSlice& Slice, FVector2d CellCoordinate);
	static double SampleComponent(
		const FChannelStorage& Storage, int32 ComponentIndex, FVector2d PositionMeters, int32 TimeSlice);

	static void AppendChannelToHash(FMassifStateHash& Hash, const FChannelStorage& Storage);
	static int64 ChannelByteCount(const FChannelStorage& Storage);

	/** Held indirectly so registering a channel cannot move the storage a write scope is filling. */
	TArray<TUniquePtr<FChannelStorage>> Channels;

	TMap<FName, int32> ChannelIndexByName;
	FVector2d WorldSizeMeters = FVector2d::ZeroVector;
};
