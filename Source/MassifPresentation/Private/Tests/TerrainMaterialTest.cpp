#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "MaterialShared.h"
#include "Misc/AutomationTest.h"

/**
 * A terrain material that fails to translate is replaced by the engine default,
 * which draws as plausible grey ground: the massif still renders, so nothing
 * looks broken enough to notice, and every texture, layer blend and parallax
 * step silently stops existing. The engine says so once, at warning level,
 * among thousands of lines.
 *
 * Loading the material is enough to translate it, which is where a bad custom
 * node, a missing shader include or a mistyped parameter surfaces.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainMaterialTest,
	"Massif.Presentation.TerrainMaterial",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FTerrainMaterialTest::RunTest(const FString& Parameters)
{
	UMaterial* Material = LoadObject<UMaterial>(nullptr, TEXT("/Game/Materials/M_Terrain.M_Terrain"));
	if (!TestNotNull(TEXT("M_Terrain loads"), Material))
	{
		return false;
	}

	const FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform);
	if (!TestNotNull(TEXT("M_Terrain has a material resource"), Resource))
	{
		return false;
	}

	for (const FString& CompileError : Resource->GetCompileErrors())
	{
		AddError(FString::Printf(TEXT("M_Terrain: %s"), *CompileError));
	}
	return true;
}
