// LiandriMapForge editor bridge module. Editor-only.
#include "MapForgeBridgeServer.h"
#include "Modules/ModuleManager.h"
#include "Misc/CoreMisc.h"

class FMapForgeBridgeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Commandlets load Editor-type modules too; the bridge only makes
		// sense inside an interactive UnrealEd session.
		if (!IsRunningCommandlet())
		{
			Server = MakeUnique<FMapForgeBridgeServer>();
		}
	}

	virtual void ShutdownModule() override
	{
		Server.Reset();
	}

private:
	TUniquePtr<FMapForgeBridgeServer> Server;
};

IMPLEMENT_MODULE(FMapForgeBridgeModule, MapForgeBridge)
