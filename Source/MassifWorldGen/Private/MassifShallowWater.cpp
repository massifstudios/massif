#include "MassifShallowWater.h"

#include "Async/ParallelFor.h"
#include "HAL/PlatformTime.h"
#include "MassifRiverField.h"
#include "MassifTerrainField.h"

namespace
{
	/** The four faces of a cell, in the order the flux array holds them. */
	enum class EFace : uint8
	{
		West,
		East,
		South,
		North,
	};

	constexpr int32 FaceCount = 4;

	constexpr int32 FaceStepX[FaceCount] = { -1, 1, 0, 0 };
	constexpr int32 FaceStepY[FaceCount] = { 0, 0, -1, 1 };

	/** The face of the neighbour that looks back through the same gap. */
	constexpr int32 OppositeFace[FaceCount] = { 1, 0, 3, 2 };

	/**
	 * The solve's own working state.
	 *
	 * Flux is held per cell per face rather than per face, so a cell owns the four
	 * numbers it writes and no two cells write the same one. That is what lets
	 * every pass be split across threads without a lock and still land on the same
	 * answer whatever order it ran in.
	 */
	struct FShallowWaterState
	{
		FIntPoint CellCounts = FIntPoint::ZeroValue;
		double CellSizeMeters = 0.25;

		TArray<float> BedAltitudeMeters;
		TArray<float> DepthMeters;
		TArray<float> NextDepthMeters;

		/** Outflow through each of a cell's four faces, in cubic metres per second. */
		TArray<float> FaceFlux;

		/**
		 * Cells held at the surface the channel solve gave them: the band one cell
		 * wide around the patch. Water enters over the upstream part of it and
		 * leaves over the downstream part without either being named as such.
		 */
		TArray<uint8> bHeld;
		TArray<float> HeldDepthMeters;

		int32 CellIndex(int32 Column, int32 Row) const
		{
			return Row * CellCounts.X + Column;
		}

		bool Contains(int32 Column, int32 Row) const
		{
			return Column >= 0 && Row >= 0 && Column < CellCounts.X && Row < CellCounts.Y;
		}

		int32 CellCount() const
		{
			return CellCounts.X * CellCounts.Y;
		}

		double SurfaceAltitudeMeters(int32 Cell) const
		{
			return static_cast<double>(BedAltitudeMeters[Cell]) + DepthMeters[Cell];
		}
	};

	// -----------------------------------------------------------------------
	// Setting the patch up
	// -----------------------------------------------------------------------

	FIntPoint MeasureCellCounts(const FMassifShallowWaterRegion& Region)
	{
		return FIntPoint(
			FMath::Max(FMath::FloorToInt32(Region.ExtentMeters.X / Region.CellSizeMeters), 1),
			FMath::Max(FMath::FloorToInt32(Region.ExtentMeters.Y / Region.CellSizeMeters), 1));
	}

	FVector2d CellCentreMeters(const FMassifShallowWaterRegion& Region, int32 Column, int32 Row)
	{
		return Region.OriginMeters
			+ (FVector2d(Column, Row) + FVector2d(0.5, 0.5)) * Region.CellSizeMeters;
	}

	/** Whether a cell sits in the band the channel's own surface is held over. */
	bool IsOnPatchEdge(const FShallowWaterState& State, int32 Column, int32 Row)
	{
		return Column == 0 || Row == 0 || Column == State.CellCounts.X - 1
			|| Row == State.CellCounts.Y - 1;
	}

	/**
	 * The ground and the water the channel solve put over one column.
	 *
	 * The bed is the terrain with the trench already cut into it, so the patch is
	 * solved over the same ground the player walks on rather than over a second
	 * version of it.
	 */
	void SeedColumn(
		const FMassifTerrainField& Terrain, const FMassifRiverField& Rivers, int32 TimeSlice,
		const FMassifShallowWaterRegion& Region, FShallowWaterState& InOutState, int32 Column, int32 Row)
	{
		const FVector2d PositionMeters = CellCentreMeters(Region, Column, Row);
		const int32 Cell = InOutState.CellIndex(Column, Row);

		const double BedMeters = Terrain.SampleHeightMeters(PositionMeters, Region.CellSizeMeters);
		InOutState.BedAltitudeMeters[Cell] = static_cast<float>(BedMeters);

		const FMassifRiverSample River = Rivers.Sample(PositionMeters, TimeSlice);
		double DepthMeters = 0.0;
		if (River.IsInChannel())
		{
			DepthMeters = FMath::Max(River.WaterSurfaceMeters - BedMeters, 0.0);
		}

		InOutState.DepthMeters[Cell] = static_cast<float>(DepthMeters);
		InOutState.HeldDepthMeters[Cell] = static_cast<float>(DepthMeters);
		InOutState.bHeld[Cell] = IsOnPatchEdge(InOutState, Column, Row) ? 1 : 0;
	}

	void SeedPatch(
		const FMassifTerrainField& Terrain, const FMassifRiverField& Rivers, int32 TimeSlice,
		const FMassifShallowWaterRegion& Region, FShallowWaterState& InOutState)
	{
		ParallelFor(InOutState.CellCounts.Y, [&](int32 Row)
		{
			for (int32 Column = 0; Column < InOutState.CellCounts.X; ++Column)
			{
				SeedColumn(Terrain, Rivers, TimeSlice, Region, InOutState, Column, Row);
			}
		});
	}

	void SetupState(const FMassifShallowWaterRegion& Region, FShallowWaterState& OutState)
	{
		OutState.CellCounts = MeasureCellCounts(Region);
		OutState.CellSizeMeters = Region.CellSizeMeters;

		const int32 CellCount = OutState.CellCount();
		OutState.BedAltitudeMeters.SetNumZeroed(CellCount);
		OutState.DepthMeters.SetNumZeroed(CellCount);
		OutState.NextDepthMeters.SetNumZeroed(CellCount);
		OutState.FaceFlux.SetNumZeroed(CellCount * FaceCount);
		OutState.bHeld.SetNumZeroed(CellCount);
		OutState.HeldDepthMeters.SetNumZeroed(CellCount);
	}

	// -----------------------------------------------------------------------
	// What the head across a face drives through it
	// -----------------------------------------------------------------------

	/**
	 * How much water stands in the gap between two cells.
	 *
	 * The lower of the two surfaces over the higher of the two beds, which is what
	 * is actually free to move: a full cell beside an empty one whose bed stands
	 * above the water in it has nothing between them however deep the full one is,
	 * and a scheme that used the mean depth instead would drive water uphill over
	 * the lip.
	 */
	double FaceDepthMeters(const FShallowWaterState& State, int32 Cell, int32 NeighbourCell)
	{
		const double SurfaceMeters =
			FMath::Min(State.SurfaceAltitudeMeters(Cell), State.SurfaceAltitudeMeters(NeighbourCell));
		const double BedMeters = FMath::Max(
			static_cast<double>(State.BedAltitudeMeters[Cell]),
			static_cast<double>(State.BedAltitudeMeters[NeighbourCell]));
		return FMath::Max(SurfaceMeters - BedMeters, 0.0);
	}

	/**
	 * Manning's drag over one step, as the share of a face's flow that survives it.
	 *
	 * Implicit in the speed — the drag is measured against the speed after it
	 * rather than before — because taken explicitly a shallow fast face is slowed
	 * by more than it was moving and turns round.
	 */
	double SurvivingShare(
		const FMassifShallowWaterParams& Params, double SpeedMetersPerSecond, double DepthMeters)
	{
		if (DepthMeters <= UE_DOUBLE_SMALL_NUMBER)
		{
			return 0.0;
		}

		const double DragPerSecond = Params.GravityMetersPerSecondSquared
			* FMath::Square(Params.Roughness) * FMath::Abs(SpeedMetersPerSecond)
			/ FMath::Pow(DepthMeters, 4.0 / 3.0);
		return 1.0 / (1.0 + Params.StepSeconds * DragPerSecond);
	}

	/** One face's outflow after this step's head, its own inertia, and the bed's drag. */
	double SolveFaceFlux(
		const FShallowWaterState& State, const FMassifShallowWaterParams& Params, int32 Cell,
		int32 NeighbourCell, double PreviousFlux)
	{
		const double DepthMeters = FaceDepthMeters(State, Cell, NeighbourCell);
		if (DepthMeters <= UE_DOUBLE_SMALL_NUMBER)
		{
			return 0.0;
		}

		const double HeadMeters =
			State.SurfaceAltitudeMeters(Cell) - State.SurfaceAltitudeMeters(NeighbourCell);
		const double AreaSquareMeters = DepthMeters * State.CellSizeMeters;
		const double DrivenFlux = PreviousFlux
			+ Params.StepSeconds * Params.GravityMetersPerSecondSquared * AreaSquareMeters * HeadMeters
				/ State.CellSizeMeters;
		if (DrivenFlux <= 0.0)
		{
			return 0.0;
		}

		const double SpeedMetersPerSecond = DrivenFlux / AreaSquareMeters;
		const double DampedFlux = DrivenFlux * SurvivingShare(Params, SpeedMetersPerSecond, DepthMeters);
		return FMath::Min(DampedFlux, Params.MaxSpeedMetersPerSecond * AreaSquareMeters);
	}

	/**
	 * Holds a cell's four outflows to what it actually has to give.
	 *
	 * Scaled together rather than clipped one at a time, so a cell emptying into
	 * three neighbours at once shares what it has between them in the proportion
	 * the heads asked for. This is the whole of what keeps depth positive: no cell
	 * can be asked for water it does not hold, whatever the step length.
	 */
	void HoldFluxToWhatIsThere(
		FShallowWaterState& InOutState, const FMassifShallowWaterParams& Params, int32 Cell)
	{
		double TotalOutflow = 0.0;
		for (int32 Face = 0; Face < FaceCount; ++Face)
		{
			TotalOutflow += InOutState.FaceFlux[Cell * FaceCount + Face];
		}

		const double AvailableCubicMeters =
			InOutState.DepthMeters[Cell] * FMath::Square(InOutState.CellSizeMeters);
		const double AskedCubicMeters = TotalOutflow * Params.StepSeconds;
		if (AskedCubicMeters <= AvailableCubicMeters || AskedCubicMeters <= UE_DOUBLE_SMALL_NUMBER)
		{
			return;
		}

		const double Share = AvailableCubicMeters / AskedCubicMeters;
		for (int32 Face = 0; Face < FaceCount; ++Face)
		{
			InOutState.FaceFlux[Cell * FaceCount + Face] *= static_cast<float>(Share);
		}
	}

	void SolveFluxAtCell(
		FShallowWaterState& InOutState, const FMassifShallowWaterParams& Params, int32 Column, int32 Row)
	{
		const int32 Cell = InOutState.CellIndex(Column, Row);
		for (int32 Face = 0; Face < FaceCount; ++Face)
		{
			const int32 NeighbourColumn = Column + FaceStepX[Face];
			const int32 NeighbourRow = Row + FaceStepY[Face];
			if (!InOutState.Contains(NeighbourColumn, NeighbourRow))
			{
				InOutState.FaceFlux[Cell * FaceCount + Face] = 0.0f;
				continue;
			}

			InOutState.FaceFlux[Cell * FaceCount + Face] = static_cast<float>(SolveFaceFlux(
				InOutState, Params, Cell, InOutState.CellIndex(NeighbourColumn, NeighbourRow),
				InOutState.FaceFlux[Cell * FaceCount + Face]));
		}
		HoldFluxToWhatIsThere(InOutState, Params, Cell);
	}

	void SolveFlux(FShallowWaterState& InOutState, const FMassifShallowWaterParams& Params)
	{
		ParallelFor(InOutState.CellCounts.Y, [&InOutState, &Params](int32 Row)
		{
			for (int32 Column = 0; Column < InOutState.CellCounts.X; ++Column)
			{
				SolveFluxAtCell(InOutState, Params, Column, Row);
			}
		});
	}

	// -----------------------------------------------------------------------
	// What that leaves in each cell
	// -----------------------------------------------------------------------

	/** What crosses into a cell from its neighbours, less what it sends them. */
	double NetInflowCubicMetersPerSecond(const FShallowWaterState& State, int32 Column, int32 Row)
	{
		const int32 Cell = State.CellIndex(Column, Row);
		double NetFlux = 0.0;
		for (int32 Face = 0; Face < FaceCount; ++Face)
		{
			NetFlux -= State.FaceFlux[Cell * FaceCount + Face];

			const int32 NeighbourColumn = Column + FaceStepX[Face];
			const int32 NeighbourRow = Row + FaceStepY[Face];
			if (!State.Contains(NeighbourColumn, NeighbourRow))
			{
				continue;
			}

			const int32 NeighbourCell = State.CellIndex(NeighbourColumn, NeighbourRow);
			NetFlux += State.FaceFlux[NeighbourCell * FaceCount + OppositeFace[Face]];
		}
		return NetFlux;
	}

	void SolveDepthAtCell(
		FShallowWaterState& InOutState, const FMassifShallowWaterParams& Params, int32 Column, int32 Row)
	{
		const int32 Cell = InOutState.CellIndex(Column, Row);
		if (InOutState.bHeld[Cell] != 0)
		{
			InOutState.NextDepthMeters[Cell] = InOutState.HeldDepthMeters[Cell];
			return;
		}

		const double ChangeMeters = NetInflowCubicMetersPerSecond(InOutState, Column, Row)
			* Params.StepSeconds / FMath::Square(InOutState.CellSizeMeters);
		InOutState.NextDepthMeters[Cell] =
			static_cast<float>(FMath::Max(InOutState.DepthMeters[Cell] + ChangeMeters, 0.0));
	}

	void SolveDepth(FShallowWaterState& InOutState, const FMassifShallowWaterParams& Params)
	{
		ParallelFor(InOutState.CellCounts.Y, [&InOutState, &Params](int32 Row)
		{
			for (int32 Column = 0; Column < InOutState.CellCounts.X; ++Column)
			{
				SolveDepthAtCell(InOutState, Params, Column, Row);
			}
		});
		Swap(InOutState.DepthMeters, InOutState.NextDepthMeters);
	}

	void StepPatch(FShallowWaterState& InOutState, const FMassifShallowWaterParams& Params)
	{
		SolveFlux(InOutState, Params);
		SolveDepth(InOutState, Params);
	}

	// -----------------------------------------------------------------------
	// Reading the answer back
	// -----------------------------------------------------------------------

	/**
	 * The velocity a cell's own flow works out to: what crosses it each way over
	 * the section it crosses.
	 *
	 * Taken across the cell rather than at one face, so a cell with water entering
	 * one side and leaving the other reports the speed through it and not the
	 * difference between two faces that agree.
	 */
	FVector2f CellVelocity(const FShallowWaterState& State, int32 Column, int32 Row)
	{
		const int32 Cell = State.CellIndex(Column, Row);
		if (State.DepthMeters[Cell] <= UE_SMALL_NUMBER)
		{
			return FVector2f::ZeroVector;
		}

		double AlongXFlux = 0.0;
		double AlongYFlux = 0.0;
		for (int32 Face = 0; Face < FaceCount; ++Face)
		{
			double Flux = State.FaceFlux[Cell * FaceCount + Face];
			const int32 NeighbourColumn = Column + FaceStepX[Face];
			const int32 NeighbourRow = Row + FaceStepY[Face];
			if (State.Contains(NeighbourColumn, NeighbourRow))
			{
				const int32 NeighbourCell = State.CellIndex(NeighbourColumn, NeighbourRow);
				Flux -= State.FaceFlux[NeighbourCell * FaceCount + OppositeFace[Face]];
			}

			AlongXFlux += 0.5 * Flux * FaceStepX[Face];
			AlongYFlux += 0.5 * Flux * FaceStepY[Face];
		}

		const double SectionSquareMeters = State.DepthMeters[Cell] * State.CellSizeMeters;
		return FVector2f(
			static_cast<float>(AlongXFlux / SectionSquareMeters),
			static_cast<float>(AlongYFlux / SectionSquareMeters));
	}

	void ReadVelocities(const FShallowWaterState& State, FMassifShallowWaterField& OutField)
	{
		OutField.VelocityMetersPerSecond.SetNumUninitialized(State.CellCount());
		ParallelFor(State.CellCounts.Y, [&State, &OutField](int32 Row)
		{
			for (int32 Column = 0; Column < State.CellCounts.X; ++Column)
			{
				OutField.VelocityMetersPerSecond[State.CellIndex(Column, Row)] =
					CellVelocity(State, Column, Row);
			}
		});
	}

	void MeasurePatch(
		const FMassifShallowWaterField& Field, const FMassifShallowWaterParams& Params,
		FMassifShallowWaterSummary& OutSummary)
	{
		for (int32 Cell = 0; Cell < Field.DepthMeters.Num(); ++Cell)
		{
			if (Field.DepthMeters[Cell] < Params.MinWetDepthMeters)
			{
				continue;
			}

			const double SpeedMetersPerSecond = FVector2d(Field.VelocityMetersPerSecond[Cell]).Length();
			OutSummary.MeanDepthMeters += Field.DepthMeters[Cell];
			OutSummary.MaxDepthMeters = FMath::Max(OutSummary.MaxDepthMeters, Field.DepthMeters[Cell]);
			OutSummary.MeanSpeedMetersPerSecond += SpeedMetersPerSecond;
			OutSummary.MaxSpeedMetersPerSecond =
				FMath::Max(OutSummary.MaxSpeedMetersPerSecond, SpeedMetersPerSecond);
			++OutSummary.WetCellCount;
		}

		if (OutSummary.WetCellCount == 0)
		{
			return;
		}

		OutSummary.MeanDepthMeters /= OutSummary.WetCellCount;
		OutSummary.MeanSpeedMetersPerSecond /= OutSummary.WetCellCount;
		OutSummary.WetAreaSquareMeters = OutSummary.WetCellCount * FMath::Square(Field.CellSizeMeters);
	}
}

FMassifShallowWaterSummary MassifBakeShallowWater(
	const FMassifTerrainField& Terrain,
	const FMassifRiverField& Rivers,
	int32 TimeSlice,
	const FMassifShallowWaterRegion& Region,
	const FMassifShallowWaterParams& Params,
	FMassifShallowWaterField& OutField)
{
	const double StartSeconds = FPlatformTime::Seconds();

	FShallowWaterState State;
	SetupState(Region, State);
	SeedPatch(Terrain, Rivers, TimeSlice, Region, State);

	for (int32 Step = 0; Step < Params.StepCount; ++Step)
	{
		StepPatch(State, Params);
	}

	OutField.CellCounts = State.CellCounts;
	OutField.CellSizeMeters = State.CellSizeMeters;
	OutField.OriginMeters = Region.OriginMeters;
	// Before the state is emptied into the field: the velocity is worked out from
	// the depth and the fluxes together.
	ReadVelocities(State, OutField);
	OutField.BedAltitudeMeters = MoveTemp(State.BedAltitudeMeters);
	OutField.DepthMeters = MoveTemp(State.DepthMeters);

	FMassifShallowWaterSummary Summary;
	Summary.CellCounts = OutField.CellCounts;
	MeasurePatch(OutField, Params, Summary);
	Summary.SolveSeconds = FPlatformTime::Seconds() - StartSeconds;
	return Summary;
}

// ---------------------------------------------------------------------------
// The surface as triangles
// ---------------------------------------------------------------------------

namespace
{
	/** Reference the material reads depth and speed against, so it works in fractions. */
	constexpr double DepthReferenceMeters = 2.0;
	constexpr double SpeedReferenceMetersPerSecond = 4.0;

	/** What one lattice node takes from the four cells that meet at it. */
	struct FNodeGather
	{
		double DepthMeters = 0.0;
		double BedAltitudeMeters = 0.0;
		double SpeedMetersPerSecond = 0.0;
		int32 CellsCounted = 0;
		int32 WetCellsCounted = 0;
	};

	void GatherCellIntoNode(
		const FMassifShallowWaterField& Field, const FMassifShallowWaterParams& Params, int32 Column,
		int32 Row, FNodeGather& InOutGather)
	{
		if (!Field.Contains(Column, Row))
		{
			return;
		}

		const int32 Cell = Field.CellIndex(Column, Row);
		InOutGather.BedAltitudeMeters += Field.BedAltitudeMeters[Cell];
		++InOutGather.CellsCounted;

		// Dry cells count towards the mean depth as nothing, which is what thins
		// the sheet to the ground at its own edge rather than ending it on a wall.
		if (Field.DepthMeters[Cell] < Params.MinWetDepthMeters)
		{
			return;
		}

		InOutGather.DepthMeters += Field.DepthMeters[Cell];
		InOutGather.SpeedMetersPerSecond += FVector2d(Field.VelocityMetersPerSecond[Cell]).Length();
		++InOutGather.WetCellsCounted;
	}

	FNodeGather GatherNode(
		const FMassifShallowWaterField& Field, const FMassifShallowWaterParams& Params, int32 NodeX,
		int32 NodeY)
	{
		FNodeGather Gather;
		GatherCellIntoNode(Field, Params, NodeX - 1, NodeY - 1, Gather);
		GatherCellIntoNode(Field, Params, NodeX, NodeY - 1, Gather);
		GatherCellIntoNode(Field, Params, NodeX - 1, NodeY, Gather);
		GatherCellIntoNode(Field, Params, NodeX, NodeY, Gather);
		return Gather;
	}

	/** Whether any of the four cells meeting at a quad's corner has water in it. */
	bool IsQuadWet(
		const FMassifShallowWaterField& Field, const FMassifShallowWaterParams& Params, int32 Column,
		int32 Row)
	{
		return Field.DepthMeters[Field.CellIndex(Column, Row)] >= Params.MinWetDepthMeters;
	}

	void AppendNodes(
		const FMassifShallowWaterField& Field, const FMassifShallowWaterParams& Params,
		FMassifShallowWaterMesh& OutMesh)
	{
		const int32 NodesX = Field.CellCounts.X + 1;
		const int32 NodesY = Field.CellCounts.Y + 1;
		OutMesh.PositionsMeters.SetNumUninitialized(NodesX * NodesY);
		OutMesh.DepthAndSpeed.SetNumUninitialized(NodesX * NodesY);

		for (int32 NodeY = 0; NodeY < NodesY; ++NodeY)
		{
			for (int32 NodeX = 0; NodeX < NodesX; ++NodeX)
			{
				const FNodeGather Gather = GatherNode(Field, Params, NodeX, NodeY);
				const int32 NodeIndex = NodeY * NodesX + NodeX;
				const FVector2d PositionMeters = Field.NodePositionMeters(NodeX, NodeY);
				const double CellsCounted = FMath::Max(Gather.CellsCounted, 1);
				const double WetCellsCounted = FMath::Max(Gather.WetCellsCounted, 1);

				OutMesh.PositionsMeters[NodeIndex] = FVector3f(
					static_cast<float>(PositionMeters.X),
					static_cast<float>(PositionMeters.Y),
					static_cast<float>(
						Gather.BedAltitudeMeters / CellsCounted + Gather.DepthMeters / CellsCounted));
				OutMesh.DepthAndSpeed[NodeIndex] = FVector2f(
					static_cast<float>(
						FMath::Clamp(Gather.DepthMeters / WetCellsCounted / DepthReferenceMeters, 0.0, 1.0)),
					static_cast<float>(FMath::Clamp(
						Gather.SpeedMetersPerSecond / WetCellsCounted / SpeedReferenceMetersPerSecond,
						0.0, 1.0)));
			}
		}
	}

	void AppendTriangles(
		const FMassifShallowWaterField& Field, const FMassifShallowWaterParams& Params,
		FMassifShallowWaterMesh& OutMesh)
	{
		const int32 NodesX = Field.CellCounts.X + 1;
		for (int32 Row = 0; Row < Field.CellCounts.Y; ++Row)
		{
			for (int32 Column = 0; Column < Field.CellCounts.X; ++Column)
			{
				if (!IsQuadWet(Field, Params, Column, Row))
				{
					continue;
				}

				const int32 LowLeft = Row * NodesX + Column;
				OutMesh.Triangles.Add(FIntVector(LowLeft, LowLeft + 1, LowLeft + NodesX + 1));
				OutMesh.Triangles.Add(FIntVector(LowLeft, LowLeft + NodesX + 1, LowLeft + NodesX));
			}
		}
	}

	/**
	 * Off the surface the nodes describe, differenced over one cell.
	 *
	 * A water surface is nearly level, so a normal taken from the triangles alone
	 * is a facet per quad; taken from the height either side it is the shape of
	 * the standing wave, which is the whole of what makes moving water read as
	 * moving.
	 */
	void AppendNormals(const FMassifShallowWaterField& Field, FMassifShallowWaterMesh& OutMesh)
	{
		const int32 NodesX = Field.CellCounts.X + 1;
		const int32 NodesY = Field.CellCounts.Y + 1;
		OutMesh.Normals.SetNumUninitialized(NodesX * NodesY);

		for (int32 NodeY = 0; NodeY < NodesY; ++NodeY)
		{
			for (int32 NodeX = 0; NodeX < NodesX; ++NodeX)
			{
				const int32 West = NodeY * NodesX + FMath::Max(NodeX - 1, 0);
				const int32 East = NodeY * NodesX + FMath::Min(NodeX + 1, NodesX - 1);
				const int32 South = FMath::Max(NodeY - 1, 0) * NodesX + NodeX;
				const int32 North = FMath::Min(NodeY + 1, NodesY - 1) * NodesX + NodeX;

				const FVector3d Normal(
					OutMesh.PositionsMeters[West].Z - OutMesh.PositionsMeters[East].Z,
					OutMesh.PositionsMeters[South].Z - OutMesh.PositionsMeters[North].Z,
					2.0 * Field.CellSizeMeters);
				OutMesh.Normals[NodeY * NodesX + NodeX] =
					FVector3f(Normal.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector3d::UpVector));
			}
		}
	}
}

FMassifShallowWaterMesh MassifBuildShallowWaterSurface(
	const FMassifShallowWaterField& Field, const FMassifShallowWaterParams& Params)
{
	const double StartSeconds = FPlatformTime::Seconds();

	FMassifShallowWaterMesh Mesh;
	if (Field.DepthMeters.IsEmpty())
	{
		return Mesh;
	}

	AppendNodes(Field, Params, Mesh);
	AppendNormals(Field, Mesh);
	AppendTriangles(Field, Params, Mesh);

	Mesh.BuildSeconds = FPlatformTime::Seconds() - StartSeconds;
	return Mesh;
}
