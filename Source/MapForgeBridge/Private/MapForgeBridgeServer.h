// LiandriMapForge editor bridge server. Editor-only.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

class FSocket;
class FJsonObject;
class UWorld;

/** One connected client: socket plus the bytes received so far (until a full '\n'-terminated line arrives). */
struct FMapForgeClient
{
	FSocket* Socket;
	TArray<uint8> RecvBuf;

	FMapForgeClient() : Socket(nullptr) {}
};

/**
 * Localhost-only JSON-lines TCP endpoint exposing UnrealEd's import/build/save
 * machinery to the MapForge MCP proxy (Tools/mapgen/mapforge_mcp.py).
 *
 * Protocol: one JSON object per line, both directions.
 *   request:  {"id": <any>, "cmd": "<verb>", "args": {...}}
 *   response: {"id": <echoed>, "ok": true, "result": {...}}
 *           | {"id": <echoed>, "ok": false, "error": "..."}
 *
 * Everything runs on the game thread via the core ticker — editor APIs are
 * game-thread-only, so the socket is non-blocking and polled once per frame
 * (same pattern as the engine's SlateRemote plugin).
 */
class FMapForgeBridgeServer
{
public:
	FMapForgeBridgeServer();
	~FMapForgeBridgeServer();

private:
	bool Tick(float DeltaTime);
	bool StartListening();
	void AcceptPending();
	void PumpClients();
	void CloseClient(FMapForgeClient& Client);

	/** Parse one request line, dispatch, and return the single-line JSON response. */
	FString HandleRequest(const FString& Line);
	TSharedPtr<FJsonObject> Dispatch(const FString& Cmd, const TSharedRef<FJsonObject>& Args, FString& OutError);
	void SendLine(FMapForgeClient& Client, const FString& Line);

	// Verbs. Each returns the "result" object, or null + OutError on failure.
	TSharedPtr<FJsonObject> CmdStatus(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError);
	TSharedPtr<FJsonObject> CmdPreloadAssets(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError);
	TSharedPtr<FJsonObject> CmdImportT3D(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError);
	TSharedPtr<FJsonObject> CmdExportT3D(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError);
	TSharedPtr<FJsonObject> CmdListActors(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError);
	TSharedPtr<FJsonObject> CmdDeleteActors(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError);
	TSharedPtr<FJsonObject> CmdExec(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError);
	TSharedPtr<FJsonObject> CmdRebuildGeometry(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError);
	TSharedPtr<FJsonObject> CmdBuild(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError);
	TSharedPtr<FJsonObject> CmdSaveLevel(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError);
	TSharedPtr<FJsonObject> CmdSetSurfaceMaterial(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError);

	FSocket* ListenSocket;
	TArray<FMapForgeClient> Clients;
	FDelegateHandle TickHandle;
	uint16 Port;
	bool bTriedListen;
};
