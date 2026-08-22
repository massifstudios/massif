#pragma once

#include "CoreMinimal.h"
#include "MassifEnvironmentProcess.h"

class FMassifEnvironmentScheduler;

/** How far one channel moved between two fidelities of the process that feeds it. */
struct FMassifChannelDifference
{
	FName Name;
	int32 CellCount = 0;

	/** Over every cell, so a channel that moved a little everywhere reads differently from one spike. */
	double MeanAbsoluteDifference = 0.0;

	double LargestAbsoluteDifference = 0.0;
	FVector2d LargestDifferencePositionMeters = FVector2d::ZeroVector;

	/** Cells that differ at all, which separates a rounding change from a structural one. */
	double ChangedCellFraction = 0.0;
};

/**
 * One process solved twice, and what the graph downstream of it made of each.
 *
 * Answers whether an expensive model is doing anything the cheap one did not,
 * with numbers rather than two screenshots and a memory of the first
 * (ARCHITECTURE §5.7).
 */
struct FMassifFidelityDifference
{
	FName ProcessName;
	EMassifFidelity FidelityA = EMassifFidelity::Off;
	EMassifFidelity FidelityB = EMassifFidelity::Off;

	/** False where the process does not implement both levels; the channels are then empty. */
	bool bComparable = false;

	TArray<FMassifChannelDifference> Channels;

	const FMassifChannelDifference* FindChannel(FName ChannelName) const;

	/** Whether the two solves produced the same world, cell for cell. */
	bool IsIdentical() const;
};

namespace MassifEnvironmentDiff
{
	/**
	 * Solves the graph at each level and compares every channel, on the calling
	 * thread. The process is left at the level it started at, re-solved, so a
	 * comparison is not a way to change the world by accident.
	 */
	FMassifFidelityDifference Compare(
		FMassifEnvironmentScheduler& Scheduler,
		FName ProcessName,
		EMassifFidelity FidelityA,
		EMassifFidelity FidelityB);

	void Log(const FMassifFidelityDifference& Difference);
}
