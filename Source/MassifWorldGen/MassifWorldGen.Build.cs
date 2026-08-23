using UnrealBuildTool;

public class MassifWorldGen : ModuleRules
{
	public MassifWorldGen(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"GeometryCore",
			"MassifCore"
		});
	}
}
