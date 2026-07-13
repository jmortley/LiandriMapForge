// LiandriMapForge editor bridge. Editor-only; never ships in a game build.

namespace UnrealBuildTool.Rules
{
	public class MapForgeBridge : ModuleRules
	{
		public MapForgeBridge(TargetInfo Target)
		{
			PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

			PrivateIncludePaths.Add("MapForgeBridge/Private");

			PrivateDependencyModuleNames.AddRange(new string[] {
				"Core",
				"CoreUObject",
				"Engine",
				"UnrealEd",
				"Networking",
				"Sockets",
				"Json",
				"AssetRegistry"   // forge_chassis_physasset: AssetCreated notify
			});
		}
	}
}
