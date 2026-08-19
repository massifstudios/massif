#include "MassifNoise.h"

namespace
{
	uint32 PcgHash(uint32 Input)
	{
		const uint32 State = Input * 747796405u + 2891336453u;
		const uint32 Word = ((State >> ((State >> 28u) + 4u)) ^ State) * 277803737u;
		return (Word >> 22u) ^ Word;
	}

	uint32 HashLattice(int64 X, int64 Y, uint32 Seed)
	{
		const uint32 Ux = static_cast<uint32>(X) * 198491317u;
		const uint32 Uy = static_cast<uint32>(Y) * 6542989u;
		return PcgHash(Ux ^ Uy ^ Seed);
	}

	FVector2f LatticeGradient(int64 X, int64 Y, uint32 Seed)
	{
		static const FVector2f Gradients[8] =
		{
			FVector2f(1.0f, 0.0f), FVector2f(-1.0f, 0.0f),
			FVector2f(0.0f, 1.0f), FVector2f(0.0f, -1.0f),
			FVector2f(0.70710678f, 0.70710678f), FVector2f(-0.70710678f, 0.70710678f),
			FVector2f(0.70710678f, -0.70710678f), FVector2f(-0.70710678f, -0.70710678f)
		};
		return Gradients[HashLattice(X, Y, Seed) & 7u];
	}

	float QuinticFade(float T)
	{
		return T * T * T * (T * (T * 6.0f - 15.0f) + 10.0f);
	}

	/** Lattice cell a coordinate falls in, and where in that cell it sits. */
	struct FLatticeSplit
	{
		int64 Cell = 0;
		float Fraction = 0.0f;
	};

	FLatticeSplit SplitLattice(double Coordinate)
	{
		const double Floor = FMath::FloorToDouble(Coordinate);
		FLatticeSplit Split;
		Split.Cell = static_cast<int64>(Floor);
		Split.Fraction = static_cast<float>(Coordinate - Floor);
		return Split;
	}
}

float MassifNoise::GradientNoise(double X, double Y, uint32 Seed)
{
	const FLatticeSplit SplitX = SplitLattice(X);
	const FLatticeSplit SplitY = SplitLattice(Y);
	const float FracX = SplitX.Fraction;
	const float FracY = SplitY.Fraction;

	const float Dot00 = LatticeGradient(SplitX.Cell, SplitY.Cell, Seed).Dot(FVector2f(FracX, FracY));
	const float Dot10 = LatticeGradient(SplitX.Cell + 1, SplitY.Cell, Seed).Dot(FVector2f(FracX - 1.0f, FracY));
	const float Dot01 = LatticeGradient(SplitX.Cell, SplitY.Cell + 1, Seed).Dot(FVector2f(FracX, FracY - 1.0f));
	const float Dot11 = LatticeGradient(SplitX.Cell + 1, SplitY.Cell + 1, Seed).Dot(FVector2f(FracX - 1.0f, FracY - 1.0f));

	const float FadeX = QuinticFade(FracX);
	const float FadeY = QuinticFade(FracY);
	const float North = FMath::Lerp(Dot00, Dot10, FadeX);
	const float South = FMath::Lerp(Dot01, Dot11, FadeX);

	// Scale so the practical output range fills [-1, 1].
	return FMath::Lerp(North, South, FadeY) * 1.41421356f;
}

FVector2f MassifNoise::CellPoint(int64 CellX, int64 CellY, uint32 Seed)
{
	const uint32 Hash = HashLattice(CellX, CellY, Seed);
	const float ToUnit = 1.0f / 65536.0f;
	return FVector2f(
		static_cast<float>(Hash & 0xFFFFu) * ToUnit,
		static_cast<float>((Hash >> 16) & 0xFFFFu) * ToUnit);
}

float MassifNoise::Fractal(double X, double Y, uint32 Seed, int32 OctaveCount, double Lacunarity, float Gain)
{
	float Sum = 0.0f;
	float Amplitude = 1.0f;
	float AmplitudeSum = 0.0f;
	double Frequency = 1.0;

	for (int32 Octave = 0; Octave < OctaveCount; ++Octave)
	{
		Sum += Amplitude * GradientNoise(X * Frequency, Y * Frequency, Seed + static_cast<uint32>(Octave) * 0x9E3779B9u);
		AmplitudeSum += Amplitude;
		Amplitude *= Gain;
		Frequency *= Lacunarity;
	}

	return Sum / FMath::Max(AmplitudeSum, UE_SMALL_NUMBER);
}

float MassifNoise::RidgedFractal(double X, double Y, uint32 Seed, int32 OctaveCount, double Lacunarity, float Gain, float SharpnessExponent)
{
	float Sum = 0.0f;
	float Amplitude = 1.0f;
	float AmplitudeSum = 0.0f;
	double Frequency = 1.0;
	float RidgeWeight = 1.0f;

	for (int32 Octave = 0; Octave < OctaveCount; ++Octave)
	{
		const float Noise = GradientNoise(X * Frequency, Y * Frequency, Seed + static_cast<uint32>(Octave) * 0x85EBCA6Bu);
		float Ridge = 1.0f - FMath::Abs(Noise);
		Ridge = FMath::Pow(Ridge, SharpnessExponent) * RidgeWeight;

		Sum += Ridge * Amplitude;
		AmplitudeSum += Amplitude;
		RidgeWeight = FMath::Clamp(Ridge * 2.0f, 0.0f, 1.0f);
		Amplitude *= Gain;
		Frequency *= Lacunarity;
	}

	return Sum / FMath::Max(AmplitudeSum, UE_SMALL_NUMBER);
}
