#pragma once

#include "CoreMinimal.h"

/**
 * Smooth value noise on a unit lattice, hashed rather than tabulated so it is
 * seedable and holds no state: the same seed and position answer the same
 * anywhere, in any order, on any thread.
 *
 * Positions are in lattice units — divide a world position by the wavelength
 * the caller wants before sampling.
 */
namespace MassifNoise
{
	inline double LatticeValue(uint32 Seed, int32 CellX, int32 CellY)
	{
		const uint32 Hash = HashCombine(HashCombine(Seed, GetTypeHash(CellX)), GetTypeHash(CellY));
		return Hash / static_cast<double>(TNumericLimits<uint32>::Max());
	}

	inline double SmoothFraction(double Fraction)
	{
		return Fraction * Fraction * (3.0 - 2.0 * Fraction);
	}

	/** 0 to 1, smooth across cell boundaries. */
	inline double ValueNoise(uint32 Seed, FVector2d LatticePosition)
	{
		const int32 CellX = FMath::FloorToInt32(LatticePosition.X);
		const int32 CellY = FMath::FloorToInt32(LatticePosition.Y);
		const double FractionX = SmoothFraction(LatticePosition.X - CellX);
		const double FractionY = SmoothFraction(LatticePosition.Y - CellY);

		const double SouthEdge = FMath::Lerp(
			LatticeValue(Seed, CellX, CellY), LatticeValue(Seed, CellX + 1, CellY), FractionX);
		const double NorthEdge = FMath::Lerp(
			LatticeValue(Seed, CellX, CellY + 1), LatticeValue(Seed, CellX + 1, CellY + 1), FractionX);
		return FMath::Lerp(SouthEdge, NorthEdge, FractionY);
	}
}
