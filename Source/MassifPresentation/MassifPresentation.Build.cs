using UnrealBuildTool;

public class MassifPresentation : ModuleRules
{
	public MassifPresentation(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"MassifCore",
			"MassifSim",
			"MassifWorldGen"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"GeometryCore",
			"GeometryFramework",
			"GeometryScriptingCore",
			// GMaxRHIShaderPlatform, for reading the terrain material's compile errors.
			"RHI"
		});
	}
}
