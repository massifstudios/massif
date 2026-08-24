#pragma once

#include "CoreMinimal.h"

class FMassifHabitatField;
class UTexture2D;
struct FMassifHabitatSample;

/** The ground cover at one point: two surface layers and how they meet. */
struct FMassifSurfaceBlend
{
	/**
	 * Indices into MassifSurface::Layers, the lower one first. The material reads
	 * the two symmetrically — it gathers the cells around a pixel and ranks the
	 * layers they name by what each claims — so the order carries no meaning
	 * beyond being the one order two points that chose the same pair will agree
	 * on.
	 */
	int32 LowerLayer = 0;
	int32 UpperLayer = 0;

	/** How much of UpperLayer shows, 0 to 1, the rest being LowerLayer. */
	double UpperWeight = 0.0;

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
	 * resolution, into two textures because they cannot share a filter. Snow is a
	 * quantity and is filtered, or the 8 m bake grid shows through as squares.
	 * Layer indices and the weight between them cannot be: a texel interpolated
	 * between layer 3 and layer 7 would be drawn as the unrelated layer 5, and a
	 * weight interpolated between two cells naming different layers is a weight
	 * between nothing in particular. The material fetches those and interpolates
	 * the coverage it works out from them instead.
	 *
	 * IndexTexture holds the lower layer in red and the upper in green.
	 * BlendTexture holds the upper layer's weight in red and snow cover in green.
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
	 * green the relief the height map stands for. A texture rather than an array
	 * of material parameters because the layer index is only known per pixel,
	 * and a material graph cannot index a parameter with it.
	 */
	void RenderLayerParameters(TObjectPtr<UTexture2D>& Texture);
}
