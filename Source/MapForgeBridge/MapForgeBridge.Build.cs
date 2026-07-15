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
				"AudioEditor",
				"Networking",
				"Sockets",
				"Json",
				"AssetRegistry"   // forge_chassis_physasset: AssetCreated notify
			});

			// import_static_mesh: IAssetTools is a runtime interface fetched via
			// FModuleManager (no link dep) -- headers on the include path + module
			// loaded dynamically, matching ContentBrowser.Build.cs. UnrealEd already
			// provides UFbxFactory / UFbxImportUI / UAutomatedAssetImportData.
			PrivateIncludePathModuleNames.AddRange(new string[] { "AssetTools" });
			DynamicallyLoadedModuleNames.AddRange(new string[] { "AssetTools" });
		}
	}
}
