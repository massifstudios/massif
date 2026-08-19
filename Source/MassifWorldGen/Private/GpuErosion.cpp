#include "GpuErosion.h"

#include "GlobalShader.h"
#include "Misc/App.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "ShaderParameterStruct.h"

namespace
{
	constexpr int32 ThreadGroupSize = 8;

	BEGIN_SHADER_PARAMETER_STRUCT(FErosionCommonParameters, )
		SHADER_PARAMETER(FUintVector2, GridSize)
		SHADER_PARAMETER(float, CellSizeMeters)
		SHADER_PARAMETER(float, DeltaTime)
		SHADER_PARAMETER(float, RainRate)
		SHADER_PARAMETER(float, EvaporationRate)
		SHADER_PARAMETER(float, PipeCrossSection)
		SHADER_PARAMETER(float, Gravity)
		SHADER_PARAMETER(float, SedimentCapacityFactor)
		SHADER_PARAMETER(float, DissolveRate)
		SHADER_PARAMETER(float, DepositRate)
		SHADER_PARAMETER(float, TalusTangent)
		SHADER_PARAMETER(float, ThermalRate)
		SHADER_PARAMETER(float, MaxWaterSpeed)
		SHADER_PARAMETER(float, MaxErosionPerStep)
		SHADER_PARAMETER(float, FluxDamping)
	END_SHADER_PARAMETER_STRUCT()

	class FAddWaterCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FAddWaterCS);
		SHADER_USE_PARAMETER_STRUCT(FAddWaterCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_INCLUDE(FErosionCommonParameters, Common)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, WaterIn)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, WaterOut)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FComputeFluxCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FComputeFluxCS);
		SHADER_USE_PARAMETER_STRUCT(FComputeFluxCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_INCLUDE(FErosionCommonParameters, Common)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, TerrainIn)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, WaterIn)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, FluxIn)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, FluxOut)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FUpdateWaterCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FUpdateWaterCS);
		SHADER_USE_PARAMETER_STRUCT(FUpdateWaterCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_INCLUDE(FErosionCommonParameters, Common)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, WaterIn)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, FluxIn)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, WaterOut)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, VelocityOut)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FErodeDepositCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FErodeDepositCS);
		SHADER_USE_PARAMETER_STRUCT(FErodeDepositCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_INCLUDE(FErosionCommonParameters, Common)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, TerrainIn)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, WaterIn)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SedimentIn)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, VelocityIn)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, TerrainOut)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, SedimentOut)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FAdvectSedimentCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FAdvectSedimentCS);
		SHADER_USE_PARAMETER_STRUCT(FAdvectSedimentCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_INCLUDE(FErosionCommonParameters, Common)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SedimentIn)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, VelocityIn)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, SedimentOut)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FEvaporateCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FEvaporateCS);
		SHADER_USE_PARAMETER_STRUCT(FEvaporateCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_INCLUDE(FErosionCommonParameters, Common)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, WaterIn)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, WaterOut)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FThermalFluxCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FThermalFluxCS);
		SHADER_USE_PARAMETER_STRUCT(FThermalFluxCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_INCLUDE(FErosionCommonParameters, Common)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, TerrainIn)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, FluxOut)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FThermalApplyCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FThermalApplyCS);
		SHADER_USE_PARAMETER_STRUCT(FThermalApplyCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_INCLUDE(FErosionCommonParameters, Common)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, TerrainIn)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, FluxIn)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, TerrainOut)
		END_SHADER_PARAMETER_STRUCT()
	};

	struct FErosionTextures
	{
		FRDGTextureRef TerrainA = nullptr;
		FRDGTextureRef TerrainB = nullptr;
		FRDGTextureRef WaterA = nullptr;
		FRDGTextureRef WaterB = nullptr;
		FRDGTextureRef SedimentA = nullptr;
		FRDGTextureRef SedimentB = nullptr;
		FRDGTextureRef FluxA = nullptr;
		FRDGTextureRef FluxB = nullptr;
		FRDGTextureRef Velocity = nullptr;
	};

	FErosionTextures CreateErosionTextures(FRDGBuilder& GraphBuilder, FIntPoint Extent)
	{
		const ETextureCreateFlags Flags = TexCreate_ShaderResource | TexCreate_UAV;
		const FRDGTextureDesc ScalarDesc = FRDGTextureDesc::Create2D(Extent, PF_R32_FLOAT, FClearValueBinding::Black, Flags);
		const FRDGTextureDesc FluxDesc = FRDGTextureDesc::Create2D(Extent, PF_A32B32G32R32F, FClearValueBinding::Black, Flags);
		const FRDGTextureDesc VelocityDesc = FRDGTextureDesc::Create2D(Extent, PF_G32R32F, FClearValueBinding::Black, Flags);

		FErosionTextures Textures;
		Textures.TerrainA = GraphBuilder.CreateTexture(ScalarDesc, TEXT("Massif.Erosion.TerrainA"));
		Textures.TerrainB = GraphBuilder.CreateTexture(ScalarDesc, TEXT("Massif.Erosion.TerrainB"));
		Textures.WaterA = GraphBuilder.CreateTexture(ScalarDesc, TEXT("Massif.Erosion.WaterA"));
		Textures.WaterB = GraphBuilder.CreateTexture(ScalarDesc, TEXT("Massif.Erosion.WaterB"));
		Textures.SedimentA = GraphBuilder.CreateTexture(ScalarDesc, TEXT("Massif.Erosion.SedimentA"));
		Textures.SedimentB = GraphBuilder.CreateTexture(ScalarDesc, TEXT("Massif.Erosion.SedimentB"));
		Textures.FluxA = GraphBuilder.CreateTexture(FluxDesc, TEXT("Massif.Erosion.FluxA"));
		Textures.FluxB = GraphBuilder.CreateTexture(FluxDesc, TEXT("Massif.Erosion.FluxB"));
		Textures.Velocity = GraphBuilder.CreateTexture(VelocityDesc, TEXT("Massif.Erosion.Velocity"));
		return Textures;
	}

	FErosionCommonParameters MakeCommonParameters(const FWorldGenConfig& Config, int32 Resolution)
	{
		FErosionCommonParameters Common;
		Common.GridSize = FUintVector2(static_cast<uint32>(Resolution), static_cast<uint32>(Resolution));
		Common.CellSizeMeters = Config.WorldSizeMeters / static_cast<float>(Resolution - 1);
		Common.DeltaTime = Config.ErosionDeltaTime;
		Common.RainRate = Config.RainRate;
		Common.EvaporationRate = Config.EvaporationRate;
		Common.PipeCrossSection = Common.CellSizeMeters * Common.CellSizeMeters;
		Common.Gravity = 9.81f;
		Common.SedimentCapacityFactor = Config.SedimentCapacityFactor;
		Common.DissolveRate = Config.DissolveRate;
		Common.DepositRate = Config.DepositRate;
		Common.TalusTangent = FMath::Tan(FMath::DegreesToRadians(Config.TalusAngleDegrees));
		Common.ThermalRate = Config.ThermalRate;
		Common.MaxWaterSpeed = 10.0f;
		Common.MaxErosionPerStep = 0.02f;
		Common.FluxDamping = 0.99f;
		return Common;
	}

	FIntVector ErosionGroupCount(int32 Resolution)
	{
		const int32 Groups = FMath::DivideAndRoundUp(Resolution, ThreadGroupSize);
		return FIntVector(Groups, Groups, 1);
	}

	void AddHydraulicIteration(
		FRDGBuilder& GraphBuilder, const FErosionCommonParameters& Common, FErosionTextures& Textures, FIntVector GroupCount)
	{
		const FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

		{
			FAddWaterCS::FParameters* Parameters = GraphBuilder.AllocParameters<FAddWaterCS::FParameters>();
			Parameters->Common = Common;
			Parameters->WaterIn = Textures.WaterA;
			Parameters->WaterOut = GraphBuilder.CreateUAV(Textures.WaterB);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Massif.Erosion.AddWater"),
				TShaderMapRef<FAddWaterCS>(ShaderMap), Parameters, GroupCount);
			Swap(Textures.WaterA, Textures.WaterB);
		}
		{
			FComputeFluxCS::FParameters* Parameters = GraphBuilder.AllocParameters<FComputeFluxCS::FParameters>();
			Parameters->Common = Common;
			Parameters->TerrainIn = Textures.TerrainA;
			Parameters->WaterIn = Textures.WaterA;
			Parameters->FluxIn = Textures.FluxA;
			Parameters->FluxOut = GraphBuilder.CreateUAV(Textures.FluxB);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Massif.Erosion.ComputeFlux"),
				TShaderMapRef<FComputeFluxCS>(ShaderMap), Parameters, GroupCount);
			Swap(Textures.FluxA, Textures.FluxB);
		}
		{
			FUpdateWaterCS::FParameters* Parameters = GraphBuilder.AllocParameters<FUpdateWaterCS::FParameters>();
			Parameters->Common = Common;
			Parameters->WaterIn = Textures.WaterA;
			Parameters->FluxIn = Textures.FluxA;
			Parameters->WaterOut = GraphBuilder.CreateUAV(Textures.WaterB);
			Parameters->VelocityOut = GraphBuilder.CreateUAV(Textures.Velocity);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Massif.Erosion.UpdateWater"),
				TShaderMapRef<FUpdateWaterCS>(ShaderMap), Parameters, GroupCount);
			Swap(Textures.WaterA, Textures.WaterB);
		}
		{
			FErodeDepositCS::FParameters* Parameters = GraphBuilder.AllocParameters<FErodeDepositCS::FParameters>();
			Parameters->Common = Common;
			Parameters->TerrainIn = Textures.TerrainA;
			Parameters->WaterIn = Textures.WaterA;
			Parameters->SedimentIn = Textures.SedimentA;
			Parameters->VelocityIn = Textures.Velocity;
			Parameters->TerrainOut = GraphBuilder.CreateUAV(Textures.TerrainB);
			Parameters->SedimentOut = GraphBuilder.CreateUAV(Textures.SedimentB);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Massif.Erosion.ErodeDeposit"),
				TShaderMapRef<FErodeDepositCS>(ShaderMap), Parameters, GroupCount);
			Swap(Textures.TerrainA, Textures.TerrainB);
			Swap(Textures.SedimentA, Textures.SedimentB);
		}
		{
			FAdvectSedimentCS::FParameters* Parameters = GraphBuilder.AllocParameters<FAdvectSedimentCS::FParameters>();
			Parameters->Common = Common;
			Parameters->SedimentIn = Textures.SedimentA;
			Parameters->VelocityIn = Textures.Velocity;
			Parameters->SedimentOut = GraphBuilder.CreateUAV(Textures.SedimentB);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Massif.Erosion.AdvectSediment"),
				TShaderMapRef<FAdvectSedimentCS>(ShaderMap), Parameters, GroupCount);
			Swap(Textures.SedimentA, Textures.SedimentB);
		}
		{
			FEvaporateCS::FParameters* Parameters = GraphBuilder.AllocParameters<FEvaporateCS::FParameters>();
			Parameters->Common = Common;
			Parameters->WaterIn = Textures.WaterA;
			Parameters->WaterOut = GraphBuilder.CreateUAV(Textures.WaterB);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Massif.Erosion.Evaporate"),
				TShaderMapRef<FEvaporateCS>(ShaderMap), Parameters, GroupCount);
			Swap(Textures.WaterA, Textures.WaterB);
		}
	}

	void AddThermalIteration(
		FRDGBuilder& GraphBuilder, const FErosionCommonParameters& Common, FErosionTextures& Textures, FIntVector GroupCount)
	{
		const FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

		{
			FThermalFluxCS::FParameters* Parameters = GraphBuilder.AllocParameters<FThermalFluxCS::FParameters>();
			Parameters->Common = Common;
			Parameters->TerrainIn = Textures.TerrainA;
			Parameters->FluxOut = GraphBuilder.CreateUAV(Textures.FluxB);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Massif.Erosion.ThermalFlux"),
				TShaderMapRef<FThermalFluxCS>(ShaderMap), Parameters, GroupCount);
			Swap(Textures.FluxA, Textures.FluxB);
		}
		{
			FThermalApplyCS::FParameters* Parameters = GraphBuilder.AllocParameters<FThermalApplyCS::FParameters>();
			Parameters->Common = Common;
			Parameters->TerrainIn = Textures.TerrainA;
			Parameters->FluxIn = Textures.FluxA;
			Parameters->TerrainOut = GraphBuilder.CreateUAV(Textures.TerrainB);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Massif.Erosion.ThermalApply"),
				TShaderMapRef<FThermalApplyCS>(ShaderMap), Parameters, GroupCount);
			Swap(Textures.TerrainA, Textures.TerrainB);
		}
	}
}

IMPLEMENT_GLOBAL_SHADER(FAddWaterCS, "/MassifWorldGen/MassifErosion.usf", "AddWaterCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FComputeFluxCS, "/MassifWorldGen/MassifErosion.usf", "ComputeFluxCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FUpdateWaterCS, "/MassifWorldGen/MassifErosion.usf", "UpdateWaterCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FErodeDepositCS, "/MassifWorldGen/MassifErosion.usf", "ErodeDepositCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FAdvectSedimentCS, "/MassifWorldGen/MassifErosion.usf", "AdvectSedimentCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FEvaporateCS, "/MassifWorldGen/MassifErosion.usf", "EvaporateCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FThermalFluxCS, "/MassifWorldGen/MassifErosion.usf", "ThermalFluxCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FThermalApplyCS, "/MassifWorldGen/MassifErosion.usf", "ThermalApplyCS", SF_Compute);

bool FMassifGpuErosion::CanRun()
{
	return FApp::CanEverRender() && GDynamicRHI != nullptr;
}

bool FMassifGpuErosion::Run(const FWorldGenConfig& Config, TArray<float>& HeightMeters)
{
	const int32 Resolution = Config.Resolution;
	check(HeightMeters.Num() == Resolution * Resolution);
	if (!CanRun())
	{
		return false;
	}

	TArray<float> ErodedHeights;
	ErodedHeights.SetNumUninitialized(HeightMeters.Num());
	FEvent* CompletionEvent = FPlatformProcess::GetSynchEventFromPool(true);
	std::atomic<bool> bSucceeded{ false };

	const float* SourceData = HeightMeters.GetData();
	float* DestinationData = ErodedHeights.GetData();

	ENQUEUE_RENDER_COMMAND(MassifGpuErosion)(
		[&Config, SourceData, DestinationData, Resolution, CompletionEvent, &bSucceeded](FRHICommandListImmediate& RHICmdList)
	{
		const FIntPoint Extent(Resolution, Resolution);

		// Upload the heightfield into a plain RHI texture RDG can import.
		const FRHITextureCreateDesc UploadDesc = FRHITextureCreateDesc::Create2D(
			TEXT("Massif.Erosion.Upload"), Resolution, Resolution, PF_R32_FLOAT)
			.SetFlags(ETextureCreateFlags::ShaderResource);
		FTextureRHIRef UploadTexture = RHICreateTexture(UploadDesc);
		uint32 DestStride = 0;
		uint8* MipData = static_cast<uint8*>(RHILockTexture2D(UploadTexture, 0, RLM_WriteOnly, DestStride, false));
		for (int32 Row = 0; Row < Resolution; ++Row)
		{
			FMemory::Memcpy(MipData + Row * DestStride, SourceData + Row * Resolution, Resolution * sizeof(float));
		}
		RHIUnlockTexture2D(UploadTexture, 0, false);

		FRHIGPUTextureReadback Readback(TEXT("Massif.Erosion.Readback"));
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			FErosionTextures Textures = CreateErosionTextures(GraphBuilder, Extent);

			const FRDGTextureRef ImportedUpload = RegisterExternalTexture(
				GraphBuilder, UploadTexture, TEXT("Massif.Erosion.UploadRT"), ERDGTextureFlags::None);
			AddCopyTexturePass(GraphBuilder, ImportedUpload, Textures.TerrainA);

			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Textures.WaterA), 0.0f);
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Textures.SedimentA), 0.0f);
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Textures.FluxA), FVector4f(0, 0, 0, 0));
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Textures.Velocity), FVector4f(0, 0, 0, 0));

			const FErosionCommonParameters Common = MakeCommonParameters(Config, Resolution);
			const FIntVector GroupCount = ErosionGroupCount(Resolution);

			for (int32 Iteration = 0; Iteration < Config.HydraulicIterationCount; ++Iteration)
			{
				AddHydraulicIteration(GraphBuilder, Common, Textures, GroupCount);
			}
			for (int32 Iteration = 0; Iteration < Config.ThermalIterationCount; ++Iteration)
			{
				AddThermalIteration(GraphBuilder, Common, Textures, GroupCount);
			}

			AddEnqueueCopyPass(GraphBuilder, &Readback, Textures.TerrainA);
			GraphBuilder.Execute();
		}

		RHICmdList.BlockUntilGPUIdle();
		if (Readback.IsReady())
		{
			int32 RowPitchInPixels = 0;
			const float* ReadbackData = static_cast<const float*>(Readback.Lock(RowPitchInPixels));
			for (int32 Row = 0; Row < Resolution; ++Row)
			{
				FMemory::Memcpy(DestinationData + Row * Resolution, ReadbackData + Row * RowPitchInPixels, Resolution * sizeof(float));
			}
			Readback.Unlock();
			bSucceeded = true;
		}
		CompletionEvent->Trigger();
	});

	const bool bCompleted = CompletionEvent->Wait(FTimespan::FromSeconds(180.0));
	FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
	if (!bCompleted || !bSucceeded)
	{
		UE_LOG(LogTemp, Error, TEXT("GPU erosion failed or timed out; keeping uneroded heightfield"));
		return false;
	}

	HeightMeters = MoveTemp(ErodedHeights);
	return true;
}
