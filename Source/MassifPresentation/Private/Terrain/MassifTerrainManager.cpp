#include "MassifTerrainManager.h"

#include "Async/Async.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Generators/RectangleMeshGenerator.h"
#include "Kismet/GameplayStatics.h"

using UE::Geometry::FDynamicMesh3;
using UE::Geometry::FDynamicMeshNormalOverlay;

namespace
{
	constexpr float MetersToCentimeters = 100.0f;

	FVector3f AnalyticNormal(const FWorldGenResult& WorldGen, float WorldXMeters, float WorldYMeters)
	{
		return WorldGen.SampleNormal(WorldXMeters, WorldYMeters);
	}

	void DisplaceGridToHeightfield(FDynamicMesh3& Mesh, const FWorldGenResult& WorldGen, const FVector2f ChunkCenterMeters)
	{
		for (const int32 VertexId : Mesh.VertexIndicesItr())
		{
			const FVector3d LocalPosition = Mesh.GetVertex(VertexId);
			const float WorldXMeters = ChunkCenterMeters.X + static_cast<float>(LocalPosition.X) / MetersToCentimeters;
			const float WorldYMeters = ChunkCenterMeters.Y + static_cast<float>(LocalPosition.Y) / MetersToCentimeters;
			const float AltitudeMeters = WorldGen.SampleHeightMeters(WorldXMeters, WorldYMeters);
			Mesh.SetVertex(VertexId, FVector3d(LocalPosition.X, LocalPosition.Y, AltitudeMeters * MetersToCentimeters));
		}
	}

	void AppendSkirts(FDynamicMesh3& Mesh, float SkirtDropCentimeters)
	{
		TArray<int32> BoundaryEdges;
		for (const int32 EdgeId : Mesh.BoundaryEdgeIndicesItr())
		{
			BoundaryEdges.Add(EdgeId);
		}

		for (const int32 EdgeId : BoundaryEdges)
		{
			const UE::Geometry::FIndex2i EdgeVertices = Mesh.GetEdgeV(EdgeId);
			const FVector3d PositionA = Mesh.GetVertex(EdgeVertices.A);
			const FVector3d PositionB = Mesh.GetVertex(EdgeVertices.B);
			const int32 DroppedA = Mesh.AppendVertex(PositionA - FVector3d(0, 0, SkirtDropCentimeters));
			const int32 DroppedB = Mesh.AppendVertex(PositionB - FVector3d(0, 0, SkirtDropCentimeters));
			Mesh.AppendTriangle(EdgeVertices.A, EdgeVertices.B, DroppedB);
			Mesh.AppendTriangle(EdgeVertices.A, DroppedB, DroppedA);
		}
	}

	void WriteAnalyticNormals(FDynamicMesh3& Mesh, const FWorldGenResult& WorldGen, const FVector2f ChunkCenterMeters)
	{
		Mesh.EnableAttributes();
		FDynamicMeshNormalOverlay* NormalOverlay = Mesh.Attributes()->PrimaryNormals();
		NormalOverlay->ClearElements();

		TArray<int32> VertexToElement;
		VertexToElement.SetNumUninitialized(Mesh.MaxVertexID());
		for (const int32 VertexId : Mesh.VertexIndicesItr())
		{
			const FVector3d LocalPosition = Mesh.GetVertex(VertexId);
			const float WorldXMeters = ChunkCenterMeters.X + static_cast<float>(LocalPosition.X) / MetersToCentimeters;
			const float WorldYMeters = ChunkCenterMeters.Y + static_cast<float>(LocalPosition.Y) / MetersToCentimeters;
			VertexToElement[VertexId] = NormalOverlay->AppendElement(AnalyticNormal(WorldGen, WorldXMeters, WorldYMeters));
		}

		for (const int32 TriangleId : Mesh.TriangleIndicesItr())
		{
			const UE::Geometry::FIndex3i Triangle = Mesh.GetTriangle(TriangleId);
			NormalOverlay->SetTriangle(TriangleId,
				UE::Geometry::FIndex3i(VertexToElement[Triangle.A], VertexToElement[Triangle.B], VertexToElement[Triangle.C]));
		}
	}

	TSharedPtr<FDynamicMesh3> BuildChunkMesh(
		const FWorldGenResult& WorldGen, FVector2f ChunkCenterMeters, float ChunkSizeMeters, int32 ChunkQuadCount, int32 Step)
	{
		const int32 QuadsPerSide = FMath::Max(1, ChunkQuadCount / Step);

		UE::Geometry::FRectangleMeshGenerator Generator;
		Generator.Width = ChunkSizeMeters * MetersToCentimeters;
		Generator.Height = ChunkSizeMeters * MetersToCentimeters;
		Generator.WidthVertexCount = QuadsPerSide + 1;
		Generator.HeightVertexCount = QuadsPerSide + 1;
		Generator.Origin = FVector3d::ZeroVector;
		Generator.Normal = FVector3f::UnitZ();
		Generator.Generate();

		TSharedPtr<FDynamicMesh3> Mesh = MakeShared<FDynamicMesh3>(&Generator);
		DisplaceGridToHeightfield(*Mesh, WorldGen, ChunkCenterMeters);

		const float SkirtDropMeters = 4.0f + 2.0f * static_cast<float>(Step) * WorldGen.MetersPerPixel;
		AppendSkirts(*Mesh, SkirtDropMeters * MetersToCentimeters);
		WriteAnalyticNormals(*Mesh, WorldGen, ChunkCenterMeters);

		return Mesh;
	}
}

AMassifTerrainManager::AMassifTerrainManager()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AMassifTerrainManager::Initialize(TSharedPtr<const FWorldGenResult> InWorldGen, UMaterialInterface* InTerrainMaterial)
{
	check(InWorldGen.IsValid() && InWorldGen->IsValid());
	checkf((InWorldGen->Resolution - 1) % ChunkQuadCount == 0,
		TEXT("Resolution-1 (%d) must be divisible by chunk quad count (%d)"), InWorldGen->Resolution - 1, ChunkQuadCount);

	WorldGen = InWorldGen;
	TerrainMaterial = InTerrainMaterial;
	FinishedBuilds = MakeShared<FFinishedBuildQueue>();
	ChunkCountPerSide = (WorldGen->Resolution - 1) / ChunkQuadCount;
	ChunkSizeMeters = WorldGen->WorldSizeMeters / static_cast<float>(ChunkCountPerSide);

	CreateChunkGrid();
}

void AMassifTerrainManager::CreateChunkGrid()
{
	const float HalfWorldMeters = WorldGen->WorldSizeMeters * 0.5f;
	Chunks.SetNum(ChunkCountPerSide * ChunkCountPerSide);
	ChunkComponents.SetNum(Chunks.Num());

	for (int32 ChunkIndex = 0; ChunkIndex < Chunks.Num(); ++ChunkIndex)
	{
		const int32 ChunkX = ChunkIndex % ChunkCountPerSide;
		const int32 ChunkY = ChunkIndex / ChunkCountPerSide;
		FChunkState& Chunk = Chunks[ChunkIndex];
		Chunk.CenterMeters = FVector2f(
			-HalfWorldMeters + (static_cast<float>(ChunkX) + 0.5f) * ChunkSizeMeters,
			-HalfWorldMeters + (static_cast<float>(ChunkY) + 0.5f) * ChunkSizeMeters);

		UDynamicMeshComponent* Component = NewObject<UDynamicMeshComponent>(this);
		Component->SetupAttachment(RootComponent);
		Component->RegisterComponent();
		Component->SetWorldLocation(FVector(Chunk.CenterMeters.X, Chunk.CenterMeters.Y, 0.0) * MetersToCentimeters);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetMaterial(0, TerrainMaterial);
		ChunkComponents[ChunkIndex] = Component;
	}
}

void AMassifTerrainManager::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!WorldGen.IsValid())
	{
		return;
	}

	ApplyFinishedBuilds();
	LaunchLodBuilds();
}

FVector AMassifTerrainManager::GetViewLocation() const
{
	const APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0);
	if (PlayerController == nullptr || PlayerController->PlayerCameraManager == nullptr)
	{
		return FVector::ZeroVector;
	}
	return PlayerController->PlayerCameraManager->GetCameraLocation();
}

int32 AMassifTerrainManager::DesiredStepForDistance(float DistanceMeters) const
{
	if (DistanceMeters < 768.0f)
	{
		return 1;
	}
	if (DistanceMeters < 1536.0f)
	{
		return 2;
	}
	if (DistanceMeters < 3072.0f)
	{
		return 4;
	}
	if (DistanceMeters < 6144.0f)
	{
		return 8;
	}
	return 16;
}

void AMassifTerrainManager::LaunchLodBuilds()
{
	const FVector ViewLocationMeters = GetViewLocation() / MetersToCentimeters;

	struct FBuildCandidate
	{
		int32 ChunkIndex = INDEX_NONE;
		int32 DesiredStep = 0;
		float DistanceMeters = 0.0f;
	};
	TArray<FBuildCandidate> Candidates;

	for (int32 ChunkIndex = 0; ChunkIndex < Chunks.Num(); ++ChunkIndex)
	{
		const FChunkState& Chunk = Chunks[ChunkIndex];
		const float GroundAltitudeMeters = WorldGen->SampleHeightMeters(Chunk.CenterMeters.X, Chunk.CenterMeters.Y);
		const FVector ChunkCenter(Chunk.CenterMeters.X, Chunk.CenterMeters.Y, GroundAltitudeMeters);
		const float DistanceMeters = static_cast<float>(FVector::Dist(ViewLocationMeters, ChunkCenter));
		const int32 DesiredStep = DesiredStepForDistance(DistanceMeters);
		const bool bNeedsBuild = Chunk.PendingStep == 0 && Chunk.CurrentStep != DesiredStep;
		if (bNeedsBuild)
		{
			Candidates.Add({ ChunkIndex, DesiredStep, DistanceMeters });
		}
	}

	Candidates.Sort([](const FBuildCandidate& Left, const FBuildCandidate& Right)
	{
		return Left.DistanceMeters < Right.DistanceMeters;
	});

	for (const FBuildCandidate& Candidate : Candidates)
	{
		if (InFlightBuildCount >= MaxInFlightBuilds)
		{
			break;
		}
		LaunchBuild(Candidate.ChunkIndex, Candidate.DesiredStep);
	}
}

void AMassifTerrainManager::LaunchBuild(int32 ChunkIndex, int32 Step)
{
	FChunkState& Chunk = Chunks[ChunkIndex];
	Chunk.PendingStep = Step;
	++InFlightBuildCount;

	TSharedPtr<const FWorldGenResult> WorldGenRef = WorldGen;
	TSharedPtr<FFinishedBuildQueue> QueueRef = FinishedBuilds;
	const FVector2f CenterMeters = Chunk.CenterMeters;
	const float SizeMeters = ChunkSizeMeters;

	Async(EAsyncExecution::ThreadPool, [WorldGenRef, QueueRef, ChunkIndex, Step, CenterMeters, SizeMeters]()
	{
		FFinishedBuild Finished;
		Finished.ChunkIndex = ChunkIndex;
		Finished.Step = Step;
		Finished.Mesh = BuildChunkMesh(*WorldGenRef, CenterMeters, SizeMeters, ChunkQuadCount, Step);
		QueueRef->Enqueue(MoveTemp(Finished));
	});
}

void AMassifTerrainManager::ApplyFinishedBuilds()
{
	int32 AppliedCount = 0;
	FFinishedBuild Finished;
	while (AppliedCount < MaxMeshApplicationsPerFrame && FinishedBuilds->Dequeue(Finished))
	{
		FChunkState& Chunk = Chunks[Finished.ChunkIndex];
		Chunk.CurrentStep = Finished.Step;
		Chunk.PendingStep = 0;
		--InFlightBuildCount;

		ChunkComponents[Finished.ChunkIndex]->SetMesh(MoveTemp(*Finished.Mesh));
		++AppliedCount;
	}
}
