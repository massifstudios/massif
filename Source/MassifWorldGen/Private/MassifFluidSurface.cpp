#include "MassifFluidSurface.h"

#include "Async/ParallelFor.h"
#include "Generators/MarchingCubes.h"
#include "HAL/PlatformTime.h"

namespace
{
	/**
	 * The particles sorted into the cells they stand in, so a node can be built by
	 * reading the cells around it.
	 *
	 * Gathered rather than scattered. A scatter has every particle adding into the
	 * nodes it reaches, which is a race the moment it is threaded and needs either
	 * locks or per-thread copies of the whole field; a gather has each node reading
	 * and writing only itself, so the whole pass threads with nothing shared.
	 */
	struct FParticleBins
	{
		FIntVector CellCounts = FIntVector::ZeroValue;
		TArray<int32> CellStart;
		TArray<int32> ParticleIndices;

		int32 CellIndex(int32 X, int32 Y, int32 Z) const
		{
			return (Z * CellCounts.Y + Y) * CellCounts.X + X;
		}
	};

	FIntVector CellOf(const FMassifFluidRegion& Region, FIntVector CellCounts, FVector3f PositionMeters)
	{
		const FVector Cells = (FVector(PositionMeters) - Region.OriginMeters) / Region.CellSizeMeters;
		return FIntVector(
			FMath::Clamp(FMath::FloorToInt32(Cells.X), 0, CellCounts.X - 1),
			FMath::Clamp(FMath::FloorToInt32(Cells.Y), 0, CellCounts.Y - 1),
			FMath::Clamp(FMath::FloorToInt32(Cells.Z), 0, CellCounts.Z - 1));
	}

	/** Counting sort: one pass to count, a prefix sum, then one pass to place. */
	FParticleBins BuildBins(
		const FMassifFluidRegion& Region, TArrayView<const FVector3f> PositionsMeters)
	{
		FParticleBins Bins;
		Bins.CellCounts = Region.CellCounts();
		const int32 CellCount = Bins.CellCounts.X * Bins.CellCounts.Y * Bins.CellCounts.Z;

		TArray<int32> CellCountsSoFar;
		CellCountsSoFar.SetNumZeroed(CellCount);
		for (const FVector3f& PositionMeters : PositionsMeters)
		{
			const FIntVector At = CellOf(Region, Bins.CellCounts, PositionMeters);
			++CellCountsSoFar[Bins.CellIndex(At.X, At.Y, At.Z)];
		}

		Bins.CellStart.SetNumUninitialized(CellCount + 1);
		int32 RunningTotal = 0;
		for (int32 Cell = 0; Cell < CellCount; ++Cell)
		{
			Bins.CellStart[Cell] = RunningTotal;
			RunningTotal += CellCountsSoFar[Cell];
			CellCountsSoFar[Cell] = Bins.CellStart[Cell];
		}
		Bins.CellStart[CellCount] = RunningTotal;

		Bins.ParticleIndices.SetNumUninitialized(PositionsMeters.Num());
		for (int32 ParticleIndex = 0; ParticleIndex < PositionsMeters.Num(); ++ParticleIndex)
		{
			const FIntVector At = CellOf(Region, Bins.CellCounts, PositionsMeters[ParticleIndex]);
			Bins.ParticleIndices[CellCountsSoFar[Bins.CellIndex(At.X, At.Y, At.Z)]++] = ParticleIndex;
		}
		return Bins;
	}

	// -----------------------------------------------------------------------
	// The field the surface is the zero crossing of
	// -----------------------------------------------------------------------

	struct FLevelSet
	{
		FIntVector NodeCounts = FIntVector::ZeroValue;
		FVector OriginMeters = FVector::ZeroVector;
		double SpacingMeters = 0.05;
		TArray<float> Values;

		int32 NodeIndex(int32 X, int32 Y, int32 Z) const
		{
			return (Z * NodeCounts.Y + Y) * NodeCounts.X + X;
		}

		FVector NodePositionMeters(int32 X, int32 Y, int32 Z) const
		{
			return OriginMeters + FVector(X, Y, Z) * SpacingMeters;
		}
	};

	/** Falls smoothly to nothing at the kernel's edge, so a node's field has no seam in it. */
	double BlobWeight(double DistanceMeters, double ReachMeters)
	{
		const double Fraction = DistanceMeters / ReachMeters;
		if (Fraction >= 1.0)
		{
			return 0.0;
		}
		const double Falloff = 1.0 - Fraction * Fraction;
		return Falloff * Falloff * Falloff;
	}

	/** What the particles around one node average to, and how much of them there is. */
	struct FNodeGather
	{
		FVector WeightedPositionMeters = FVector::ZeroVector;
		double TotalWeight = 0.0;
	};

	void GatherCell(
		const FParticleBins& Bins, TArrayView<const FVector3f> PositionsMeters, int32 Cell,
		FVector NodePositionMeters, double ReachMeters, FNodeGather& InOutGather)
	{
		for (int32 Slot = Bins.CellStart[Cell]; Slot < Bins.CellStart[Cell + 1]; ++Slot)
		{
			const FVector ParticleMeters(PositionsMeters[Bins.ParticleIndices[Slot]]);
			const double Weight =
				BlobWeight((ParticleMeters - NodePositionMeters).Length(), ReachMeters);
			if (Weight <= 0.0)
			{
				continue;
			}
			InOutGather.WeightedPositionMeters += ParticleMeters * Weight;
			InOutGather.TotalWeight += Weight;
		}
	}

	void GatherSlab(
		const FParticleBins& Bins, TArrayView<const FVector3f> PositionsMeters, FIntVector Lowest,
		FIntVector Highest, int32 Z, FVector NodePositionMeters, double ReachMeters,
		FNodeGather& InOutGather);

	FNodeGather GatherAroundNode(
		const FParticleBins& Bins, TArrayView<const FVector3f> PositionsMeters,
		FIntVector CentreCell, int32 ReachCells, FVector NodePositionMeters, double ReachMeters)
	{
		const FIntVector Lowest(
			FMath::Max(CentreCell.X - ReachCells, 0),
			FMath::Max(CentreCell.Y - ReachCells, 0),
			FMath::Max(CentreCell.Z - ReachCells, 0));
		const FIntVector Highest(
			FMath::Min(CentreCell.X + ReachCells, Bins.CellCounts.X - 1),
			FMath::Min(CentreCell.Y + ReachCells, Bins.CellCounts.Y - 1),
			FMath::Min(CentreCell.Z + ReachCells, Bins.CellCounts.Z - 1));

		FNodeGather Gather;
		for (int32 Z = Lowest.Z; Z <= Highest.Z; ++Z)
		{
			GatherSlab(Bins, PositionsMeters, Lowest, Highest, Z, NodePositionMeters, ReachMeters, Gather);
		}
		return Gather;
	}

	void GatherSlab(
		const FParticleBins& Bins, TArrayView<const FVector3f> PositionsMeters, FIntVector Lowest,
		FIntVector Highest, int32 Z, FVector NodePositionMeters, double ReachMeters,
		FNodeGather& InOutGather)
	{
		for (int32 Y = Lowest.Y; Y <= Highest.Y; ++Y)
		{
			for (int32 X = Lowest.X; X <= Highest.X; ++X)
			{
				GatherCell(
					Bins, PositionsMeters, Bins.CellIndex(X, Y, Z), NodePositionMeters, ReachMeters,
					InOutGather);
			}
		}
	}

	/**
	 * The field at one node: how far it stands from the mean of the water around it,
	 * less the radius that water fills.
	 *
	 * Negative inside the body and positive outside it, and smooth across the
	 * boundary because the mean moves smoothly — which is what separates this from a
	 * union of spheres, whose surface carries every particle as a bump.
	 */
	float NodeValue(
		const FParticleBins& Bins, TArrayView<const FVector3f> PositionsMeters,
		const FMassifFluidSurfaceParams& Params, const FLevelSet& Field, FIntVector Node,
		double ReachMeters, int32 ReachCells, double RadiusMeters)
	{
		const FVector NodePositionMeters = Field.NodePositionMeters(Node.X, Node.Y, Node.Z);
		const FIntVector CentreCell(
			FMath::Clamp(Node.X, 0, Bins.CellCounts.X - 1),
			FMath::Clamp(Node.Y, 0, Bins.CellCounts.Y - 1),
			FMath::Clamp(Node.Z, 0, Bins.CellCounts.Z - 1));

		const FNodeGather Gather = GatherAroundNode(
			Bins, PositionsMeters, CentreCell, ReachCells, NodePositionMeters, ReachMeters);
		if (Gather.TotalWeight <= UE_DOUBLE_SMALL_NUMBER)
		{
			return static_cast<float>(ReachMeters);
		}

		const FVector MeanMeters = Gather.WeightedPositionMeters / Gather.TotalWeight;
		return static_cast<float>((NodePositionMeters - MeanMeters).Length() - RadiusMeters);
	}

	void BuildLevelSet(
		const FMassifFluidRegion& Region, const FMassifFluidSurfaceParams& Params,
		const FParticleBins& Bins, TArrayView<const FVector3f> PositionsMeters, FLevelSet& OutField)
	{
		const FIntVector CellCounts = Region.CellCounts();
		OutField.NodeCounts = CellCounts + FIntVector(1, 1, 1);
		OutField.OriginMeters = Region.OriginMeters;
		OutField.SpacingMeters = Region.CellSizeMeters;
		OutField.Values.SetNumUninitialized(
			OutField.NodeCounts.X * OutField.NodeCounts.Y * OutField.NodeCounts.Z);

		const double ReachMeters = Params.KernelCells * Region.CellSizeMeters;
		const int32 ReachCells = FMath::CeilToInt32(Params.KernelCells);
		const double RadiusMeters = Params.ParticleRadiusFactor * Params.ParticleSpacingMeters;

		ParallelFor(OutField.NodeCounts.Z, [&](int32 Z)
		{
			for (int32 Y = 0; Y < OutField.NodeCounts.Y; ++Y)
			{
				for (int32 X = 0; X < OutField.NodeCounts.X; ++X)
				{
					OutField.Values[OutField.NodeIndex(X, Y, Z)] = NodeValue(
						Bins, PositionsMeters, Params, OutField, FIntVector(X, Y, Z), ReachMeters,
						ReachCells, RadiusMeters);
				}
			}
		});
	}

	// -----------------------------------------------------------------------
	// Smoothing, and reading the field back
	// -----------------------------------------------------------------------

	float SmoothedNode(const FLevelSet& Field, const TArray<float>& Values, int32 X, int32 Y, int32 Z)
	{
		const FIntVector Steps[] = {
			FIntVector(-1, 0, 0), FIntVector(1, 0, 0), FIntVector(0, -1, 0),
			FIntVector(0, 1, 0), FIntVector(0, 0, -1), FIntVector(0, 0, 1) };
		double TotalMeters = 0.0;
		int32 NeighbourCount = 0;
		for (const FIntVector& Step : Steps)
		{
			const FIntVector At(X + Step.X, Y + Step.Y, Z + Step.Z);
			if (At.X < 0 || At.Y < 0 || At.Z < 0 || At.X >= Field.NodeCounts.X
				|| At.Y >= Field.NodeCounts.Y || At.Z >= Field.NodeCounts.Z)
			{
				continue;
			}
			TotalMeters += Values[Field.NodeIndex(At.X, At.Y, At.Z)];
			++NeighbourCount;
		}
		if (NeighbourCount == 0)
		{
			return Values[Field.NodeIndex(X, Y, Z)];
		}
		const double HereMeters = Values[Field.NodeIndex(X, Y, Z)];
		return static_cast<float>(0.5 * HereMeters + 0.5 * TotalMeters / NeighbourCount);
	}

	void SmoothLevelSet(FLevelSet& InOutField, int32 PassCount)
	{
		TArray<float> Scratch;
		for (int32 Pass = 0; Pass < PassCount; ++Pass)
		{
			Scratch = InOutField.Values;
			ParallelFor(InOutField.NodeCounts.Z, [&](int32 Z)
			{
				for (int32 Y = 0; Y < InOutField.NodeCounts.Y; ++Y)
				{
					for (int32 X = 0; X < InOutField.NodeCounts.X; ++X)
					{
						InOutField.Values[InOutField.NodeIndex(X, Y, Z)] =
							SmoothedNode(InOutField, Scratch, X, Y, Z);
					}
				}
			});
		}
	}

	double SampleLevelSet(const FLevelSet& Field, FVector PositionMeters)
	{
		const FVector Nodes = (PositionMeters - Field.OriginMeters) / Field.SpacingMeters;
		const FIntVector Corner(
			FMath::FloorToInt32(Nodes.X), FMath::FloorToInt32(Nodes.Y), FMath::FloorToInt32(Nodes.Z));
		const FVector Fraction = Nodes - FVector(Corner);

		double Total = 0.0;
		for (int32 CornerIndex = 0; CornerIndex < 8; ++CornerIndex)
		{
			const FIntVector Step(CornerIndex & 1, (CornerIndex >> 1) & 1, (CornerIndex >> 2) & 1);
			const int32 X = FMath::Clamp(Corner.X + Step.X, 0, Field.NodeCounts.X - 1);
			const int32 Y = FMath::Clamp(Corner.Y + Step.Y, 0, Field.NodeCounts.Y - 1);
			const int32 Z = FMath::Clamp(Corner.Z + Step.Z, 0, Field.NodeCounts.Z - 1);
			const double WeightX = Step.X == 0 ? 1.0 - Fraction.X : Fraction.X;
			const double WeightY = Step.Y == 0 ? 1.0 - Fraction.Y : Fraction.Y;
			const double WeightZ = Step.Z == 0 ? 1.0 - Fraction.Z : Fraction.Z;
			Total += WeightX * WeightY * WeightZ * Field.Values[Field.NodeIndex(X, Y, Z)];
		}
		return Total;
	}

	/** Straight off the field's own gradient, which is smoother than anything the triangles carry. */
	FVector3f FieldNormalAt(const FLevelSet& Field, FVector PositionMeters)
	{
		const double StepMeters = 0.5 * Field.SpacingMeters;
		const FVector Gradient(
			SampleLevelSet(Field, PositionMeters + FVector(StepMeters, 0.0, 0.0))
				- SampleLevelSet(Field, PositionMeters - FVector(StepMeters, 0.0, 0.0)),
			SampleLevelSet(Field, PositionMeters + FVector(0.0, StepMeters, 0.0))
				- SampleLevelSet(Field, PositionMeters - FVector(0.0, StepMeters, 0.0)),
			SampleLevelSet(Field, PositionMeters + FVector(0.0, 0.0, StepMeters))
				- SampleLevelSet(Field, PositionMeters - FVector(0.0, 0.0, StepMeters)));
		return FVector3f(Gradient.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector));
	}

	// -----------------------------------------------------------------------
	// Meshing the crossing
	// -----------------------------------------------------------------------

	void MeshLevelSet(
		const FMassifFluidRegion& Region, const FLevelSet& Field, FMassifFluidSurfaceMesh& OutMesh)
	{
		UE::Geometry::FMarchingCubes Cubes;
		Cubes.CubeSize = Region.CellSizeMeters;
		Cubes.IsoValue = 0.0;
		Cubes.Bounds = UE::Geometry::FAxisAlignedBox3d(
			Region.OriginMeters, Region.OriginMeters + Region.ExtentMeters);
		Cubes.RootMode = UE::Geometry::ERootfindingModes::SingleLerp;
		Cubes.bParallelCompute = true;
		Cubes.Implicit = [&Field](UE::Math::TVector<double> PositionMeters)
		{
			return SampleLevelSet(Field, FVector(PositionMeters));
		};
		Cubes.Generate();

		OutMesh.PositionsMeters.SetNumUninitialized(Cubes.Vertices.Num());
		OutMesh.Normals.SetNumUninitialized(Cubes.Vertices.Num());
		ParallelFor(Cubes.Vertices.Num(), [&](int32 VertexIndex)
		{
			const FVector PositionMeters(Cubes.Vertices[VertexIndex]);
			OutMesh.PositionsMeters[VertexIndex] = FVector3f(PositionMeters);
			OutMesh.Normals[VertexIndex] = FieldNormalAt(Field, PositionMeters);
		});

		OutMesh.Triangles.SetNumUninitialized(Cubes.Triangles.Num());
		for (int32 TriangleIndex = 0; TriangleIndex < Cubes.Triangles.Num(); ++TriangleIndex)
		{
			const UE::Geometry::FIndex3i Triangle = Cubes.Triangles[TriangleIndex];
			OutMesh.Triangles[TriangleIndex] = FIntVector(Triangle.A, Triangle.B, Triangle.C);
		}
	}
}

FMassifFluidSurfaceMesh MassifBuildFluidSurface(
	const FMassifFluidRegion& Region,
	const FMassifFluidSurfaceParams& Params,
	TArrayView<const FVector3f> ParticlePositionsMeters)
{
	const double StartSeconds = FPlatformTime::Seconds();

	FMassifFluidSurfaceMesh Mesh;
	if (ParticlePositionsMeters.IsEmpty())
	{
		return Mesh;
	}

	const FParticleBins Bins = BuildBins(Region, ParticlePositionsMeters);

	FLevelSet Field;
	BuildLevelSet(Region, Params, Bins, ParticlePositionsMeters, Field);
	SmoothLevelSet(Field, Params.SmoothingPasses);
	MeshLevelSet(Region, Field, Mesh);

	Mesh.BuildSeconds = FPlatformTime::Seconds() - StartSeconds;
	return Mesh;
}
