using UnrealBuildTool;

public class MassifTarget : TargetRules
{
	public MassifTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.AddRange(new string[]
		{
			"MassifCore",
			"MassifSim",
			"MassifWorldGen",
			"MassifPresentation"
		});
	}
}
