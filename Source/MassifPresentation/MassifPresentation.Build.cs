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
			"GeometryCore",
			"GeometryFramework",
			"GeometryScriptingCore",
			"Slate",
			"SlateCore"
		});
	}
}
