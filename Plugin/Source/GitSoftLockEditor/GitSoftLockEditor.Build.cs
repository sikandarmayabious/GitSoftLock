using UnrealBuildTool;

public class GitSoftLockEditor : ModuleRules
{
    public GitSoftLockEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "UnrealEd", "HTTP", "Json",
            "Slate", "SlateCore", "DeveloperSettings", "ToolMenus", "Projects"
        });
    }
}
