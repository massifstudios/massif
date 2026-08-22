#include "MassifSurfaceMap.h"

#include "Async/ParallelFor.h"
#include "Engine/Texture2D.h"
#include "MassifBandWeight.h"
#include "MassifEnvironmentRaster.h"
#include "MassifHabitat.h"
#include "MassifSeed.h"
#include "MassifSurfaceLayers.h"
#include "MassifValueNoise.h"
#include "TextureResource.h"

namespace
{
	/** Spacing habitat is sampled at for the whole-world pass: the hydrology channels' cell size. */
	constexpr double SurfaceSampleSpacingMeters = 8.0;

	/**
	 * How far the patch noise moves a layer's weight. At 0 the highest abundance
	 * takes every point that fits, and one texture covers a whole belt of the
	 * mountain; at 1 a layer can be shut out of ground it belongs on. Two thirds
	 * lets any of the sets that fit a niche win somewhere in it.
	 */
	constexpr double PatchContrast = 0.65;

	/** Offsets the patch noise off the placement stream, so layers and plants do not share a lattice. */
	constexpr uint32 SurfacePatchSeedOffset = 0x85EBCA6Bu;

	/**
	 * Temperature either side of freezing over which snow goes from absent to
	 * total. Centred on the same 0 °C that makes ground permanently snowed in
	 * MassifHabitat, so the cover and the ground type agree about where the
	 * snowline is and only disagree about how sharp it is.
	 */
	constexpr double SnowTemperatureCelsius = 0.0;
	constexpr double SnowTemperatureFadeCelsius = 1.6;

	/** Ground steep enough to shed snow, and the angle it starts sliding at. */
	constexpr double SnowHoldSlopeDegrees = 42.0;
	constexpr double SnowShedSlopeDegrees = 58.0;

	/**
	 * Shuffles the ranking between layers over tens of metres. Driven by position
	 * rather than by a stream, so a patch keeps its shape across bake cells
	 * instead of being cut off at them.
	 */
	double PatchWeight(const FMassifSurfaceLayer& Layer, uint32 LayerSeed, FVector2d PositionMeters)
	{
		const double Noise = MassifNoise::ValueNoise(
			LayerSeed + SurfacePatchSeedOffset, PositionMeters / Layer.PatchWavelengthMeters);
		return 1.0 - PatchContrast * (1.0 - Noise);
	}

	uint32 LayerSeed(const FMassifSurfaceLayer& Layer, uint32 MasterSeed)
	{
		return FMassifSeed(MasterSeed).MakeChannelSeed(Layer.Name);
	}

	/**
	 * How much the ground's angle counts against a layer.
	 *
	 * Not at all under a river. A channel bed is as steep as the cross-section cut
	 * for it — a brook a metre wide has banks at forty degrees — and that says
	 * nothing about what is lying on the bottom of it. Everywhere else the angle is
	 * exactly what decides between a face, a scree and a meadow.
	 */
	double SlopeFit(const FMassifSurfaceLayer& Layer, const FMassifHabitatSample& Habitat)
	{
		if (Habitat.GroundType == EMassifGroundType::Water)
		{
			return 1.0;
		}
		return MassifBandWeight(
			Habitat.SlopeDegrees, Layer.MinSlopeDegrees, Layer.MaxSlopeDegrees, Layer.SlopeFadeDegrees);
	}

	/** How well one layer suits the ground, before the patch noise breaks ties. */
	double HabitatFit(const FMassifSurfaceLayer& Layer, const FMassifHabitatSample& Habitat)
	{
		if ((Layer.GroundTypeMask & MassifGroundTypeBit(Habitat.GroundType)) == 0)
		{
			return 0.0;
		}

		const double Temperature = MassifBandWeight(
			Habitat.TemperatureCelsius,
			Layer.MinTemperatureCelsius,
			Layer.MaxTemperatureCelsius,
			Layer.TemperatureFadeCelsius);
		const double Humidity = MassifBandWeight(
			Habitat.Humidity, Layer.MinHumidity, Layer.MaxHumidity, Layer.HumidityFade);
		return Temperature * Humidity * SlopeFit(Layer, Habitat);
	}

	double LayerWeight(
		const FMassifSurfaceLayer& Layer,
		uint32 MasterSeed,
		const FMassifHabitatSample& Habitat,
		FVector2d PositionMeters)
	{
		const double Fit = HabitatFit(Layer, Habitat);
		if (Fit <= 0.0)
		{
			return 0.0;
		}
		return Fit * Layer.Abundance * PatchWeight(Layer, LayerSeed(Layer, MasterSeed), PositionMeters);
	}

	/** The two best-fitting layers and their weights. */
	struct FLayerRanking
	{
		int32 BestIndex = 0;
		int32 SecondIndex = 0;
		double BestWeight = 0.0;
		double SecondWeight = 0.0;
	};

	void ConsiderLayer(FLayerRanking& Ranking, int32 Index, double Weight)
	{
		if (Weight > Ranking.BestWeight)
		{
			Ranking.SecondIndex = Ranking.BestIndex;
			Ranking.SecondWeight = Ranking.BestWeight;
			Ranking.BestIndex = Index;
			Ranking.BestWeight = Weight;
			return;
		}
		if (Weight > Ranking.SecondWeight)
		{
			Ranking.SecondIndex = Index;
			Ranking.SecondWeight = Weight;
		}
	}

	/**
	 * The layer to wear where nothing in the table fits — ground whose
	 * combination of climate and cover no set was written for. Picked on ground
	 * type and slope alone, which always answers something, so the terrain is
	 * never left holding layer 0 by accident.
	 */
	double SlopeOnlyWeight(const FMassifSurfaceLayer& Layer, const FMassifHabitatSample& Habitat)
	{
		if ((Layer.GroundTypeMask & MassifGroundTypeBit(Habitat.GroundType)) == 0)
		{
			return 0.0;
		}
		return SlopeFit(Layer, Habitat) * Layer.Abundance;
	}

	FLayerRanking RankBySlopeOnly(const FMassifHabitatSample& Habitat)
	{
		FLayerRanking Ranking;
		for (int32 Index = 0; Index < MassifSurface::LayerCount; ++Index)
		{
			ConsiderLayer(Ranking, Index, SlopeOnlyWeight(MassifSurface::Layers[Index], Habitat));
		}
		return Ranking;
	}

	FLayerRanking RankLayers(
		const FMassifHabitatSample& Habitat, FVector2d PositionMeters, uint32 MasterSeed)
	{
		FLayerRanking Ranking;
		for (int32 Index = 0; Index < MassifSurface::LayerCount; ++Index)
		{
			const FMassifSurfaceLayer& Layer = MassifSurface::Layers[Index];
			ConsiderLayer(Ranking, Index, LayerWeight(Layer, MasterSeed, Habitat, PositionMeters));
		}

		if (Ranking.BestWeight > 0.0)
		{
			return Ranking;
		}
		return RankBySlopeOnly(Habitat);
	}

	/** Snow lies where it is cold and the ground is not steep enough to shed it. */
	double SnowCover(const FMassifHabitatSample& Habitat)
	{
		const double Cold = FMath::Clamp(
			(SnowTemperatureCelsius - Habitat.TemperatureCelsius) / SnowTemperatureFadeCelsius + 0.5,
			0.0, 1.0);
		const double Holds = FMath::Clamp(
			(SnowShedSlopeDegrees - Habitat.SlopeDegrees) / (SnowShedSlopeDegrees - SnowHoldSlopeDegrees),
			0.0, 1.0);
		return Cold * Holds;
	}

	uint8 ToByte(double Fraction)
	{
		return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt32(Fraction * 255.0), 0, 255));
	}

	UTexture2D* EnsureTexture(TObjectPtr<UTexture2D>& Texture, int32 Width, int32 Height, TextureFilter Filter)
	{
		const bool bReusable = Texture != nullptr
			&& Texture->GetSizeX() == Width && Texture->GetSizeY() == Height;
		if (bReusable)
		{
			return Texture;
		}

		Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		// Indices and weights are data, not colour: an sRGB curve over a layer
		// index would decode as a different layer.
		Texture->SRGB = false;
		Texture->NeverStream = true;
		Texture->Filter = Filter;
		return Texture;
	}

	/** One row of the world, classified into the two textures' pixel buffers. */
	void ClassifyRow(
		const FMassifHabitatField& HabitatField,
		uint32 MasterSeed,
		const FMassifChannelGrid& Grid,
		int32 Row,
		FColor* IndexPixels,
		FColor* BlendPixels)
	{
		for (int32 Column = 0; Column < Grid.Width; ++Column)
		{
			const FVector2d PositionMeters = Grid.CellCentreMeters(Column, Row);
			const FMassifHabitatSample Habitat =
				HabitatField.Sample(PositionMeters, SurfaceSampleSpacingMeters);
			const FMassifSurfaceBlend Blend =
				MassifSurfaceMap::Classify(Habitat, PositionMeters, MasterSeed);

			const int32 Index = Row * Grid.Width + Column;
			IndexPixels[Index] = FColor(
				static_cast<uint8>(Blend.LowerLayer), static_cast<uint8>(Blend.UpperLayer), 0, 255);
			BlendPixels[Index] = FColor(ToByte(Blend.UpperWeight), ToByte(Blend.SnowCover), 0, 255);
		}
	}
}

FMassifSurfaceBlend MassifSurfaceMap::Classify(
	const FMassifHabitatSample& Habitat, FVector2d PositionMeters, uint32 MasterSeed)
{
	const FLayerRanking Ranking = RankLayers(Habitat, PositionMeters, MasterSeed);
	const double TotalWeight = Ranking.BestWeight + Ranking.SecondWeight;

	FMassifSurfaceBlend Blend;
	Blend.SnowCover = SnowCover(Habitat);
	Blend.LowerLayer = FMath::Min(Ranking.BestIndex, Ranking.SecondIndex);
	Blend.UpperLayer = FMath::Max(Ranking.BestIndex, Ranking.SecondIndex);
	if (TotalWeight <= 0.0)
	{
		return Blend;
	}

	// The share belongs to whichever of the two the ranking put first, and the
	// pair is stored in index order, so which end of the range that share lands
	// on depends on which way round the ranking came out.
	const double BestShare = Ranking.BestWeight / TotalWeight;
	if (Ranking.BestIndex >= Ranking.SecondIndex)
	{
		Blend.UpperWeight = BestShare;
		return Blend;
	}
	Blend.UpperWeight = 1.0 - BestShare;
	return Blend;
}

void MassifSurfaceMap::Render(
	const FMassifHabitatField& Habitat,
	uint32 MasterSeed,
	TObjectPtr<UTexture2D>& IndexTexture,
	TObjectPtr<UTexture2D>& BlendTexture)
{
	const FMassifChannelGrid& Grid = MassifEnvironmentRaster::GetGrid(Habitat.GetEnvironment());
	UTexture2D* Indices = EnsureTexture(IndexTexture, Grid.Width, Grid.Height, TF_Nearest);
	UTexture2D* Blends = EnsureTexture(BlendTexture, Grid.Width, Grid.Height, TF_Bilinear);

	FTexture2DMipMap& IndexMip = Indices->GetPlatformData()->Mips[0];
	FTexture2DMipMap& BlendMip = Blends->GetPlatformData()->Mips[0];
	FColor* IndexPixels = static_cast<FColor*>(IndexMip.BulkData.Lock(LOCK_READ_WRITE));
	FColor* BlendPixels = static_cast<FColor*>(BlendMip.BulkData.Lock(LOCK_READ_WRITE));

	ParallelFor(Grid.Height, [&Habitat, MasterSeed, &Grid, IndexPixels, BlendPixels](int32 Row)
	{
		ClassifyRow(Habitat, MasterSeed, Grid, Row, IndexPixels, BlendPixels);
	});

	IndexMip.BulkData.Unlock();
	BlendMip.BulkData.Unlock();
	Indices->UpdateResource();
	Blends->UpdateResource();
}

void MassifSurfaceMap::RenderLayerParameters(TObjectPtr<UTexture2D>& Texture)
{
	// One row longer than the table: snow is the slice after the last layer, and
	// the shader reads its tiling from the same place as everything else.
	const int32 EntryCount = MassifSurface::LayerCount + 1;
	if (Texture == nullptr)
	{
		Texture = UTexture2D::CreateTransient(EntryCount, 1, PF_A32B32G32R32F);
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		Texture->SRGB = false;
		Texture->NeverStream = true;
		Texture->Filter = TF_Nearest;
	}

	FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
	FLinearColor* Pixels = static_cast<FLinearColor*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
	for (int32 Index = 0; Index < MassifSurface::LayerCount; ++Index)
	{
		const FMassifSurfaceLayer& Layer = MassifSurface::Layers[Index];
		Pixels[Index] = FLinearColor(
			static_cast<float>(Layer.TileMeters), static_cast<float>(Layer.ReliefMeters), 0.0f, 1.0f);
	}
	Pixels[MassifSurface::LayerCount] = FLinearColor(
		static_cast<float>(MassifSurface::SnowTileMeters),
		static_cast<float>(MassifSurface::SnowReliefMeters), 0.0f, 1.0f);
	Mip.BulkData.Unlock();
	Texture->UpdateResource();
}
