// LiandriMapForge editor bridge server. Editor-only.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "UObject/GCObject.h"

class FSocket;
class FJsonObject;
class UWorld;

/** One connected client: socket, inbound bytes awaiting a full line, and a
    queued outbound buffer drained a bounded amount per tick. */
struct FMapForgeClient
{
	FSocket* Socket;
	TArray<uint8> RecvBuf;
	TArray<uint8> SendBuf;
	int32 SendCursor;
	float SendStallSeconds;

	FMapForgeClient()
		: Socket(nullptr)
		, SendCursor(0)
		, SendStallSeconds(0.0f)
	{}
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
 * Everything runs on the game thread via the core ticker (editor APIs are
 * game-thread-only); the socket is non-blocking and polled once per frame
 * (the engine's SlateRemote pattern).
 *
 * Socket semantics in this fork (SocketsBSD.cpp): Recv returns true with 0
 * bytes for EWOULDBLOCK and FALSE for orderly close/error. Send does NOT
 * translate would-block -- distinguish via GetLastErrorCode()==SE_EWOULDBLOCK.
 *
 * Security posture (deliberate): no authentication; bound to 127.0.0.1 on a
 * trusted single-user dev box. Any local process can drive the editor while
 * it runs. Do not weaken the loopback bind.
 *
 * Implements FGCObject to root assets loaded via preload_assets, so they
 * cannot be garbage-collected between a preload and the import that needs
 * them (StaticLoadObject alone does not create a GC reference).
 */
class FMapForgeBridgeServer : public FGCObject
{
public:
	FMapForgeBridgeServer();
	virtual ~FMapForgeBridgeServer();

	// FGCObject
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;

private:
	// Limits. Line cap comfortably fits a captured full map (~2.4 MB raw,
	// larger as escaped JSON) with a wide margin.
	static const int32 MaxClients = 4;
	static const int32 MaxLineBytes = 64 * 1024 * 1024;
	static const int32 MaxRecvBytesPerTick = 8 * 1024 * 1024;
	static const int32 MaxSendChunkBytes = 256 * 1024;
	static const int32 MaxQueuedSendBytes = 256 * 1024 * 1024;
	static const int32 SendStallTimeoutSeconds = 30;

	bool Tick(float DeltaTime);
	bool StartListening();
	void AcceptPending();
	void CloseClient(FMapForgeClient& Client);

	/** Returns false when the client must be dropped (peer closed / error / cap exceeded). */
	bool PumpClientRecv(FMapForgeClient& Client);
	bool ProcessClientLines(FMapForgeClient& Client);
	bool PumpClientSend(FMapForgeClient& Client, float DeltaTime);
	void QueueLine(FMapForgeClient& Client, const FString& Line);

	/** Parse one request line, dispatch, and return the single-line JSON response. */
	FString HandleRequest(const FString& Line);
	TSharedPtr<FJsonObject> Dispatch(const FString& Cmd, const TSharedRef<FJsonObject>& Args, FString& OutError);

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
	TSharedPtr<FJsonObject> CmdForgeChassisPhysAsset(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError);
	TSharedPtr<FJsonObject> CmdCreateBlueprint(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError);
	TSharedPtr<FJsonObject> CmdSetClassDefaults(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError);
	TSharedPtr<FJsonObject> CmdAddVariable(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError);
	TSharedPtr<FJsonObject> CmdImportGraph(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError);
	TSharedPtr<FJsonObject> CmdCompileBlueprint(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError);
	TSharedPtr<FJsonObject> CmdExportGraph(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError);
	TSharedPtr<FJsonObject> CmdImportStaticMesh(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError);
	TSharedPtr<FJsonObject> CmdConfigureStaticMesh(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError);
	TSharedPtr<FJsonObject> CmdPlaceStaticMesh(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError);

	FSocket* ListenSocket;
	TArray<FMapForgeClient> Clients;
	FDelegateHandle TickHandle;
	/** Assets pinned by preload_assets (see class comment). */
	TArray<UObject*> PreloadedAssets;
	uint16 Port;
	bool bTriedListen;
};
