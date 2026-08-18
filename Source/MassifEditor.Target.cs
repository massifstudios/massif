using UnrealBuildTool;

public class MassifEditorTarget : TargetRules
{
	public MassifEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
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
