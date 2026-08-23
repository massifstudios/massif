#include "MassifFluidBake.h"

#include "Async/ParallelFor.h"
#include "HAL/PlatformTime.h"
#include "MassifTerrainField.h"

namespace
{
	/** What a cell holds, which is what the pressure solve treats it as. */
	enum class ECellKind : uint8
	{
		Air,
		Fluid,
		Solid,
	};

	/** Keeps a particle off the ground it landed on, so it is never read as inside it. */
	constexpr double GroundSkinMeters = 1.0e-3;

	// -----------------------------------------------------------------------
	// One staggered component of the velocity field
	// -----------------------------------------------------------------------

	/**
	 * Velocities live on cell faces, not at cell centres: a face carries the flow
	 * through it, which is what the pressure solve cancels and what a centred field
	 * can only average.
	 *
	 * The three components differ only in how many samples they have along each axis
	 * and where in the cell a sample sits, so they share one type and the scatter
	 * and the gather are written once.
	 */
	struct FFaceField
	{
		FIntVector Counts = FIntVector::ZeroValue;
		FVector OffsetCells = FVector::ZeroVector;
		TArray<float> Values;
		TArray<float> Weights;
		TArray<float> BeforePressure;

		/**
		 * Which samples the particles themselves gave a velocity to, and which the
		 * extrapolation has reached so far.
		 *
		 * Kept apart because the sweep has to start from the particles' own faces both
		 * times it runs, and by the end of a sweep the reached set covers a band that
		 * no particle is in.
		 */
		TArray<uint8> Seeded;
		TArray<uint8> Reached;
		TArray<uint8> NextReached;
		TArray<float> NextValues;

		void Setup(FIntVector InCounts, FVector InOffsetCells)
		{
			Counts = InCounts;
			OffsetCells = InOffsetCells;
			const int32 SampleCount = Counts.X * Counts.Y * Counts.Z;
			Values.SetNumZeroed(SampleCount);
			Weights.SetNumZeroed(SampleCount);
			BeforePressure.SetNumZeroed(SampleCount);
			Seeded.SetNumZeroed(SampleCount);
			Reached.SetNumZeroed(SampleCount);
			NextReached.SetNumZeroed(SampleCount);
			NextValues.SetNumZeroed(SampleCount);
		}

		int32 SampleIndex(int32 X, int32 Y, int32 Z) const
		{
			return (Z * Counts.Y + Y) * Counts.X + X;
		}

		bool Contains(int32 X, int32 Y, int32 Z) const
		{
			return X >= 0 && Y >= 0 && Z >= 0 && X < Counts.X && Y < Counts.Y && Z < Counts.Z;
		}
	};

	/** The eight lattice samples a position falls between, and how much of each it takes. */
	struct FTrilinearStencil
	{
		FIntVector Corner = FIntVector::ZeroValue;
		FVector Fraction = FVector::ZeroVector;

		double CornerWeight(int32 CornerX, int32 CornerY, int32 CornerZ) const
		{
			const double WeightX = CornerX == 0 ? 1.0 - Fraction.X : Fraction.X;
			const double WeightY = CornerY == 0 ? 1.0 - Fraction.Y : Fraction.Y;
			const double WeightZ = CornerZ == 0 ? 1.0 - Fraction.Z : Fraction.Z;
			return WeightX * WeightY * WeightZ;
		}
	};

	FTrilinearStencil MakeStencil(const FFaceField& Field, FVector PositionCells)
	{
		const FVector SampleCells = PositionCells - Field.OffsetCells;
		FTrilinearStencil Stencil;
		Stencil.Corner = FIntVector(
			FMath::FloorToInt32(SampleCells.X),
			FMath::FloorToInt32(SampleCells.Y),
			FMath::FloorToInt32(SampleCells.Z));
		Stencil.Fraction = SampleCells - FVector(Stencil.Corner);
		return Stencil;
	}

	FIntVector StencilCorner(int32 CornerIndex)
	{
		return FIntVector(CornerIndex & 1, (CornerIndex >> 1) & 1, (CornerIndex >> 2) & 1);
	}

	void ScatterToField(FFaceField& InOutField, FVector PositionCells, double Value)
	{
		const FTrilinearStencil Stencil = MakeStencil(InOutField, PositionCells);
		for (int32 CornerIndex = 0; CornerIndex < 8; ++CornerIndex)
		{
			const FIntVector Corner = StencilCorner(CornerIndex);
			const FIntVector At = Stencil.Corner + Corner;
			if (!InOutField.Contains(At.X, At.Y, At.Z))
			{
				continue;
			}
			const double Weight = Stencil.CornerWeight(Corner.X, Corner.Y, Corner.Z);
			const int32 Sample = InOutField.SampleIndex(At.X, At.Y, At.Z);
			InOutField.Values[Sample] += static_cast<float>(Weight * Value);
			InOutField.Weights[Sample] += static_cast<float>(Weight);
		}
	}

	double GatherFromField(const FFaceField& Field, const TArray<float>& Values, FVector PositionCells)
	{
		const FTrilinearStencil Stencil = MakeStencil(Field, PositionCells);
		double Total = 0.0;
		for (int32 CornerIndex = 0; CornerIndex < 8; ++CornerIndex)
		{
			const FIntVector Corner = StencilCorner(CornerIndex);
			const int32 X = FMath::Clamp(Stencil.Corner.X + Corner.X, 0, Field.Counts.X - 1);
			const int32 Y = FMath::Clamp(Stencil.Corner.Y + Corner.Y, 0, Field.Counts.Y - 1);
			const int32 Z = FMath::Clamp(Stencil.Corner.Z + Corner.Z, 0, Field.Counts.Z - 1);
			Total += Stencil.CornerWeight(Corner.X, Corner.Y, Corner.Z)
				* Values[Field.SampleIndex(X, Y, Z)];
		}
		return Total;
	}

	// -----------------------------------------------------------------------
	// The region, its ground, and the water in it
	// -----------------------------------------------------------------------

	struct FFluidState
	{
		FIntVector CellCounts = FIntVector::ZeroValue;
		double CellSizeMeters = 0.05;
		FVector OriginMeters = FVector::ZeroVector;

		FFaceField AlongX;
		FFaceField AlongY;
		FFaceField AlongZ;

		TArray<ECellKind> CellKinds;
		TArray<float> Pressure;
		TArray<float> NextPressure;
		TArray<float> Divergence;

		/** Ground altitude under each column of cells, which is what makes a cell solid. */
		TArray<float> GroundAltitudeMeters;

		TArray<FVector3f> PositionsMeters;
		TArray<FVector3f> VelocitiesMetersPerSecond;

		/** The particles sorted by cell, so a grid sample can read the ones near it. */
		TArray<int32> BinStart;
		TArray<int32> BinCounts;
		TArray<int32> BinnedParticles;

		int32 CellIndex(int32 X, int32 Y, int32 Z) const
		{
			return (Z * CellCounts.Y + Y) * CellCounts.X + X;
		}

		int32 ColumnIndex(int32 X, int32 Y) const
		{
			return Y * CellCounts.X + X;
		}

		FVector ToCells(FVector PositionMeters) const
		{
			return (PositionMeters - OriginMeters) / CellSizeMeters;
		}

		int32 CellOfParticle(FVector PositionMeters) const
		{
			const FVector Cells = ToCells(PositionMeters);
			return CellIndex(
				FMath::Clamp(FMath::FloorToInt32(Cells.X), 0, CellCounts.X - 1),
				FMath::Clamp(FMath::FloorToInt32(Cells.Y), 0, CellCounts.Y - 1),
				FMath::Clamp(FMath::FloorToInt32(Cells.Z), 0, CellCounts.Z - 1));
		}

		double GroundAt(int32 X, int32 Y) const
		{
			return GroundAltitudeMeters[ColumnIndex(X, Y)];
		}
	};

	void SeedLayer(const FMassifFluidBakeParams& Params, FFluidState& InOutState, int32 Z);
	void SeedCellIfWet(
		const FMassifFluidBakeParams& Params, FFluidState& InOutState, int32 X, int32 Y, int32 Z);
	void MarkSolidLayer(FFluidState& InOutState, int32 Z);
	void MarkFluidAt(FFluidState& InOutState, FVector PositionMeters);
	void CloseSolidFaces(FFluidState& InOutState);
	void CloseSolidFacesInLayer(FFluidState& InOutState, int32 Z);
	void CloseFacesOfCell(FFluidState& InOutState, int32 X, int32 Y, int32 Z);
	void MeasureDivergenceInLayer(FFluidState& InOutState, int32 Z, double Scale);
	bool IsOpenNeighbour(const FFluidState& State, int32 X, int32 Y, int32 Z);
	double FluidPressureAt(const FFluidState& State, int32 X, int32 Y, int32 Z);
	double SweptPressure(const FFluidState& State, int32 X, int32 Y, int32 Z, double SourceScale);
	void SubtractPressureInLayer(FFluidState& InOutState, int32 Z, double Scale);
	void SubtractPressureAtCell(FFluidState& InOutState, int32 X, int32 Y, int32 Z, double Scale);
	void GatherNeighbourSlab(
		const FFluidState& State, const FFaceField& Field, FIntVector Sample, int32 Axis, int32 Z,
		double& InOutTotal, double& InOutWeight);

	void SetupFields(const FMassifFluidRegion& Region, FFluidState& OutState)
	{
		OutState.CellCounts = Region.CellCounts();
		OutState.CellSizeMeters = Region.CellSizeMeters;
		OutState.OriginMeters = Region.OriginMeters;

		const FIntVector Counts = OutState.CellCounts;
		OutState.AlongX.Setup(FIntVector(Counts.X + 1, Counts.Y, Counts.Z), FVector(0.0, 0.5, 0.5));
		OutState.AlongY.Setup(FIntVector(Counts.X, Counts.Y + 1, Counts.Z), FVector(0.5, 0.0, 0.5));
		OutState.AlongZ.Setup(FIntVector(Counts.X, Counts.Y, Counts.Z + 1), FVector(0.5, 0.5, 0.0));

		const int32 CellCount = Counts.X * Counts.Y * Counts.Z;
		OutState.CellKinds.SetNumZeroed(CellCount);
		OutState.Pressure.SetNumZeroed(CellCount);
		OutState.NextPressure.SetNumZeroed(CellCount);
		OutState.Divergence.SetNumZeroed(CellCount);
		OutState.GroundAltitudeMeters.SetNumZeroed(Counts.X * Counts.Y);
	}

	/** The ground under every column, read once at the grid's own spacing. */
	void SampleGroundColumn(
		const FMassifTerrainField& Terrain, FFluidState& InOutState, int32 X, int32 Y)
	{
		const double HalfCellMeters = 0.5 * InOutState.CellSizeMeters;
		const FVector2d PositionMeters(
			InOutState.OriginMeters.X + X * InOutState.CellSizeMeters + HalfCellMeters,
			InOutState.OriginMeters.Y + Y * InOutState.CellSizeMeters + HalfCellMeters);
		InOutState.GroundAltitudeMeters[InOutState.ColumnIndex(X, Y)] = static_cast<float>(
			Terrain.SampleHeightMeters(PositionMeters, InOutState.CellSizeMeters));
	}

	void SampleGround(const FMassifTerrainField& Terrain, FFluidState& InOutState)
	{
		for (int32 Y = 0; Y < InOutState.CellCounts.Y; ++Y)
		{
			for (int32 X = 0; X < InOutState.CellCounts.X; ++X)
			{
				SampleGroundColumn(Terrain, InOutState, X, Y);
			}
		}
	}

	double CellCentreAltitudeMeters(const FFluidState& State, int32 Z)
	{
		return State.OriginMeters.Z + (Z + 0.5) * State.CellSizeMeters;
	}

	bool IsSolidCell(const FFluidState& State, int32 X, int32 Y, int32 Z)
	{
		return CellCentreAltitudeMeters(State, Z) < State.GroundAt(X, Y);
	}

	// -----------------------------------------------------------------------
	// Seeding
	// -----------------------------------------------------------------------

	/** One cell's worth of particles, on the cell's own lattice rather than scattered. */
	void SeedCell(
		const FMassifFluidBakeParams& Params, FFluidState& InOutState, int32 X, int32 Y, int32 Z)
	{
		const int32 PerAxis = FMath::Max(Params.ParticlesPerCellAxis, 1);
		const double StepMeters = InOutState.CellSizeMeters / PerAxis;
		const FVector CellOriginMeters =
			InOutState.OriginMeters + FVector(X, Y, Z) * InOutState.CellSizeMeters;

		for (int32 Sample = 0; Sample < PerAxis * PerAxis * PerAxis; ++Sample)
		{
			const FVector LatticeStep(
				Sample % PerAxis, (Sample / PerAxis) % PerAxis, Sample / (PerAxis * PerAxis));
			const FVector OffsetMeters = (LatticeStep + FVector(0.5)) * StepMeters;
			InOutState.PositionsMeters.Add(FVector3f(CellOriginMeters + OffsetMeters));
			InOutState.VelocitiesMetersPerSecond.Add(
				FVector3f(static_cast<float>(Params.InflowSpeedMetersPerSecond), 0.0f, 0.0f));
		}
	}

	/** Every cell under the starting level and over the ground, filled with water. */
	void SeedWater(
		const FMassifFluidRegion& Region, const FMassifFluidBakeParams& Params, FFluidState& InOutState)
	{
		for (int32 Z = 0; Z < InOutState.CellCounts.Z; ++Z)
		{
			const double CentreMeters = CellCentreAltitudeMeters(InOutState, Z);
			if (CentreMeters > Region.InitialSurfaceAltitudeMeters)
			{
				continue;
			}
			SeedLayer(Params, InOutState, Z);
		}
	}

	void SeedLayer(const FMassifFluidBakeParams& Params, FFluidState& InOutState, int32 Z)
	{
		for (int32 Y = 0; Y < InOutState.CellCounts.Y; ++Y)
		{
			for (int32 X = 0; X < InOutState.CellCounts.X; ++X)
			{
				SeedCellIfWet(Params, InOutState, X, Y, Z);
			}
		}
	}

	void SeedCellIfWet(
		const FMassifFluidBakeParams& Params, FFluidState& InOutState, int32 X, int32 Y, int32 Z)
	{
		if (IsSolidCell(InOutState, X, Y, Z))
		{
			return;
		}
		SeedCell(Params, InOutState, X, Y, Z);
	}

	// -----------------------------------------------------------------------
	// Particles to grid
	// -----------------------------------------------------------------------

	void ClearFields(FFluidState& InOutState)
	{
		FFaceField* Fields[] = { &InOutState.AlongX, &InOutState.AlongY, &InOutState.AlongZ };
		for (FFaceField* Field : Fields)
		{
			FMemory::Memzero(Field->Values.GetData(), Field->Values.Num() * sizeof(float));
			FMemory::Memzero(Field->Weights.GetData(), Field->Weights.Num() * sizeof(float));
		}
	}

	/** Counting sort of the particles into the cells they stand in: count, prefix sum, place. */
	void BinParticles(FFluidState& InOutState)
	{
		const int32 CellCount = InOutState.CellKinds.Num();
		InOutState.BinCounts.SetNumZeroed(CellCount);
		for (const FVector3f& PositionMeters : InOutState.PositionsMeters)
		{
			++InOutState.BinCounts[InOutState.CellOfParticle(FVector(PositionMeters))];
		}

		InOutState.BinStart.SetNumUninitialized(CellCount + 1);
		int32 RunningTotal = 0;
		for (int32 Cell = 0; Cell < CellCount; ++Cell)
		{
			InOutState.BinStart[Cell] = RunningTotal;
			RunningTotal += InOutState.BinCounts[Cell];
			InOutState.BinCounts[Cell] = InOutState.BinStart[Cell];
		}
		InOutState.BinStart[CellCount] = RunningTotal;

		InOutState.BinnedParticles.SetNumUninitialized(InOutState.PositionsMeters.Num());
		for (int32 ParticleIndex = 0; ParticleIndex < InOutState.PositionsMeters.Num(); ++ParticleIndex)
		{
			const int32 Cell =
				InOutState.CellOfParticle(FVector(InOutState.PositionsMeters[ParticleIndex]));
			InOutState.BinnedParticles[InOutState.BinCounts[Cell]++] = ParticleIndex;
		}
	}

	/** The share one particle gives a sample, which is nothing unless the sample is one of its eight. */
	double SampleWeightForParticle(const FFaceField& Field, FVector ParticleCells, FIntVector Sample)
	{
		const FVector SampleCells = ParticleCells - Field.OffsetCells;
		const FIntVector Corner(
			FMath::FloorToInt32(SampleCells.X),
			FMath::FloorToInt32(SampleCells.Y),
			FMath::FloorToInt32(SampleCells.Z));
		const FVector Fraction = SampleCells - FVector(Corner);

		double Weight = 1.0;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (Sample[Axis] == Corner[Axis])
			{
				Weight *= 1.0 - Fraction[Axis];
				continue;
			}
			if (Sample[Axis] == Corner[Axis] + 1)
			{
				Weight *= Fraction[Axis];
				continue;
			}
			return 0.0;
		}
		return Weight;
	}

	/** Everything one sample takes from the particles in one cell. */
	void GatherCellIntoSample(
		const FFluidState& State, const FFaceField& Field, int32 Cell, FIntVector Sample, int32 Axis,
		double& InOutTotal, double& InOutWeight)
	{
		for (int32 Slot = State.BinStart[Cell]; Slot < State.BinStart[Cell + 1]; ++Slot)
		{
			const int32 ParticleIndex = State.BinnedParticles[Slot];
			const FVector ParticleCells = State.ToCells(FVector(State.PositionsMeters[ParticleIndex]));
			const double Weight = SampleWeightForParticle(Field, ParticleCells, Sample);
			if (Weight <= 0.0)
			{
				continue;
			}
			InOutTotal += Weight * State.VelocitiesMetersPerSecond[ParticleIndex][Axis];
			InOutWeight += Weight;
		}
	}

	void GatherSample(
		const FFluidState& State, FFaceField& InOutField, FIntVector Sample, int32 Axis)
	{
		double Total = 0.0;
		double TotalWeight = 0.0;
		for (int32 Z = Sample.Z - 1; Z <= Sample.Z + 1; ++Z)
		{
			GatherNeighbourSlab(State, InOutField, Sample, Axis, Z, Total, TotalWeight);
		}
		const int32 Index = InOutField.SampleIndex(Sample.X, Sample.Y, Sample.Z);
		InOutField.Values[Index] = static_cast<float>(Total);
		InOutField.Weights[Index] = static_cast<float>(TotalWeight);
	}

	void GatherNeighbourSlab(
		const FFluidState& State, const FFaceField& Field, FIntVector Sample, int32 Axis, int32 Z,
		double& InOutTotal, double& InOutWeight)
	{
		if (Z < 0 || Z >= State.CellCounts.Z)
		{
			return;
		}
		for (int32 Y = FMath::Max(Sample.Y - 1, 0); Y <= FMath::Min(Sample.Y + 1, State.CellCounts.Y - 1); ++Y)
		{
			for (int32 X = FMath::Max(Sample.X - 1, 0); X <= FMath::Min(Sample.X + 1, State.CellCounts.X - 1); ++X)
			{
				GatherCellIntoSample(
					State, Field, State.CellIndex(X, Y, Z), Sample, Axis, InOutTotal, InOutWeight);
			}
		}
	}

	void GatherFieldSlab(const FFluidState& State, FFaceField& InOutField, int32 Axis, int32 Z)
	{
		for (int32 Y = 0; Y < InOutField.Counts.Y; ++Y)
		{
			for (int32 X = 0; X < InOutField.Counts.X; ++X)
			{
				GatherSample(State, InOutField, FIntVector(X, Y, Z), Axis);
			}
		}
	}

	/**
	 * The particles' velocity on the grid, gathered by the samples rather than
	 * scattered by the particles.
	 *
	 * Same arithmetic either way, but a scatter has every particle adding into eight
	 * samples it shares with its neighbours, which races the moment it is threaded
	 * and was the one pass here still running on a single core. A sample that reads
	 * the particles around it instead writes only itself, so the whole pass threads
	 * with nothing shared and each sample sums in a fixed order of its own.
	 */
	void GatherParticlesToFields(FFluidState& InOutState)
	{
		BinParticles(InOutState);
		FFaceField* Fields[] = { &InOutState.AlongX, &InOutState.AlongY, &InOutState.AlongZ };
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			FFaceField& Field = *Fields[Axis];
			ParallelFor(Field.Counts.Z, [&InOutState, &Field, Axis](int32 Z)
			{
				GatherFieldSlab(InOutState, Field, Axis, Z);
			});
		}
	}

	void NormaliseField(FFaceField& InOutField)
	{
		for (int32 Sample = 0; Sample < InOutField.Values.Num(); ++Sample)
		{
			const bool bSeeded = InOutField.Weights[Sample] > UE_SMALL_NUMBER;
			InOutField.Seeded[Sample] = bSeeded ? 1 : 0;
			if (bSeeded)
			{
				InOutField.Values[Sample] /= InOutField.Weights[Sample];
			}
		}
	}

	void SaveBeforePressure(FFaceField& InOutField)
	{
		FMemory::Memcpy(
			InOutField.BeforePressure.GetData(), InOutField.Values.GetData(),
			InOutField.Values.Num() * sizeof(float));
	}

	// -----------------------------------------------------------------------
	// Carrying the velocity out past the water
	// -----------------------------------------------------------------------

	/**
	 * The mean of a sample's neighbours that already carry a velocity, and whether
	 * it had any.
	 */
	bool MeanOfReachedNeighbours(const FFaceField& Field, int32 X, int32 Y, int32 Z, float& OutValue)
	{
		const FIntVector Steps[] = {
			FIntVector(-1, 0, 0), FIntVector(1, 0, 0), FIntVector(0, -1, 0),
			FIntVector(0, 1, 0), FIntVector(0, 0, -1), FIntVector(0, 0, 1) };
		double TotalMetersPerSecond = 0.0;
		int32 ReachedCount = 0;
		for (const FIntVector& Step : Steps)
		{
			const FIntVector At(X + Step.X, Y + Step.Y, Z + Step.Z);
			if (!Field.Contains(At.X, At.Y, At.Z))
			{
				continue;
			}
			const int32 Sample = Field.SampleIndex(At.X, At.Y, At.Z);
			if (Field.Reached[Sample] == 0)
			{
				continue;
			}
			TotalMetersPerSecond += Field.Values[Sample];
			++ReachedCount;
		}
		if (ReachedCount == 0)
		{
			return false;
		}
		OutValue = static_cast<float>(TotalMetersPerSecond / ReachedCount);
		return true;
	}

	void ExtrapolateSample(FFaceField& InOutField, int32 X, int32 Y, int32 Z)
	{
		const int32 Sample = InOutField.SampleIndex(X, Y, Z);
		if (InOutField.Reached[Sample] != 0)
		{
			InOutField.NextValues[Sample] = InOutField.Values[Sample];
			InOutField.NextReached[Sample] = 1;
			return;
		}

		float MeanMetersPerSecond = 0.0f;
		const bool bHasNeighbour = MeanOfReachedNeighbours(InOutField, X, Y, Z, MeanMetersPerSecond);
		InOutField.NextValues[Sample] = bHasNeighbour ? MeanMetersPerSecond : 0.0f;
		InOutField.NextReached[Sample] = bHasNeighbour ? 1 : 0;
	}

	void ExtrapolateLayerAt(FFaceField& InOutField, int32 Z)
	{
		for (int32 Y = 0; Y < InOutField.Counts.Y; ++Y)
		{
			for (int32 X = 0; X < InOutField.Counts.X; ++X)
			{
				ExtrapolateSample(InOutField, X, Y, Z);
			}
		}
	}

	/**
	 * Carries the velocity a few cells out past the water into the air around it.
	 *
	 * A face with no particle either side holds nothing, and nothing is a wall: the
	 * pressure solve reads it as flow that cannot leave, and water at a free surface
	 * is held in by air it should be free to move into. Spreading the velocity out
	 * one layer at a time gives the surface somewhere to go, and gives a particle
	 * that crosses into an empty cell a velocity to be carried by rather than a
	 * dead stop.
	 *
	 * Swept a fixed number of layers, each reading only the layer before it, so the
	 * band is the same however the grid is walked.
	 */
	void ExtrapolateField(FFaceField& InOutField, int32 LayerCount)
	{
		FMemory::Memcpy(
			InOutField.Reached.GetData(), InOutField.Seeded.GetData(), InOutField.Seeded.Num());
		for (int32 Layer = 0; Layer < LayerCount; ++Layer)
		{
			ParallelFor(InOutField.Counts.Z, [&InOutField](int32 Z)
			{
				ExtrapolateLayerAt(InOutField, Z);
			});
			Swap(InOutField.Values, InOutField.NextValues);
			Swap(InOutField.Reached, InOutField.NextReached);
		}
	}

	void ExtrapolateAll(FFluidState& InOutState, int32 LayerCount)
	{
		ExtrapolateField(InOutState.AlongX, LayerCount);
		ExtrapolateField(InOutState.AlongY, LayerCount);
		ExtrapolateField(InOutState.AlongZ, LayerCount);
	}

	/** A cell is solid under the ground, fluid where a particle sits in it, and air otherwise. */
	void MarkCells(FFluidState& InOutState)
	{
		ParallelFor(InOutState.CellCounts.Z, [&InOutState](int32 Z)
		{
			MarkSolidLayer(InOutState, Z);
		});
		for (const FVector3f& PositionMeters : InOutState.PositionsMeters)
		{
			MarkFluidAt(InOutState, FVector(PositionMeters));
		}
	}

	void MarkSolidLayer(FFluidState& InOutState, int32 Z)
	{
		for (int32 Y = 0; Y < InOutState.CellCounts.Y; ++Y)
		{
			for (int32 X = 0; X < InOutState.CellCounts.X; ++X)
			{
				const int32 Cell = InOutState.CellIndex(X, Y, Z);
				const bool bSolid = IsSolidCell(InOutState, X, Y, Z);
				InOutState.CellKinds[Cell] = bSolid ? ECellKind::Solid : ECellKind::Air;
			}
		}
	}

	void MarkFluidAt(FFluidState& InOutState, FVector PositionMeters)
	{
		const FVector Cells = InOutState.ToCells(PositionMeters);
		const int32 X = FMath::Clamp(FMath::FloorToInt32(Cells.X), 0, InOutState.CellCounts.X - 1);
		const int32 Y = FMath::Clamp(FMath::FloorToInt32(Cells.Y), 0, InOutState.CellCounts.Y - 1);
		const int32 Z = FMath::Clamp(FMath::FloorToInt32(Cells.Z), 0, InOutState.CellCounts.Z - 1);
		const int32 Cell = InOutState.CellIndex(X, Y, Z);
		if (InOutState.CellKinds[Cell] == ECellKind::Solid)
		{
			return;
		}
		InOutState.CellKinds[Cell] = ECellKind::Fluid;
	}

	// -----------------------------------------------------------------------
	// The pressure solve
	// -----------------------------------------------------------------------

	void AddGravity(FFluidState& InOutState, const FMassifFluidBakeParams& Params)
	{
		const float FallMetersPerSecond = static_cast<float>(
			-Params.GravityMetersPerSecondSquared * Params.StepSeconds);
		for (int32 Sample = 0; Sample < InOutState.AlongZ.Values.Num(); ++Sample)
		{
			InOutState.AlongZ.Values[Sample] += FallMetersPerSecond;
		}
	}

	/** Nothing flows through the ground, so every face of a solid cell is held at nothing. */
	void CloseSolidFaces(FFluidState& InOutState)
	{
		for (int32 Z = 0; Z < InOutState.CellCounts.Z; ++Z)
		{
			CloseSolidFacesInLayer(InOutState, Z);
		}
	}

	void CloseSolidFacesInLayer(FFluidState& InOutState, int32 Z)
	{
		for (int32 Y = 0; Y < InOutState.CellCounts.Y; ++Y)
		{
			for (int32 X = 0; X < InOutState.CellCounts.X; ++X)
			{
				CloseFacesOfCell(InOutState, X, Y, Z);
			}
		}
	}

	void CloseFacesOfCell(FFluidState& InOutState, int32 X, int32 Y, int32 Z)
	{
		if (InOutState.CellKinds[InOutState.CellIndex(X, Y, Z)] != ECellKind::Solid)
		{
			return;
		}
		InOutState.AlongX.Values[InOutState.AlongX.SampleIndex(X, Y, Z)] = 0.0f;
		InOutState.AlongX.Values[InOutState.AlongX.SampleIndex(X + 1, Y, Z)] = 0.0f;
		InOutState.AlongY.Values[InOutState.AlongY.SampleIndex(X, Y, Z)] = 0.0f;
		InOutState.AlongY.Values[InOutState.AlongY.SampleIndex(X, Y + 1, Z)] = 0.0f;
		InOutState.AlongZ.Values[InOutState.AlongZ.SampleIndex(X, Y, Z)] = 0.0f;
		InOutState.AlongZ.Values[InOutState.AlongZ.SampleIndex(X, Y, Z + 1)] = 0.0f;
	}

	/** How much more leaves a cell than enters it, which is what the pressure has to cancel. */
	void MeasureDivergence(FFluidState& InOutState)
	{
		const double Scale = 1.0 / InOutState.CellSizeMeters;
		ParallelFor(InOutState.CellCounts.Z, [&InOutState, Scale](int32 Z)
		{
			MeasureDivergenceInLayer(InOutState, Z, Scale);
		});
	}

	void MeasureDivergenceInLayer(FFluidState& InOutState, int32 Z, double Scale)
	{
		for (int32 Y = 0; Y < InOutState.CellCounts.Y; ++Y)
		{
			for (int32 X = 0; X < InOutState.CellCounts.X; ++X)
			{
				const double OutwardMetersPerSecond =
					InOutState.AlongX.Values[InOutState.AlongX.SampleIndex(X + 1, Y, Z)]
						- InOutState.AlongX.Values[InOutState.AlongX.SampleIndex(X, Y, Z)]
					+ InOutState.AlongY.Values[InOutState.AlongY.SampleIndex(X, Y + 1, Z)]
						- InOutState.AlongY.Values[InOutState.AlongY.SampleIndex(X, Y, Z)]
					+ InOutState.AlongZ.Values[InOutState.AlongZ.SampleIndex(X, Y, Z + 1)]
						- InOutState.AlongZ.Values[InOutState.AlongZ.SampleIndex(X, Y, Z)];
				InOutState.Divergence[InOutState.CellIndex(X, Y, Z)] =
					static_cast<float>(OutwardMetersPerSecond * Scale);
			}
		}
	}

	/** Neighbours a cell may push into: air and fluid take pressure, solid gives none back. */
	int32 OpenNeighbourCount(const FFluidState& State, int32 X, int32 Y, int32 Z)
	{
		const FIntVector Steps[] = {
			FIntVector(-1, 0, 0), FIntVector(1, 0, 0), FIntVector(0, -1, 0),
			FIntVector(0, 1, 0), FIntVector(0, 0, -1), FIntVector(0, 0, 1) };
		int32 OpenCount = 0;
		for (const FIntVector& Step : Steps)
		{
			OpenCount += IsOpenNeighbour(State, X + Step.X, Y + Step.Y, Z + Step.Z) ? 1 : 0;
		}
		return OpenCount;
	}

	bool IsOpenNeighbour(const FFluidState& State, int32 X, int32 Y, int32 Z)
	{
		if (X < 0 || Y < 0 || Z < 0
			|| X >= State.CellCounts.X || Y >= State.CellCounts.Y || Z >= State.CellCounts.Z)
		{
			return false;
		}
		return State.CellKinds[State.CellIndex(X, Y, Z)] != ECellKind::Solid;
	}

	double NeighbourPressureSum(const FFluidState& State, int32 X, int32 Y, int32 Z)
	{
		const FIntVector Steps[] = {
			FIntVector(-1, 0, 0), FIntVector(1, 0, 0), FIntVector(0, -1, 0),
			FIntVector(0, 1, 0), FIntVector(0, 0, -1), FIntVector(0, 0, 1) };
		double TotalPascals = 0.0;
		for (const FIntVector& Step : Steps)
		{
			TotalPascals += FluidPressureAt(State, X + Step.X, Y + Step.Y, Z + Step.Z);
		}
		return TotalPascals;
	}

	/** Air stands at nothing, which is what gives the water a free surface to fall away at. */
	double FluidPressureAt(const FFluidState& State, int32 X, int32 Y, int32 Z)
	{
		if (!IsOpenNeighbour(State, X, Y, Z))
		{
			return 0.0;
		}
		if (State.CellKinds[State.CellIndex(X, Y, Z)] != ECellKind::Fluid)
		{
			return 0.0;
		}
		return State.Pressure[State.CellIndex(X, Y, Z)];
	}

	void SweepPressureInLayer(FFluidState& InOutState, int32 Z, double SourceScale)
	{
		for (int32 Y = 0; Y < InOutState.CellCounts.Y; ++Y)
		{
			for (int32 X = 0; X < InOutState.CellCounts.X; ++X)
			{
				InOutState.NextPressure[InOutState.CellIndex(X, Y, Z)] =
					static_cast<float>(SweptPressure(InOutState, X, Y, Z, SourceScale));
			}
		}
	}

	double SweptPressure(const FFluidState& State, int32 X, int32 Y, int32 Z, double SourceScale)
	{
		const int32 Cell = State.CellIndex(X, Y, Z);
		if (State.CellKinds[Cell] != ECellKind::Fluid)
		{
			return 0.0;
		}
		const int32 OpenCount = OpenNeighbourCount(State, X, Y, Z);
		if (OpenCount == 0)
		{
			return 0.0;
		}
		const double SourcePascals = State.Divergence[Cell] * SourceScale;
		return (NeighbourPressureSum(State, X, Y, Z) - SourcePascals) / OpenCount;
	}

	/**
	 * Jacobi rather than Gauss-Seidel: every cell reads the sweep before it and
	 * none reads a neighbour this sweep already touched, so the result does not
	 * depend on the order the grid is walked. That is what lets the sweep be split
	 * across threads and still land on the same answer to the bit, which is the
	 * whole reason the solve can be both threaded and deterministic.
	 */
	void SolvePressure(FFluidState& InOutState, const FMassifFluidBakeParams& Params)
	{
		FMemory::Memzero(InOutState.Pressure.GetData(), InOutState.Pressure.Num() * sizeof(float));
		const double SourceScale =
			FMath::Square(InOutState.CellSizeMeters) / FMath::Max(Params.StepSeconds, UE_DOUBLE_SMALL_NUMBER);

		for (int32 Iteration = 0; Iteration < Params.PressureIterations; ++Iteration)
		{
			ParallelFor(InOutState.CellCounts.Z, [&InOutState, SourceScale](int32 Z)
			{
				SweepPressureInLayer(InOutState, Z, SourceScale);
			});
			Swap(InOutState.Pressure, InOutState.NextPressure);
		}
	}

	void SubtractPressureInLayer(FFluidState& InOutState, int32 Z, double Scale)
	{
		for (int32 Y = 0; Y < InOutState.CellCounts.Y; ++Y)
		{
			for (int32 X = 0; X < InOutState.CellCounts.X; ++X)
			{
				SubtractPressureAtCell(InOutState, X, Y, Z, Scale);
			}
		}
	}

	void SubtractPressureAtCell(FFluidState& InOutState, int32 X, int32 Y, int32 Z, double Scale)
	{
		const double HerePascals = FluidPressureAt(InOutState, X, Y, Z);
		InOutState.AlongX.Values[InOutState.AlongX.SampleIndex(X, Y, Z)] -= static_cast<float>(
			Scale * (HerePascals - FluidPressureAt(InOutState, X - 1, Y, Z)));
		InOutState.AlongY.Values[InOutState.AlongY.SampleIndex(X, Y, Z)] -= static_cast<float>(
			Scale * (HerePascals - FluidPressureAt(InOutState, X, Y - 1, Z)));
		InOutState.AlongZ.Values[InOutState.AlongZ.SampleIndex(X, Y, Z)] -= static_cast<float>(
			Scale * (HerePascals - FluidPressureAt(InOutState, X, Y, Z - 1)));
	}

	void SubtractPressureGradient(FFluidState& InOutState, const FMassifFluidBakeParams& Params)
	{
		const double Scale = Params.StepSeconds / InOutState.CellSizeMeters;
		ParallelFor(InOutState.CellCounts.Z, [&InOutState, Scale](int32 Z)
		{
			SubtractPressureInLayer(InOutState, Z, Scale);
		});
		CloseSolidFaces(InOutState);
	}

	// -----------------------------------------------------------------------
	// Grid back to particles, and the step they take
	// -----------------------------------------------------------------------

	FVector GridVelocityAt(const FFluidState& State, FVector Cells)
	{
		return FVector(
			GatherFromField(State.AlongX, State.AlongX.Values, Cells),
			GatherFromField(State.AlongY, State.AlongY.Values, Cells),
			GatherFromField(State.AlongZ, State.AlongZ.Values, Cells));
	}

	FVector GridChangeAt(const FFluidState& State, FVector Cells)
	{
		return GridVelocityAt(State, Cells) - FVector(
			GatherFromField(State.AlongX, State.AlongX.BeforePressure, Cells),
			GatherFromField(State.AlongY, State.AlongY.BeforePressure, Cells),
			GatherFromField(State.AlongZ, State.AlongZ.BeforePressure, Cells));
	}

	/** The blend of what the grid says and what the grid changed, which is what FLIP is. */
	FVector BlendedVelocity(
		const FFluidState& State, const FMassifFluidBakeParams& Params, FVector Cells,
		FVector CarriedMetersPerSecond)
	{
		const FVector PicMetersPerSecond = GridVelocityAt(State, Cells);
		const FVector FlipMetersPerSecond = CarriedMetersPerSecond + GridChangeAt(State, Cells);
		return FMath::Lerp(PicMetersPerSecond, FlipMetersPerSecond, Params.FlipRatio);
	}

	/** Puts a particle back on the ground it went through, and takes the bed's share of its speed. */
	void SettleOnGround(
		const FFluidState& State, const FMassifFluidBakeParams& Params, FVector& InOutPositionMeters,
		FVector& InOutVelocityMetersPerSecond)
	{
		const FVector Cells = State.ToCells(InOutPositionMeters);
		const int32 X = FMath::Clamp(FMath::FloorToInt32(Cells.X), 0, State.CellCounts.X - 1);
		const int32 Y = FMath::Clamp(FMath::FloorToInt32(Cells.Y), 0, State.CellCounts.Y - 1);
		const double GroundMeters = State.GroundAt(X, Y);
		if (InOutPositionMeters.Z >= GroundMeters + GroundSkinMeters)
		{
			return;
		}

		InOutPositionMeters.Z = GroundMeters + GroundSkinMeters;
		InOutVelocityMetersPerSecond.X *= Params.BedSlip;
		InOutVelocityMetersPerSecond.Y *= Params.BedSlip;
		InOutVelocityMetersPerSecond.Z = FMath::Max(InOutVelocityMetersPerSecond.Z, 0.0);
	}

	/**
	 * A particle that leaves by the downstream face comes back in at the upstream
	 * one, at the depth and speed the reach feeds the region at.
	 *
	 * Recycled rather than destroyed and replaced: the count stays fixed, so the
	 * solve allocates nothing after it starts and every step does the same work.
	 */
	void RecycleOrHold(
		const FFluidState& State, const FMassifFluidBakeParams& Params, const FMassifFluidRegion& Region,
		FVector& InOutPositionMeters, FVector& InOutVelocityMetersPerSecond)
	{
		const FVector HighestMeters = State.OriginMeters + Region.ExtentMeters;
		if (InOutPositionMeters.X < HighestMeters.X)
		{
			InOutPositionMeters.Y =
				FMath::Clamp(InOutPositionMeters.Y, State.OriginMeters.Y, HighestMeters.Y);
			InOutPositionMeters.Z = FMath::Min(InOutPositionMeters.Z, HighestMeters.Z);
			return;
		}

		InOutPositionMeters.X -= Region.ExtentMeters.X;
		InOutVelocityMetersPerSecond =
			FVector(Params.InflowSpeedMetersPerSecond, 0.0, 0.0);
	}

	void StepParticle(
		const FFluidState& State, const FMassifFluidBakeParams& Params, const FMassifFluidRegion& Region,
		FVector3f& InOutPositionMeters, FVector3f& InOutVelocityMetersPerSecond)
	{
		FVector PositionMeters(InOutPositionMeters);
		const FVector Cells = State.ToCells(PositionMeters);
		FVector VelocityMetersPerSecond =
			BlendedVelocity(State, Params, Cells, FVector(InOutVelocityMetersPerSecond));

		PositionMeters += VelocityMetersPerSecond * Params.StepSeconds;
		SettleOnGround(State, Params, PositionMeters, VelocityMetersPerSecond);
		RecycleOrHold(State, Params, Region, PositionMeters, VelocityMetersPerSecond);

		InOutPositionMeters = FVector3f(PositionMeters);
		InOutVelocityMetersPerSecond = FVector3f(VelocityMetersPerSecond);
	}

	void StepParticles(
		FFluidState& InOutState, const FMassifFluidBakeParams& Params, const FMassifFluidRegion& Region)
	{
		ParallelFor(InOutState.PositionsMeters.Num(),
			[&InOutState, &Params, &Region](int32 ParticleIndex)
		{
			StepParticle(
				InOutState, Params, Region, InOutState.PositionsMeters[ParticleIndex],
				InOutState.VelocitiesMetersPerSecond[ParticleIndex]);
		});
	}

	/** Seconds each phase of a step cost, summed over the whole solve. */
	struct FPhaseTimings
	{
		double ScatterSeconds = 0.0;
		double PressureSeconds = 0.0;
		double AdvectSeconds = 0.0;
	};

	void StepRegion(
		FFluidState& InOutState, const FMassifFluidBakeParams& Params, const FMassifFluidRegion& Region,
		FPhaseTimings& InOutTimings)
	{
		const double ScatterStartSeconds = FPlatformTime::Seconds();
		ClearFields(InOutState);
		GatherParticlesToFields(InOutState);
		NormaliseField(InOutState.AlongX);
		NormaliseField(InOutState.AlongY);
		NormaliseField(InOutState.AlongZ);
		ExtrapolateAll(InOutState, Params.ExtrapolationLayers);

		MarkCells(InOutState);
		AddGravity(InOutState, Params);
		CloseSolidFaces(InOutState);
		SaveBeforePressure(InOutState.AlongX);
		SaveBeforePressure(InOutState.AlongY);
		SaveBeforePressure(InOutState.AlongZ);
		InOutTimings.ScatterSeconds += FPlatformTime::Seconds() - ScatterStartSeconds;

		const double PressureStartSeconds = FPlatformTime::Seconds();
		MeasureDivergence(InOutState);
		SolvePressure(InOutState, Params);
		SubtractPressureGradient(InOutState, Params);
		ExtrapolateAll(InOutState, Params.ExtrapolationLayers);
		InOutTimings.PressureSeconds += FPlatformTime::Seconds() - PressureStartSeconds;

		const double AdvectStartSeconds = FPlatformTime::Seconds();
		StepParticles(InOutState, Params, Region);
		InOutTimings.AdvectSeconds += FPlatformTime::Seconds() - AdvectStartSeconds;
	}

	// -----------------------------------------------------------------------
	// What the region settled into
	// -----------------------------------------------------------------------

	/** Deepest and mean water over each column that has any, from the particles standing in it. */
	void AccumulateDepths(const FFluidState& State, FMassifFluidBakeSummary& OutSummary)
	{
		TArray<float> HighestMeters;
		HighestMeters.Init(-TNumericLimits<float>::Max(), State.CellCounts.X * State.CellCounts.Y);
		for (const FVector3f& PositionMeters : State.PositionsMeters)
		{
			const FVector Cells = State.ToCells(FVector(PositionMeters));
			const int32 X = FMath::Clamp(FMath::FloorToInt32(Cells.X), 0, State.CellCounts.X - 1);
			const int32 Y = FMath::Clamp(FMath::FloorToInt32(Cells.Y), 0, State.CellCounts.Y - 1);
			const int32 Column = State.ColumnIndex(X, Y);
			HighestMeters[Column] = FMath::Max(HighestMeters[Column], PositionMeters.Z);
		}

		int32 WetColumnCount = 0;
		for (int32 Column = 0; Column < HighestMeters.Num(); ++Column)
		{
			if (HighestMeters[Column] <= -TNumericLimits<float>::Max() * 0.5f)
			{
				continue;
			}
			const double DepthMeters = HighestMeters[Column] - State.GroundAltitudeMeters[Column];
			OutSummary.MeanDepthMeters += DepthMeters;
			OutSummary.MaxDepthMeters = FMath::Max(OutSummary.MaxDepthMeters, DepthMeters);
			++WetColumnCount;
		}
		if (WetColumnCount > 0)
		{
			OutSummary.MeanDepthMeters /= WetColumnCount;
		}
		OutSummary.WetVolumeCubicMeters =
			WetColumnCount * FMath::Square(State.CellSizeMeters) * OutSummary.MeanDepthMeters;
	}

	void AccumulateSpeeds(const FFluidState& State, FMassifFluidBakeSummary& OutSummary)
	{
		for (const FVector3f& VelocityMetersPerSecond : State.VelocitiesMetersPerSecond)
		{
			const double SpeedMetersPerSecond = FVector(VelocityMetersPerSecond).Length();
			OutSummary.MeanSpeedMetersPerSecond += SpeedMetersPerSecond;
			OutSummary.MaxSpeedMetersPerSecond =
				FMath::Max(OutSummary.MaxSpeedMetersPerSecond, SpeedMetersPerSecond);
		}
		if (!State.VelocitiesMetersPerSecond.IsEmpty())
		{
			OutSummary.MeanSpeedMetersPerSecond /= State.VelocitiesMetersPerSecond.Num();
		}
	}
}

FMassifFluidBakeSummary MassifBakeFluidRegion(
	const FMassifTerrainField& Terrain,
	const FMassifFluidRegion& Region,
	const FMassifFluidBakeParams& Params,
	TArray<FVector3f>& OutParticlePositionsMeters,
	TArray<FVector3f>& OutParticleVelocitiesMetersPerSecond)
{
	const double StartSeconds = FPlatformTime::Seconds();

	FFluidState State;
	SetupFields(Region, State);
	SampleGround(Terrain, State);
	SeedWater(Region, Params, State);

	FPhaseTimings Timings;
	for (int32 Step = 0; Step < Params.StepCount; ++Step)
	{
		StepRegion(State, Params, Region, Timings);
	}

	FMassifFluidBakeSummary Summary;
	Summary.CellCounts = State.CellCounts;
	Summary.ParticleCount = State.PositionsMeters.Num();
	AccumulateDepths(State, Summary);
	AccumulateSpeeds(State, Summary);
	Summary.SolveSeconds = FPlatformTime::Seconds() - StartSeconds;
	Summary.ScatterSeconds = Timings.ScatterSeconds;
	Summary.PressureSeconds = Timings.PressureSeconds;
	Summary.AdvectSeconds = Timings.AdvectSeconds;

	OutParticlePositionsMeters = MoveTemp(State.PositionsMeters);
	OutParticleVelocitiesMetersPerSecond = MoveTemp(State.VelocitiesMetersPerSecond);
	return Summary;
}
