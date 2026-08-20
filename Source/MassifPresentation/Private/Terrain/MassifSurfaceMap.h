#pragma once

#include "CoreMinimal.h"

class FMassifHabitatField;
class UTexture2D;
struct FMassifHabitatSample;

/** The ground cover at one point: two surface layers and how they meet. */
struct FMassifSurfaceBlend
{
	/** Indices into MassifSurface::Layers. */
	int32 PrimaryLayer = 0;
	int32 SecondaryLayer = 0;

	/** How much of the secondary layer shows, 0 to 0.5. The primary always leads. */
	double SecondaryWeight = 0.0;

	/** Permanent snow over both, 0 bare to 1 covered. */
	double SnowCover = 0.0;
};

/**
 * Which textures the terrain material wears, as a function of habitat
 * (ARCHITECTURE §5.5). The material cannot read the habitat field, so the
 * classification is run over the whole world once and handed to it as two
 * textures.
 *
 * Only two layers out of the whole table reach the shader. That is what keeps
 * the material's cost fixed as texture sets are added: the choosing happens
 * here, on the CPU, once.
 *
 * Classification is a pure function of the habitat sample, the position and the
 * seed — same input, same answer, from any thread, testable headless.
 */
namespace MassifSurfaceMap
{
	FMassifSurfaceBlend Classify(
		const FMassifHabitatSample& Habitat, FVector2d PositionMeters, uint32 MasterSeed);

	/**
	 * Renders the classification over the whole world at the bake's own
	 * resolution, into two textures because they cannot share a filter: layer
	 * indices must be point-sampled, since a pixel interpolated between layer 3
	 * and layer 7 would be drawn as the unrelated layer 5, while the blend and
	 * the snow have to be filtered or the 8 m bake grid shows through as squares.
	 *
	 * IndexTexture holds the primary layer in red and the secondary in green.
	 * BlendTexture holds the weight between them in red and snow cover in green.
	 *
	 * Textures are reused when they already have the right size. Sampling runs
	 * across the thread pool: habitat is a pure function of position, so a row
	 * is independent of every other row.
	 */
	void Render(
		const FMassifHabitatField& Habitat,
		uint32 MasterSeed,
		TObjectPtr<UTexture2D>& IndexTexture,
		TObjectPtr<UTexture2D>& BlendTexture);

	/**
	 * The per-layer constants the shader needs once it knows which layer it is
	 * drawing, as a LayerCount x 1 float texture: red is the tiling in metres,
	 * green the relief the parallax offsets by. A texture rather than an array
	 * of material parameters because the layer index is only known per pixel,
	 * and a material graph cannot index a parameter with it.
	 */
	void RenderLayerParameters(TObjectPtr<UTexture2D>& Texture);
}
