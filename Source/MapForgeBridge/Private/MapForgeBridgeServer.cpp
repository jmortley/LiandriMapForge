// LiandriMapForge editor bridge server. Editor-only.
#include "MapForgeBridgeServer.h"

#include "UnrealEd.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/Selection.h"
#include "EngineUtils.h"
#include "EditorBuildUtils.h"
#include "FileHelpers.h"
#include "ActorEditorUtils.h"
#include "ScopedTransaction.h"
#include "GameFramework/WorldSettings.h"
#include "Model.h"
#include "Materials/MaterialInterface.h"
#include "Engine/SkeletalMesh.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/BodySetup.h"
#include "AssetRegistryModule.h"
#include "Misc/EngineVersion.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraphUtilities.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AutomatedAssetImportData.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "Factories/FbxMeshImportData.h"
#include "Factories/SoundFactory.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundNodeMixer.h"
#include "Sound/SoundNodeModulator.h"
#include "Sound/SoundNodeRandom.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundWave.h"
#include "PhysicsEngine/BodySetupEnums.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "StaticMeshResources.h"
#include "HAL/FileManager.h"

#include "Networking.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

#include "Json.h"

DEFINE_LOG_CATEGORY_STATIC(LogMapForgeBridge, Log, All);

namespace
{
	const uint16 DefaultPort = 8765;

	/** Mutating verbs are gated on editor state (PIE, builds); read-only ones are not. */
	bool IsMutatingCmd(const FString& Cmd)
	{
		return Cmd == TEXT("import_t3d")
			|| Cmd == TEXT("delete_actors")
			|| Cmd == TEXT("exec")
			|| Cmd == TEXT("rebuild_geometry")
			|| Cmd == TEXT("build")
			|| Cmd == TEXT("save_level")
			|| Cmd == TEXT("set_surface_material")
			|| Cmd == TEXT("forge_chassis_physasset")
			|| Cmd == TEXT("create_blueprint")
			|| Cmd == TEXT("set_class_defaults")
			|| Cmd == TEXT("add_variable")
			|| Cmd == TEXT("import_graph")
			|| Cmd == TEXT("compile_blueprint")
			|| Cmd == TEXT("import_static_mesh")
			|| Cmd == TEXT("configure_static_mesh")
			|| Cmd == TEXT("place_static_mesh")
			|| Cmd == TEXT("import_sound")
			|| Cmd == TEXT("create_sound_cue");
	}

	/** Reject command families that can load/replace the whole map or otherwise
	    take the editor down (the MAP LOAD of a bad .umap that OOM-crashed the
	    bridge). Callers must use the dedicated verbs instead. Blocklist, not
	    allowlist, so existing ad-hoc exec use (MAP CHECK, etc.) keeps working. */
	bool IsDangerousExecCommand(const FString& Command, FString& OutReason)
	{
		TArray<FString> Tokens;
		Command.ParseIntoArrayWS(Tokens);
		if (Tokens.Num() == 0)
		{
			return false;
		}
		const FString A = Tokens[0].ToUpper();
		const FString B = Tokens.Num() > 1 ? Tokens[1].ToUpper() : FString();

		if (A == TEXT("MAP") && (B == TEXT("LOAD") || B == TEXT("NEW") || B == TEXT("IMPORT")))
		{
			OutReason = TEXT("blocked: 'MAP LOAD/NEW/IMPORT' via exec can load an incompatible .umap and fatally crash the editor; use import_t3d / dedicated verbs");
			return true;
		}
		if (A == TEXT("OBJ") && (B == TEXT("IMPORT") || B == TEXT("LOAD")))
		{
			OutReason = TEXT("blocked: 'OBJ IMPORT/LOAD' via exec is unsafe; use import_static_mesh");
			return true;
		}
		if (A == TEXT("QUIT") || A == TEXT("EXIT"))
		{
			OutReason = TEXT("blocked: quit/exit would terminate the editor");
			return true;
		}
		return false;
	}

	/** Snapshot of the actor selection, restorable after bridge work clobbers it. */
	struct FScopedActorSelection
	{
		TArray<TWeakObjectPtr<AActor>> Saved;
		bool bRestore;

		explicit FScopedActorSelection(bool bInRestore)
			: bRestore(bInRestore)
		{
			if (bRestore)
			{
				for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
				{
					if (AActor* Actor = Cast<AActor>(*It))
					{
						Saved.Add(Actor);
					}
				}
			}
		}

		~FScopedActorSelection()
		{
			if (bRestore)
			{
				GEditor->SelectNone(false, true, false);
				for (const TWeakObjectPtr<AActor>& Weak : Saved)
				{
					if (AActor* Actor = Weak.Get())
					{
						if (!Actor->IsPendingKill())
						{
							GEditor->SelectActor(Actor, true, false, true);
						}
					}
				}
				GEditor->NoteSelectionChange();
			}
		}
	};
}

/* Lifecycle
 *****************************************************************************/

FMapForgeBridgeServer::FMapForgeBridgeServer()
	: ListenSocket(nullptr)
	, Port(DefaultPort)
	, bTriedListen(false)
{
	// [MapForgeBridge] Port= in the per-project editor ini, -MapForgePort= wins.
	int32 ConfigPort = 0;
	if (GConfig != nullptr && GConfig->GetInt(TEXT("MapForgeBridge"), TEXT("Port"), ConfigPort, GEditorPerProjectIni) && ConfigPort > 0 && ConfigPort <= MAX_uint16)
	{
		Port = (uint16)ConfigPort;
	}
	int32 CmdLinePort = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("MapForgePort="), CmdLinePort) && CmdLinePort > 0 && CmdLinePort <= MAX_uint16)
	{
		Port = (uint16)CmdLinePort;
	}

	TickHandle = FTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FMapForgeBridgeServer::Tick), 0.0f);
}

FMapForgeBridgeServer::~FMapForgeBridgeServer()
{
	FTicker::GetCoreTicker().RemoveTicker(TickHandle);

	for (FMapForgeClient& Client : Clients)
	{
		CloseClient(Client);
	}
	Clients.Empty();

	if (ListenSocket != nullptr)
	{
		ListenSocket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ListenSocket);
		ListenSocket = nullptr;
	}
}

void FMapForgeBridgeServer::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObjects(PreloadedAssets);
}

bool FMapForgeBridgeServer::Tick(float DeltaTime)
{
	// The core ticker starts firing while the editor is still booting; wait
	// for the editor singletons before opening shop.
	if (GEditor == nullptr || GUnrealEd == nullptr)
	{
		return true;
	}

	if (!bTriedListen)
	{
		bTriedListen = true;
		StartListening();
	}

	if (ListenSocket != nullptr)
	{
		AcceptPending();

		for (int32 ClientIdx = Clients.Num() - 1; ClientIdx >= 0; --ClientIdx)
		{
			FMapForgeClient& Client = Clients[ClientIdx];
			const bool bAlive =
				PumpClientSend(Client, DeltaTime)
				&& PumpClientRecv(Client)
				&& ProcessClientLines(Client)
				&& PumpClientSend(Client, 0.0f);
			if (!bAlive)
			{
				CloseClient(Client);
				Clients.RemoveAt(ClientIdx);
			}
		}
	}
	return true;
}

bool FMapForgeBridgeServer::StartListening()
{
	// Localhost only, by design: the bridge is a local authoring tool and must
	// never be reachable from the network. There is deliberately no auth on
	// top (trusted single-user dev box) -- see the class comment.
	const FIPv4Endpoint Endpoint(FIPv4Address(127, 0, 0, 1), Port);
	ListenSocket = FTcpSocketBuilder(TEXT("MapForgeBridge"))
		.AsNonBlocking()
		.AsReusable()
		.BoundToEndpoint(Endpoint)
		.Listening(8);

	if (ListenSocket == nullptr)
	{
		UE_LOG(LogMapForgeBridge, Error, TEXT("Failed to listen on 127.0.0.1:%d (port in use? another editor instance?)"), (int32)Port);
		return false;
	}

	UE_LOG(LogMapForgeBridge, Log, TEXT("Listening on 127.0.0.1:%d"), (int32)Port);
	return true;
}

void FMapForgeBridgeServer::AcceptPending()
{
	bool bPending = false;
	while (ListenSocket->HasPendingConnection(bPending) && bPending)
	{
		FSocket* Incoming = ListenSocket->Accept(TEXT("MapForgeBridgeClient"));
		if (Incoming == nullptr)
		{
			break;
		}
		if (Clients.Num() >= MaxClients)
		{
			UE_LOG(LogMapForgeBridge, Warning, TEXT("Rejecting connection: %d clients already active"), Clients.Num());
			Incoming->Close();
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Incoming);
			bPending = false;
			continue;
		}
		Incoming->SetNonBlocking(true);
		int32 ActualSize = 0;
		Incoming->SetReceiveBufferSize(4 * 1024 * 1024, ActualSize);
		Incoming->SetSendBufferSize(4 * 1024 * 1024, ActualSize);

		FMapForgeClient Client;
		Client.Socket = Incoming;
		Clients.Add(Client);
		UE_LOG(LogMapForgeBridge, Log, TEXT("Client connected (%d active)"), Clients.Num());
		bPending = false;
	}
}

void FMapForgeBridgeServer::CloseClient(FMapForgeClient& Client)
{
	if (Client.Socket != nullptr)
	{
		Client.Socket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Client.Socket);
		Client.Socket = nullptr;
	}
}

bool FMapForgeBridgeServer::PumpClientRecv(FMapForgeClient& Client)
{
	// This fork's FSocketBSD::Recv (SocketsBSD.cpp:187): would-block => true
	// with 0 bytes; orderly close or error => false. So a false return is the
	// ONLY disconnect signal, and true/0 just means nothing more this tick.
	int32 ReadThisTick = 0;
	for (;;)
	{
		uint8 Chunk[64 * 1024];
		int32 BytesRead = 0;
		if (!Client.Socket->Recv(Chunk, sizeof(Chunk), BytesRead))
		{
			return false; // peer closed, or socket error
		}
		if (BytesRead <= 0)
		{
			break; // would-block: drained for now
		}
		if (Client.RecvBuf.Num() + BytesRead > MaxLineBytes)
		{
			UE_LOG(LogMapForgeBridge, Warning, TEXT("Dropping client: request exceeds %d byte line cap"), MaxLineBytes);
			return false;
		}
		Client.RecvBuf.Append(Chunk, BytesRead);
		ReadThisTick += BytesRead;
		if (ReadThisTick >= MaxRecvBytesPerTick)
		{
			break; // bound this frame's work; resume next tick
		}
	}
	return true;
}

bool FMapForgeBridgeServer::ProcessClientLines(FMapForgeClient& Client)
{
	// Consume with a cursor, compact once at the end (no per-line RemoveAt(0)).
	int32 Consumed = 0;
	for (;;)
	{
		int32 NewlineIdx = INDEX_NONE;
		for (int32 ByteIdx = Consumed; ByteIdx < Client.RecvBuf.Num(); ++ByteIdx)
		{
			if (Client.RecvBuf[ByteIdx] == (uint8)'\n')
			{
				NewlineIdx = ByteIdx;
				break;
			}
		}
		if (NewlineIdx == INDEX_NONE)
		{
			break;
		}

		int32 LineEnd = NewlineIdx;
		while (LineEnd > Consumed && Client.RecvBuf[LineEnd - 1] == (uint8)'\r')
		{
			--LineEnd;
		}

		if (LineEnd > Consumed)
		{
			FUTF8ToTCHAR Converter((const ANSICHAR*)Client.RecvBuf.GetData() + Consumed, LineEnd - Consumed);
			const FString Line(Converter.Length(), Converter.Get());
			QueueLine(Client, HandleRequest(Line));
		}
		Consumed = NewlineIdx + 1;
	}

	if (Consumed > 0)
	{
		Client.RecvBuf.RemoveAt(0, Consumed, false);
	}
	// Over-cap send queue means the client stopped reading; drop it.
	return Client.SendBuf.Num() <= MaxQueuedSendBytes;
}

void FMapForgeBridgeServer::QueueLine(FMapForgeClient& Client, const FString& Line)
{
	const FString Payload = Line + TEXT("\n");
	FTCHARToUTF8 Converter(*Payload);
	Client.SendBuf.Append((const uint8*)Converter.Get(), Converter.Length());
}

bool FMapForgeBridgeServer::PumpClientSend(FMapForgeClient& Client, float DeltaTime)
{
	// Bounded, non-sleeping drain: never stall the game thread (the old code
	// slept in a retry loop for up to ~10s on a dead peer). Send does NOT
	// translate would-block in this fork -- a false return needs
	// GetLastErrorCode() to distinguish full-buffer from dead-socket.
	while (Client.SendCursor < Client.SendBuf.Num())
	{
		const int32 ToSend = FMath::Min(Client.SendBuf.Num() - Client.SendCursor, MaxSendChunkBytes);
		int32 Sent = 0;
		if (Client.Socket->Send(Client.SendBuf.GetData() + Client.SendCursor, ToSend, Sent) && Sent > 0)
		{
			Client.SendCursor += Sent;
			Client.SendStallSeconds = 0.0f;
			continue;
		}

		const ESocketErrors LastError = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLastErrorCode();
		if (Sent <= 0 && LastError == SE_EWOULDBLOCK)
		{
			Client.SendStallSeconds += DeltaTime;
			if (Client.SendStallSeconds > (float)SendStallTimeoutSeconds)
			{
				UE_LOG(LogMapForgeBridge, Warning, TEXT("Dropping client: send stalled %ds with %d bytes queued"),
					SendStallTimeoutSeconds, Client.SendBuf.Num() - Client.SendCursor);
				return false;
			}
			break; // kernel buffer full; retry next tick
		}
		UE_LOG(LogMapForgeBridge, Log, TEXT("Dropping client: send failed (socket error %d)"), (int32)LastError);
		return false;
	}

	if (Client.SendCursor > 0 && Client.SendCursor == Client.SendBuf.Num())
	{
		Client.SendBuf.Reset();
		Client.SendCursor = 0;
	}
	return true;
}

/* Request handling
 *****************************************************************************/

FString FMapForgeBridgeServer::HandleRequest(const FString& Line)
{
	TSharedPtr<FJsonObject> Req;
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
		FJsonSerializer::Deserialize(Reader, Req);
	}

	TSharedRef<FJsonObject> Resp = MakeShareable(new FJsonObject());
	TSharedPtr<FJsonValue> IdValue = Req.IsValid() ? Req->TryGetField(TEXT("id")) : TSharedPtr<FJsonValue>();
	Resp->SetField(TEXT("id"), IdValue.IsValid() ? IdValue : TSharedPtr<FJsonValue>(MakeShareable(new FJsonValueNull())));

	FString Error;
	TSharedPtr<FJsonObject> Result;

	if (!Req.IsValid())
	{
		Error = TEXT("request is not valid JSON");
	}
	else
	{
		FString Cmd;
		if (!Req->TryGetStringField(TEXT("cmd"), Cmd))
		{
			Error = TEXT("missing 'cmd'");
		}
		else
		{
			TSharedPtr<FJsonObject> ArgsObj;
			const TSharedPtr<FJsonObject>* ArgsPtr = nullptr;
			if (Req->TryGetObjectField(TEXT("args"), ArgsPtr) && ArgsPtr != nullptr && ArgsPtr->IsValid())
			{
				ArgsObj = *ArgsPtr;
			}
			else
			{
				ArgsObj = MakeShareable(new FJsonObject());
			}
			Result = Dispatch(Cmd, ArgsObj.ToSharedRef(), Error);
		}
	}

	if (Result.IsValid())
	{
		Resp->SetBoolField(TEXT("ok"), true);
		Resp->SetObjectField(TEXT("result"), Result);
	}
	else
	{
		Resp->SetBoolField(TEXT("ok"), false);
		Resp->SetStringField(TEXT("error"), Error.IsEmpty() ? TEXT("unknown error") : Error);
	}

	// Condensed writer => guaranteed single line.
	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Resp, Writer);
	return Out;
}

TSharedPtr<FJsonObject> FMapForgeBridgeServer::Dispatch(const FString& Cmd, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (World == nullptr || World->GetCurrentLevel() == nullptr)
	{
		OutError = TEXT("no editor world/level");
		return nullptr;
	}

	// Gate mutators on editor state: mutating the source world during PIE/SIE
	// desyncs the play world and saving packages mid-PIE is unsafe; mutating
	// during an async build invalidates its snapshot. Read-only verbs
	// (status/list/export/preload) stay available.
	if (IsMutatingCmd(Cmd))
	{
		if (GEditor->PlayWorld != nullptr || GEditor->bIsSimulatingInEditor)
		{
			OutError = TEXT("rejected: PIE/simulate session is active");
			return nullptr;
		}
		if (GUnrealEd->IsLightingBuildCurrentlyRunning())
		{
			OutError = TEXT("rejected: lighting build in progress (poll 'status')");
			return nullptr;
		}
		if (Cmd != TEXT("build") && FEditorBuildUtils::IsBuildCurrentlyRunning())
		{
			OutError = TEXT("rejected: editor build in progress (poll 'status')");
			return nullptr;
		}
	}

	UE_LOG(LogMapForgeBridge, Log, TEXT("cmd=%s"), *Cmd);

	if (Cmd == TEXT("ping") || Cmd == TEXT("status"))          { return CmdStatus(World, Args, OutError); }
	if (Cmd == TEXT("preload_assets"))                          { return CmdPreloadAssets(World, Args, OutError); }
	if (Cmd == TEXT("import_t3d"))                              { return CmdImportT3D(World, Args, OutError); }
	if (Cmd == TEXT("export_t3d"))                              { return CmdExportT3D(World, Args, OutError); }
	if (Cmd == TEXT("list_actors"))                             { return CmdListActors(World, Args, OutError); }
	if (Cmd == TEXT("delete_actors"))                           { return CmdDeleteActors(World, Args, OutError); }
	if (Cmd == TEXT("exec"))                                    { return CmdExec(World, Args, OutError); }
	if (Cmd == TEXT("rebuild_geometry"))                        { return CmdRebuildGeometry(World, Args, OutError); }
	if (Cmd == TEXT("build"))                                   { return CmdBuild(World, Args, OutError); }
	if (Cmd == TEXT("save_level"))                              { return CmdSaveLevel(World, Args, OutError); }
	if (Cmd == TEXT("set_surface_material"))                    { return CmdSetSurfaceMaterial(World, Args, OutError); }
	if (Cmd == TEXT("forge_chassis_physasset"))                 { return CmdForgeChassisPhysAsset(World, Args, OutError); }
	if (Cmd == TEXT("create_blueprint"))                        { return CmdCreateBlueprint(World, Args, OutError); }
	if (Cmd == TEXT("set_class_defaults"))                      { return CmdSetClassDefaults(World, Args, OutError); }
	if (Cmd == TEXT("add_variable"))                            { return CmdAddVariable(World, Args, OutError); }
	if (Cmd == TEXT("import_graph"))                            { return CmdImportGraph(World, Args, OutError); }
	if (Cmd == TEXT("compile_blueprint"))                       { return CmdCompileBlueprint(World, Args, OutError); }
	if (Cmd == TEXT("export_graph"))                            { return CmdExportGraph(World, Args, OutError); }
	if (Cmd == TEXT("import_static_mesh"))                      { return CmdImportStaticMesh(World, Args, OutError); }
	if (Cmd == TEXT("configure_static_mesh"))                   { return CmdConfigureStaticMesh(World, Args, OutError); }
	if (Cmd == TEXT("place_static_mesh"))                       { return CmdPlaceStaticMesh(World, Args, OutError); }
	if (Cmd == TEXT("import_sound"))                            { return CmdImportSound(World, Args, OutError); }
	if (Cmd == TEXT("create_sound_cue"))                       { return CmdCreateSoundCue(World, Args, OutError); }
	// Read-only recovery helpers -- deliberately NOT in IsMutatingCmd.
	if (Cmd == TEXT("recovery_layout"))                         { return CmdRecoveryLayout(World, Args, OutError); }
	if (Cmd == TEXT("inspect_static_mesh_actors"))              { return CmdInspectStaticMeshActors(World, Args, OutError); }

	OutError = FString::Printf(TEXT("unknown cmd '%s'"), *Cmd);
	return nullptr;
}

/* Verbs
 *****************************************************************************/

TSharedPtr<FJsonObject> FMapForgeBridgeServer::CmdStatus(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	ULevel* Level = World->GetCurrentLevel();
	const bool bLighting = GUnrealEd->IsLightingBuildCurrentlyRunning();
	const bool bEditorBuild = FEditorBuildUtils::IsBuildCurrentlyRunning();

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetStringField(TEXT("engine"), FEngineVersion::Current().ToString());
	Result->SetStringField(TEXT("map"), Level->GetOutermost()->GetName());
	Result->SetBoolField(TEXT("map_dirty"), Level->GetOutermost()->IsDirty());
	Result->SetBoolField(TEXT("level_locked"), Level->bLocked != 0);
	Result->SetNumberField(TEXT("actors"), Level->Actors.Num());
	// IsBuildCurrentlyRunning() only tracks FEditorBuildUtils' own InProgressBuildId;
	// Lightmass has its own flag. Report both, keep 'building' as the union.
	Result->SetBoolField(TEXT("lighting_building"), bLighting);
	Result->SetBoolField(TEXT("editor_building"), bEditorBuild);
	Result->SetBoolField(TEXT("building"), bLighting || bEditorBuild);
	Result->SetBoolField(TEXT("pie_active"), GEditor->PlayWorld != nullptr || GEditor->bIsSimulatingInEditor);
	return Result;
}

TSharedPtr<FJsonObject> FMapForgeBridgeServer::CmdPreloadAssets(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	const TArray<TSharedPtr<FJsonValue>>* Paths = nullptr;
	if (!Args->TryGetArrayField(TEXT("paths"), Paths))
	{
		OutError = TEXT("missing 'paths' array");
		return nullptr;
	}

	bool bClear = false;
	Args->TryGetBoolField(TEXT("clear"), bClear);
	if (bClear)
	{
		PreloadedAssets.Empty();
	}

	TArray<TSharedPtr<FJsonValue>> Failed;
	int32 LoadedCount = 0;
	for (const TSharedPtr<FJsonValue>& Value : *Paths)
	{
		FString Path;
		if (!Value.IsValid() || !Value->TryGetString(Path) || Path.IsEmpty())
		{
			continue;
		}
		// Loading resolves the path; PreloadedAssets (rooted via FGCObject)
		// keeps it alive until the import that needs it -- StaticLoadObject
		// alone does not protect against a GC between the two commands.
		UObject* Loaded = StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
		if (Loaded != nullptr)
		{
			PreloadedAssets.AddUnique(Loaded);
			++LoadedCount;
		}
		else
		{
			Failed.Add(MakeShareable(new FJsonValueString(Path)));
		}
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetNumberField(TEXT("loaded"), LoadedCount);
	Result->SetNumberField(TEXT("pinned"), PreloadedAssets.Num());
	Result->SetArrayField(TEXT("failed"), Failed);
	return Result;
}

TSharedPtr<FJsonObject> FMapForgeBridgeServer::CmdImportT3D(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	FString T3D;
	if (!Args->TryGetStringField(TEXT("t3d"), T3D) || T3D.IsEmpty())
	{
		OutError = TEXT("missing 't3d'");
		return nullptr;
	}

	ULevel* Level = World->GetCurrentLevel();
	if (Level->bLocked)
	{
		OutError = TEXT("current level is locked");
		return nullptr;
	}

	// ULevelFactory silently imports nothing unless the buffer starts with
	// "Begin Map" (EditorFactories.cpp:478), so wrap bare actor lists.
	FString Probe = T3D.Left(200);
	Probe.Trim();
	if (!Probe.StartsWith(TEXT("Begin Map")))
	{
		T3D = FString(TEXT("Begin Map\n   Begin Level\n")) + T3D + TEXT("\n   End Level\nEnd Map\n");
	}

	const FScopedTransaction Transaction(FText::FromString(TEXT("MapForge Import T3D")));

	// edactPasteSelected pastes *components* instead of actors if any are
	// selected, so clear both selection sets. It selects what it creates
	// (which we deliberately keep selected afterwards -- same UX as a manual
	// paste); that selection is also how we enumerate the new actors.
	GEditor->GetSelectedComponents()->DeselectAll();
	GEditor->SelectNone(false, true, false);

	GUnrealEd->edactPasteSelected(World, false, false, false, &T3D);

	TArray<TSharedPtr<FJsonValue>> Imported;
	for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
	{
		AActor* Actor = Cast<AActor>(*It);
		if (Actor == nullptr)
		{
			continue;
		}
		TSharedRef<FJsonObject> Entry = MakeShareable(new FJsonObject());
		Entry->SetStringField(TEXT("name"), Actor->GetName());
		Entry->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Entry->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
		Entry->SetStringField(TEXT("path"), Actor->GetPathName());
		Imported.Add(MakeShareable(new FJsonValueObject(Entry)));
	}

	if (Imported.Num() == 0)
	{
		OutError = TEXT("import produced no actors (malformed T3D, or referenced assets not loaded)");
		return nullptr;
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetNumberField(TEXT("count"), Imported.Num());
	Result->SetArrayField(TEXT("actors"), Imported);
	return Result;
}

TSharedPtr<FJsonObject> FMapForgeBridgeServer::CmdExportT3D(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	bool bSelectedOnly = false;
	Args->TryGetBoolField(TEXT("selected_only"), bSelectedOnly);

	// A selected component would route edactCopySelected down its component
	// branch, which never fills DestinationData -- clear it in both modes.
	GEditor->GetSelectedComponents()->DeselectAll();

	FString ExportedT3D;
	int32 ExportedCount = 0;

	if (bSelectedOnly)
	{
		for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
		{
			++ExportedCount;
		}
		GUnrealEd->edactCopySelected(World, &ExportedT3D);
	}
	else
	{
		// Current level only: edactCopySelected pops a modal warning and
		// skips actors outside the current level, so selecting across
		// streaming sublevels would go interactive mid-export.
		FScopedActorSelection RestoreSelection(true);
		GEditor->SelectNone(false, true, false);
		for (AActor* Actor : World->GetCurrentLevel()->Actors)
		{
			if (Actor != nullptr && !Actor->IsPendingKill())
			{
				GEditor->SelectActor(Actor, true, false, true);
				++ExportedCount;
			}
		}
		GUnrealEd->edactCopySelected(World, &ExportedT3D);
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetStringField(TEXT("t3d"), ExportedT3D);
	Result->SetNumberField(TEXT("length"), ExportedT3D.Len());
	Result->SetNumberField(TEXT("actors_selected"), ExportedCount);
	return Result;
}

TSharedPtr<FJsonObject> FMapForgeBridgeServer::CmdListActors(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	FString NameContains, ClassContains;
	Args->TryGetStringField(TEXT("name_contains"), NameContains);
	Args->TryGetStringField(TEXT("class_contains"), ClassContains);
	double LimitValue = 500.0;
	Args->TryGetNumberField(TEXT("limit"), LimitValue);
	const int32 Limit = FMath::Clamp((int32)LimitValue, 1, 10000);
	bool bAllLevels = false;
	Args->TryGetBoolField(TEXT("all_levels"), bAllLevels);

	// Object names are only unique per level, so default to the current level
	// (matching what delete_actors operates on) and include the full path.
	TArray<AActor*> Candidates;
	if (bAllLevels)
	{
		for (FActorIterator It(World); It; ++It)
		{
			Candidates.Add(*It);
		}
	}
	else
	{
		Candidates = World->GetCurrentLevel()->Actors;
	}

	int32 Matched = 0;
	TArray<TSharedPtr<FJsonValue>> Actors;
	for (AActor* Actor : Candidates)
	{
		if (Actor == nullptr || Actor->IsPendingKill())
		{
			continue;
		}
		if (!NameContains.IsEmpty()
			&& !Actor->GetName().Contains(NameContains)
			&& !Actor->GetActorLabel().Contains(NameContains))
		{
			continue;
		}
		if (!ClassContains.IsEmpty() && !Actor->GetClass()->GetName().Contains(ClassContains))
		{
			continue;
		}

		++Matched;
		if (Actors.Num() < Limit)
		{
			const FVector Location = Actor->GetActorLocation();
			TSharedRef<FJsonObject> Entry = MakeShareable(new FJsonObject());
			Entry->SetStringField(TEXT("name"), Actor->GetName());
			Entry->SetStringField(TEXT("label"), Actor->GetActorLabel());
			Entry->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
			if (bAllLevels)
			{
				Entry->SetStringField(TEXT("level"), Actor->GetLevel() ? Actor->GetLevel()->GetOutermost()->GetName() : TEXT(""));
			}
			TSharedRef<FJsonObject> Loc = MakeShareable(new FJsonObject());
			Loc->SetNumberField(TEXT("x"), Location.X);
			Loc->SetNumberField(TEXT("y"), Location.Y);
			Loc->SetNumberField(TEXT("z"), Location.Z);
			Entry->SetObjectField(TEXT("location"), Loc);
			Actors.Add(MakeShareable(new FJsonValueObject(Entry)));
		}
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetArrayField(TEXT("actors"), Actors);
	Result->SetNumberField(TEXT("matched"), Matched);
	Result->SetBoolField(TEXT("truncated"), Matched > Actors.Num());
	return Result;
}

TSharedPtr<FJsonObject> FMapForgeBridgeServer::CmdDeleteActors(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	const TArray<TSharedPtr<FJsonValue>>* NamesArray = nullptr;
	if (!Args->TryGetArrayField(TEXT("names"), NamesArray))
	{
		OutError = TEXT("missing 'names' array");
		return nullptr;
	}

	ULevel* Level = World->GetCurrentLevel();
	if (Level->bLocked)
	{
		OutError = TEXT("current level is locked");
		return nullptr;
	}

	TArray<FString> Names;
	for (const TSharedPtr<FJsonValue>& Value : *NamesArray)
	{
		FString Name;
		if (Value.IsValid() && Value->TryGetString(Name) && !Name.IsEmpty())
		{
			Names.Add(Name);
		}
	}

	// Current level only: names are only unique per level, and matching across
	// sublevels deleted more than callers asked for.
	FScopedActorSelection RestoreSelection(true);
	GEditor->GetSelectedComponents()->DeselectAll();
	GEditor->SelectNone(false, true, false);

	TArray<TSharedPtr<FJsonValue>> MatchedNames;
	for (AActor* Actor : Level->Actors)
	{
		if (Actor == nullptr || Actor->IsPendingKill()
			|| FActorEditorUtils::IsABuilderBrush(Actor) || Actor->IsA(AWorldSettings::StaticClass()))
		{
			continue;
		}
		for (const FString& Name : Names)
		{
			if (Actor->GetName().Equals(Name, ESearchCase::IgnoreCase)
				|| Actor->GetActorLabel().Equals(Name, ESearchCase::IgnoreCase))
			{
				GEditor->SelectActor(Actor, true, false, true);
				MatchedNames.Add(MakeShareable(new FJsonValueString(Actor->GetPathName())));
				break;
			}
		}
	}

	bool bDeleted = false;
	if (MatchedNames.Num() > 0)
	{
		const FScopedTransaction Transaction(FText::FromString(TEXT("MapForge Delete Actors")));
		// bVerifyDeletionCanHappen=true can open dialogs; we preflight the
		// locked-level case ourselves above to stay headless.
		bDeleted = GUnrealEd->edactDeleteSelected(World, false, false);
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetNumberField(TEXT("matched"), MatchedNames.Num());
	Result->SetArrayField(TEXT("matched_paths"), MatchedNames);
	Result->SetBoolField(TEXT("deleted"), bDeleted);
	return Result;
}

TSharedPtr<FJsonObject> FMapForgeBridgeServer::CmdExec(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	FString Command;
	if (!Args->TryGetStringField(TEXT("command"), Command) || Command.IsEmpty())
	{
		OutError = TEXT("missing 'command'");
		return nullptr;
	}

	// Guard BEFORE Exec: a fatal engine failure (e.g. MAP LOAD of a bad .umap
	// OOM) can't be caught in C++, so dangerous families are prevented, not
	// recovered.
	FString DangerReason;
	if (IsDangerousExecCommand(Command, DangerReason))
	{
		OutError = DangerReason;
		return nullptr;
	}

	FStringOutputDevice Output;
	const bool bHandled = GEditor->Exec(World, *Command, Output);
	GEditor->RedrawLevelEditingViewports();

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("handled"), bHandled);
	Result->SetStringField(TEXT("output"), Output);
	return Result;
}

TSharedPtr<FJsonObject> FMapForgeBridgeServer::CmdRebuildGeometry(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	// "BSP REBUILD" is a deprecated no-op in this engine version; MAP REBUILD
	// (EditorServer.cpp:6071 -> Map_Rebuild) is the real CSG rebuild path.
	FStringOutputDevice Output;
	const bool bHandled = GEditor->Exec(World, TEXT("MAP REBUILD ALLVISIBLE"), Output);
	GEditor->RedrawLevelEditingViewports();

	if (!bHandled)
	{
		OutError = TEXT("MAP REBUILD was not handled by the editor");
		return nullptr;
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetStringField(TEXT("output"), Output);
	return Result;
}

TSharedPtr<FJsonObject> FMapForgeBridgeServer::CmdBuild(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	FString What = TEXT("lighting");
	Args->TryGetStringField(TEXT("what"), What);

	FName BuildId;
	if (What == TEXT("lighting"))      { BuildId = FBuildOptions::BuildLighting; }
	else if (What == TEXT("geometry")) { BuildId = FBuildOptions::BuildGeometry; }
	else if (What == TEXT("paths"))    { BuildId = FBuildOptions::BuildAIPaths; }
	else if (What == TEXT("all"))      { BuildId = FBuildOptions::BuildAll; }
	else
	{
		OutError = FString::Printf(TEXT("unknown build type '%s' (lighting|geometry|paths|all)"), *What);
		return nullptr;
	}

	// Dispatch() already rejects while a lighting/editor build runs, so a
	// duplicate request can't double-start one.
	const bool bStarted = FEditorBuildUtils::EditorBuild(World, BuildId, /*bAllowLightingDialog*/ false);
	if (!bStarted)
	{
		OutError = FString::Printf(TEXT("editor build '%s' failed to start"), *What);
		return nullptr;
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("started"), true);
	// Lighting completes asynchronously -- poll 'status' until
	// lighting_building=false. Explicit navigation builds run synchronously
	// in this fork (NavigationSystem::Build), so 'paths' is done on return.
	Result->SetBoolField(TEXT("lighting_building"), GUnrealEd->IsLightingBuildCurrentlyRunning());
	Result->SetBoolField(TEXT("editor_building"), FEditorBuildUtils::IsBuildCurrentlyRunning());
	return Result;
}

TSharedPtr<FJsonObject> FMapForgeBridgeServer::CmdSaveLevel(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	FString Filename;
	Args->TryGetStringField(TEXT("filename"), Filename);
	bool bAllowDialog = false;
	Args->TryGetBoolField(TEXT("allow_dialog"), bAllowDialog);

	ULevel* Level = World->GetCurrentLevel();

	// A never-saved level with no filename opens a modal Save-As dialog and
	// wedges the headless endpoint until a human answers -- fail fast unless
	// the caller explicitly opts into interactive behavior.
	if (Filename.IsEmpty() && !bAllowDialog && FEditorFileUtils::GetFilename(Level).IsEmpty())
	{
		OutError = TEXT("level has never been saved; pass 'filename' (or allow_dialog=true for the interactive Save As)");
		return nullptr;
	}

	FString SavedFilename;
	const bool bSaved = FEditorFileUtils::SaveLevel(Level, Filename, &SavedFilename);
	if (!bSaved)
	{
		OutError = TEXT("save failed (source control checkout declined, path invalid, or dialog cancelled)");
		return nullptr;
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("saved"), true);
	Result->SetStringField(TEXT("filename"), SavedFilename);
	return Result;
}

TSharedPtr<FJsonObject> FMapForgeBridgeServer::CmdSetSurfaceMaterial(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	FString MaterialPath;
	if (!Args->TryGetStringField(TEXT("material"), MaterialPath) || MaterialPath.IsEmpty())
	{
		OutError = TEXT("missing 'material'");
		return nullptr;
	}

	ULevel* Level = World->GetCurrentLevel();
	if (Level->bLocked)
	{
		OutError = TEXT("current level is locked");
		return nullptr;
	}

	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	if (Material == nullptr)
	{
		OutError = FString::Printf(TEXT("could not load material '%s'"), *MaterialPath);
		return nullptr;
	}
	PreloadedAssets.AddUnique(Material);

	FString BrushFilter;
	Args->TryGetStringField(TEXT("brush"), BrushFilter);

	bool bHasNormalFilter = false;
	FVector NormalFilter = FVector::ZeroVector;
	const TArray<TSharedPtr<FJsonValue>>* NormalArray = nullptr;
	if (Args->TryGetArrayField(TEXT("normal"), NormalArray) && NormalArray->Num() == 3)
	{
		NormalFilter.X = (*NormalArray)[0]->AsNumber();
		NormalFilter.Y = (*NormalArray)[1]->AsNumber();
		NormalFilter.Z = (*NormalArray)[2]->AsNumber();
		NormalFilter = NormalFilter.GetSafeNormal();
		bHasNormalFilter = !NormalFilter.IsNearlyZero();
	}
	double Tolerance = 0.1;
	Args->TryGetNumberField(TEXT("tolerance"), Tolerance);

	// POLY SETMATERIAL works off the editor's live selection (content browser
	// material + selected surfaces) -- hostile to external driving. Set the
	// surface directly; ModifySurf(..., true) also transacts the master brush
	// poly so undo restores both sides, then polyUpdateMaster syncs it so the
	// change survives a CSG rebuild (same as EditorServer.cpp:4379).
	const FScopedTransaction Transaction(FText::FromString(TEXT("MapForge Set Surface Material")));

	UModel* Model = Level->Model;
	int32 Changed = 0;
	for (int32 SurfIdx = 0; SurfIdx < Model->Surfs.Num(); ++SurfIdx)
	{
		FBspSurf& Surf = Model->Surfs[SurfIdx];

		if (!BrushFilter.IsEmpty())
		{
			if (Surf.Actor == nullptr
				|| (!Surf.Actor->GetName().Contains(BrushFilter)
					&& !Surf.Actor->GetActorLabel().Contains(BrushFilter)))
			{
				continue;
			}
		}
		if (bHasNormalFilter)
		{
			const FVector SurfNormal = Model->Vectors[Surf.vNormal].GetSafeNormal();
			if (FVector::DotProduct(SurfNormal, NormalFilter) < 1.0 - Tolerance)
			{
				continue;
			}
		}

		Model->ModifySurf(SurfIdx, true);
		Surf.Material = Material;
		GEditor->polyUpdateMaster(Model, SurfIdx, false, true);
		++Changed;
	}

	if (Changed > 0)
	{
		Model->MarkPackageDirty();
		ULevel::LevelDirtiedEvent.Broadcast();
		GEditor->RedrawLevelEditingViewports();
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetNumberField(TEXT("changed"), Changed);
	Result->SetNumberField(TEXT("total_surfaces"), Model->Surfs.Num());
	return Result;
}

namespace
{
	/** Rename an object out to the transient package + mark it for GC, so its
	    package name is free for a fresh asset (the overwrite/rollback idiom). */
	void TrashObject(UObject* Obj)
	{
		Obj->ClearFlags(RF_Public | RF_Standalone);
		const FName TrashName = MakeUniqueObjectName(GetTransientPackage(), Obj->GetClass(), FName(TEXT("MapForgeTrash")));
		Obj->Rename(*TrashName.ToString(), GetTransientPackage(),
			REN_DontCreateRedirectors | REN_NonTransactional | REN_ForceNoResetLoaders);
		Obj->MarkPendingKill();
	}

	/** Resolve a class reference: bare native name ("UTMutator"), a full object
	    path ("/Script/UnrealTournament.UTMutator"), or a BP package path
	    ("/Game/Mods/BP_Base" -> its BP_Base_C generated class). */
	UClass* ResolveClass(const FString& Ref)
	{
		if (Ref.IsEmpty())
		{
			return nullptr;
		}
		if (Ref.Contains(TEXT(".")))
		{
			return LoadObject<UClass>(nullptr, *Ref);
		}
		if (Ref.StartsWith(TEXT("/")))
		{
			const FString Leaf = FPackageName::GetShortName(Ref);
			return LoadObject<UClass>(nullptr, *(Ref + TEXT(".") + Leaf + TEXT("_C")));
		}
		return FindObject<UClass>(ANY_PACKAGE, *Ref);
	}

	/** A JSON scalar as a UProperty::ImportText string. Strings pass through so
	    callers can supply UE literals ("(X=1,Y=2,Z=3)", class paths, enum names). */
	FString JsonScalarToImportString(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return FString();
		}
		if (Value->Type == EJson::Boolean)
		{
			return Value->AsBool() ? TEXT("true") : TEXT("false");
		}
		if (Value->Type == EJson::Number)
		{
			const double N = Value->AsNumber();
			if (FMath::Abs(N) < 1e15 && FMath::IsNearlyEqual(N, FMath::RoundToDouble(N)))
			{
				return FString::Printf(TEXT("%lld"), (int64)N);
			}
			return FString::SanitizeFloat(N);
		}
		return Value->AsString();
	}

	/** Load a UBlueprint from a bare package path or a full object path. */
	UBlueprint* LoadBlueprintByRef(const FString& AssetArg)
	{
		FString Path = AssetArg;
		if (!Path.Contains(TEXT(".")))
		{
			Path = AssetArg + TEXT(".") + FPackageName::GetShortName(AssetArg);
		}
		return LoadObject<UBlueprint>(nullptr, *Path);
	}

	/** Save the Blueprint's package to disk. */
	bool SaveBlueprintPackage(UBlueprint* Blueprint)
	{
		UPackage* Pkg = Blueprint->GetOutermost();
		const FString Filename = FPackageName::LongPackageNameToFilename(
			Pkg->GetName(), FPackageName::GetAssetPackageExtension());
		return UPackage::SavePackage(Pkg, Blueprint, RF_Public | RF_Standalone,
			*Filename, GError, nullptr, false, true, SAVE_NoError);
	}

	/** Build an FEdGraphPinType from a type spec: "bool"|"int"|"float"|"string"|
	    "name"|"byte", or "object:<class>"|"class:<class>"|"struct:<path>", with an
	    optional "[]" array suffix. Category strings match UEdGraphSchema_K2's
	    PC_* literals (verified against real graph exports), so no BlueprintGraph
	    link is needed. */
	bool BuildPinType(const FString& TypeSpec, FEdGraphPinType& Out, FString& Err)
	{
		FString Spec = TypeSpec;
		bool bIsArray = false;
		if (Spec.EndsWith(TEXT("[]")))
		{
			bIsArray = true;
			Spec = Spec.LeftChop(2);
		}
		FString Cat, Sub;
		if (!Spec.Split(TEXT(":"), &Cat, &Sub))
		{
			Cat = Spec;
		}
		Cat = Cat.ToLower();

		UObject* SubObj = nullptr;
		if (Cat == TEXT("bool") || Cat == TEXT("int") || Cat == TEXT("float")
			|| Cat == TEXT("string") || Cat == TEXT("name") || Cat == TEXT("byte"))
		{
			// scalar; no sub-object
		}
		else if (Cat == TEXT("object") || Cat == TEXT("class"))
		{
			SubObj = ResolveClass(Sub);
			if (SubObj == nullptr)
			{
				Err = FString::Printf(TEXT("could not resolve %s class '%s'"), *Cat, *Sub);
				return false;
			}
		}
		else if (Cat == TEXT("struct"))
		{
			SubObj = LoadObject<UScriptStruct>(nullptr, *Sub);
			if (SubObj == nullptr)
			{
				Err = FString::Printf(TEXT("could not resolve struct '%s'"), *Sub);
				return false;
			}
		}
		else
		{
			Err = FString::Printf(TEXT("unsupported variable type '%s'"), *TypeSpec);
			return false;
		}

		Out = FEdGraphPinType(Cat, FString(), SubObj, bIsArray, false);
		return true;
	}

	/** Resolve a target graph on a Blueprint: empty/"event"/"eventgraph" -> the
	    primary ubergraph; otherwise a ubergraph or function graph by name. */
	UEdGraph* ResolveGraph(UBlueprint* Blueprint, const FString& GraphName, FString& OutError)
	{
		if (GraphName.IsEmpty()
			|| GraphName.Equals(TEXT("event"), ESearchCase::IgnoreCase)
			|| GraphName.Equals(TEXT("eventgraph"), ESearchCase::IgnoreCase))
		{
			if (Blueprint->UbergraphPages.Num() == 0)
			{
				OutError = TEXT("blueprint has no event graph");
				return nullptr;
			}
			return Blueprint->UbergraphPages[0];
		}
		for (UEdGraph* G : Blueprint->UbergraphPages)
		{
			if (G && G->GetName() == GraphName) { return G; }
		}
		for (UEdGraph* G : Blueprint->FunctionGraphs)
		{
			if (G && G->GetName() == GraphName) { return G; }
		}
		OutError = FString::Printf(TEXT("no graph named '%s'"), *GraphName);
		return nullptr;
	}

	const TCHAR* BlueprintStatusString(EBlueprintStatus Status)
	{
		switch (Status)
		{
		case BS_Dirty:                 return TEXT("dirty");
		case BS_Error:                 return TEXT("error");
		case BS_UpToDate:              return TEXT("up_to_date");
		case BS_BeingCreated:          return TEXT("being_created");
		case BS_UpToDateWithWarnings:  return TEXT("up_to_date_with_warnings");
		default:                       return TEXT("unknown");
		}
	}

	/* ---- static-mesh import/config/place helpers ---- */

	/** Save any asset's package to disk (mesh, etc). */
	bool SaveAssetPackage(UObject* Asset)
	{
		UPackage* Pkg = Asset->GetOutermost();
		const FString Filename = FPackageName::LongPackageNameToFilename(
			Pkg->GetName(), FPackageName::GetAssetPackageExtension());
		return UPackage::SavePackage(Pkg, Asset, RF_Public | RF_Standalone,
			*Filename, GError, nullptr, false, true, SAVE_NoError);
	}

	const TCHAR* CollisionTraceFlagString(ECollisionTraceFlag F)
	{
		switch (F)
		{
		case CTF_UseSimpleAndComplex: return TEXT("simple_and_complex");
		case CTF_UseSimpleAsComplex:  return TEXT("simple_as_complex");
		case CTF_UseComplexAsSimple:  return TEXT("complex_as_simple");
		case CTF_UseDefault:          return TEXT("default");
		default:                      return TEXT("unknown");
		}
	}

	bool MeshExtensionOk(const FString& Path, FString& OutExt, FString& OutErr)
	{
		OutExt = FPaths::GetExtension(Path).ToLower();
		if (OutExt == TEXT("obj") || OutExt == TEXT("fbx"))
		{
			return true;
		}
		OutErr = FString::Printf(TEXT("unsupported extension '.%s' (only .obj and .fbx allowed)"), *OutExt);
		return false;
	}

	/** Destination must be a /Game content folder -- never /Engine, a file, or a
	    map/package path. */
	bool ValidContentDestination(const FString& Dest, FString& OutErr)
	{
		if (!Dest.StartsWith(TEXT("/Game/")))
		{
			OutErr = FString::Printf(TEXT("destination must be under /Game/ (got '%s')"), *Dest);
			return false;
		}
		if (Dest.Contains(TEXT("..")) || Dest.Contains(TEXT(".umap")) || Dest.Contains(TEXT(".uasset")))
		{
			OutErr = TEXT("destination must be a /Game content folder, not a file or traversal path");
			return false;
		}
		return true;
	}

	/** Keep only [A-Za-z0-9_]; other chars become '_'. */
	FString SanitizeMeshAssetName(const FString& In)
	{
		FString Out;
		for (int32 i = 0; i < In.Len(); ++i)
		{
			const TCHAR C = In[i];
			const bool bOk = (C >= '0' && C <= '9') || (C >= 'A' && C <= 'Z')
				|| (C >= 'a' && C <= 'z') || C == '_';
			Out.AppendChar(bOk ? C : TEXT('_'));
		}
		return Out;
	}

	/** Set the collision trace mode ("none"|"simple"|"complex_as_simple") and
	    optionally strip generated simple-collision primitives. "none" always
	    strips simple. Leaves the mesh unchanged for empty/unknown modes. */
	void ApplyCollision(UStaticMesh* Mesh, const FString& Mode, bool bClearSimple, TArray<FString>& Warnings)
	{
		UBodySetup* BS = Mesh->BodySetup;
		if (BS == nullptr)
		{
			if (!Mode.IsEmpty() || bClearSimple)
			{
				Warnings.Add(TEXT("mesh has no BodySetup; collision unchanged"));
			}
			return;
		}
		BS->Modify();

		const FString M = Mode.ToLower();
		if (M == TEXT("none"))
		{
			BS->CollisionTraceFlag = CTF_UseSimpleAndComplex;
			bClearSimple = true;
		}
		else if (M == TEXT("simple"))
		{
			BS->CollisionTraceFlag = CTF_UseSimpleAndComplex;
		}
		else if (M == TEXT("complex_as_simple"))
		{
			BS->CollisionTraceFlag = CTF_UseComplexAsSimple;
		}
		else if (!M.IsEmpty())
		{
			Warnings.Add(FString::Printf(TEXT("unknown collision mode '%s'; trace flag left unchanged"), *Mode));
		}

		if (bClearSimple)
		{
			BS->AggGeom.EmptyElements();
			BS->InvalidatePhysicsData();
			BS->CreatePhysicsMeshes();
		}
	}

	/** Set lightmap coordinate index + generation on SourceModel[0] and rebuild
	    the single asset (Build() is per-asset DDC, not a global editor build). */
	void ApplyLightmapAndRebuild(UStaticMesh* Mesh, int32 LightmapIndex, bool bGenerateLightmapUVs)
	{
		Mesh->Modify();
		if (Mesh->SourceModels.Num() > 0)
		{
			FMeshBuildSettings& Settings = Mesh->SourceModels[0].BuildSettings;
			Settings.bGenerateLightmapUVs = bGenerateLightmapUVs;
			if (LightmapIndex >= 0)
			{
				Settings.DstLightmapIndex = LightmapIndex;
			}
		}
		if (LightmapIndex >= 0)
		{
			Mesh->LightMapCoordinateIndex = LightmapIndex;
		}
		Mesh->Build(true);
		Mesh->PostEditChange();
	}

	/** Populate slot count, LOD0 verts/tris/UVs, bounds, and collision stats. */
	void FillMeshStats(UStaticMesh* Mesh, const TSharedRef<FJsonObject>& Result)
	{
		Result->SetNumberField(TEXT("material_slot_count"), Mesh->StaticMaterials.Num());

		int32 Verts = 0, Tris = 0, UVs = 0;
		if (Mesh->RenderData.IsValid() && Mesh->RenderData->LODResources.Num() > 0)
		{
			const FStaticMeshLODResources& LOD0 = Mesh->RenderData->LODResources[0];
			Verts = LOD0.GetNumVertices();
			Tris = LOD0.GetNumTriangles();
			UVs = (int32)LOD0.GetNumTexCoords();
		}
		Result->SetNumberField(TEXT("lod0_vertices"), Verts);
		Result->SetNumberField(TEXT("lod0_triangles"), Tris);
		Result->SetNumberField(TEXT("uv_channels"), UVs);

		const FBoxSphereBounds B = Mesh->GetBounds();
		TSharedRef<FJsonObject> Bounds = MakeShareable(new FJsonObject());
		TSharedRef<FJsonObject> Origin = MakeShareable(new FJsonObject());
		Origin->SetNumberField(TEXT("x"), B.Origin.X);
		Origin->SetNumberField(TEXT("y"), B.Origin.Y);
		Origin->SetNumberField(TEXT("z"), B.Origin.Z);
		TSharedRef<FJsonObject> Extent = MakeShareable(new FJsonObject());
		Extent->SetNumberField(TEXT("x"), B.BoxExtent.X);
		Extent->SetNumberField(TEXT("y"), B.BoxExtent.Y);
		Extent->SetNumberField(TEXT("z"), B.BoxExtent.Z);
		Bounds->SetObjectField(TEXT("origin"), Origin);
		Bounds->SetObjectField(TEXT("box_extent"), Extent);
		Bounds->SetNumberField(TEXT("sphere_radius"), B.SphereRadius);
		Result->SetObjectField(TEXT("bounds"), Bounds);

		if (Mesh->BodySetup != nullptr)
		{
			Result->SetStringField(TEXT("collision_trace_mode"), CollisionTraceFlagString(Mesh->BodySetup->CollisionTraceFlag));
			Result->SetNumberField(TEXT("simple_collision_primitives"), Mesh->BodySetup->AggGeom.GetElementCount());
		}
		else
		{
			Result->SetStringField(TEXT("collision_trace_mode"), TEXT("none"));
			Result->SetNumberField(TEXT("simple_collision_primitives"), 0);
		}
	}

	/** Parse location/rotation([pitch,yaw,roll])/scale arrays; identity default. */
	FTransform ParseTransform(const TSharedRef<FJsonObject>& Args)
	{
		FVector Loc(0.f, 0.f, 0.f);
		FVector Scale(1.f, 1.f, 1.f);
		FRotator Rot(0.f, 0.f, 0.f);
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Args->TryGetArrayField(TEXT("location"), Arr) && Arr->Num() == 3)
		{
			Loc = FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
		}
		if (Args->TryGetArrayField(TEXT("rotation"), Arr) && Arr->Num() == 3)
		{
			Rot = FRotator((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
		}
		if (Args->TryGetArrayField(TEXT("scale"), Arr) && Arr->Num() == 3)
		{
			Scale = FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
		}
		return FTransform(Rot, Loc, Scale);
	}
}

TSharedPtr<FJsonObject> FMapForgeBridgeServer::CmdForgeChassisPhysAsset(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	FString MeshPath;
	if (!Args->TryGetStringField(TEXT("mesh"), MeshPath) || MeshPath.IsEmpty())
	{
		OutError = TEXT("missing 'mesh'");
		return nullptr;
	}
	FString BoneArg, TargetArg;
	Args->TryGetStringField(TEXT("bone"), BoneArg);
	Args->TryGetStringField(TEXT("target"), TargetArg);

	// The caller may pass a bare package path (/Game/A/Foo); normalize to an
	// object path (/Game/A/Foo.Foo) so LoadObject resolves it.
	FString MeshObjectPath = MeshPath;
	if (!MeshObjectPath.Contains(TEXT(".")))
	{
		MeshObjectPath = MeshPath + TEXT(".") + FPackageName::GetShortName(MeshPath);
	}

	USkeletalMesh* SkelMesh = LoadObject<USkeletalMesh>(nullptr, *MeshObjectPath);
	if (SkelMesh == nullptr)
	{
		OutError = FString::Printf(TEXT("could not load skeletal mesh '%s'"), *MeshPath);
		return nullptr;
	}

	// The mesh's default (authored) physics asset -- never modified here.
	UPhysicsAsset* SourceAsset = SkelMesh->PhysicsAsset;
	if (SourceAsset == nullptr)
	{
		OutError = FString::Printf(TEXT("mesh '%s' has no default PhysicsAsset"), *SkelMesh->GetName());
		return nullptr;
	}

	// Bone to keep: explicit arg, else the reference skeleton's root (index 0).
	FName KeepBone;
	if (!BoneArg.IsEmpty())
	{
		KeepBone = FName(*BoneArg);
	}
	else
	{
		const FReferenceSkeleton& RefSkel = SkelMesh->RefSkeleton;
		if (RefSkel.GetNum() <= 0)
		{
			OutError = FString::Printf(TEXT("mesh '%s' has an empty reference skeleton"), *SkelMesh->GetName());
			return nullptr;
		}
		KeepBone = RefSkel.GetBoneName(0);
	}

	// Resolve target package + asset short name.
	FString TargetPackage = TargetArg.IsEmpty()
		? (FPackageName::ObjectPathToPackageName(MeshObjectPath) + TEXT("_Physics"))
		: FPackageName::ObjectPathToPackageName(TargetArg);
	if (!FPackageName::IsValidLongPackageName(TargetPackage))
	{
		OutError = FString::Printf(TEXT("invalid target package name '%s'"), *TargetPackage);
		return nullptr;
	}
	const FString TargetShortName = FPackageName::GetShortName(TargetPackage);

	UPackage* TargetPkg = CreatePackage(nullptr, *TargetPackage);
	if (TargetPkg == nullptr)
	{
		OutError = FString::Printf(TEXT("could not create package '%s'"), *TargetPackage);
		return nullptr;
	}
	// Refuse to overwrite the mesh's own physics asset -- trashing it would
	// leave the mesh pointing at a dead object.
	if (TargetPkg == SourceAsset->GetOutermost())
	{
		OutError = TEXT("target resolves to the mesh's existing PhysicsAsset package; choose a different 'target'");
		return nullptr;
	}
	TargetPkg->FullyLoad();

	// Clean overwrite: evict any prior object of this name in the package.
	if (UObject* Existing = StaticFindObjectFast(nullptr, TargetPkg, FName(*TargetShortName)))
	{
		TrashObject(Existing);
	}

	// Duplicate-and-strip (never regenerate) preserves the authored UT3
	// chassis collision geometry instead of boxing it.
	UPhysicsAsset* NewAsset = Cast<UPhysicsAsset>(
		StaticDuplicateObject(SourceAsset, TargetPkg, FName(*TargetShortName)));
	if (NewAsset == nullptr)
	{
		OutError = TEXT("failed to duplicate physics asset");
		return nullptr;
	}
	NewAsset->SetFlags(RF_Public | RF_Standalone);

	// Locate the kept body in the duplicate.
	int32 KeptIndex = INDEX_NONE;
	TArray<FString> BodyBones;
	for (int32 BodyIdx = 0; BodyIdx < NewAsset->SkeletalBodySetups.Num(); ++BodyIdx)
	{
		USkeletalBodySetup* Setup = NewAsset->SkeletalBodySetups[BodyIdx];
		if (Setup == nullptr)
		{
			continue;
		}
		BodyBones.Add(Setup->BoneName.ToString());
		if (Setup->BoneName == KeepBone)
		{
			KeptIndex = BodyIdx;
		}
	}
	if (KeptIndex == INDEX_NONE)
	{
		TrashObject(NewAsset); // don't leave a half-built asset in the package
		OutError = FString::Printf(TEXT("bone '%s' has no body in the physics asset; bodies exist on: %s"),
			*KeepBone.ToString(), *FString::Join(BodyBones, TEXT(", ")));
		return nullptr;
	}

	const int32 RemovedBodies = NewAsset->SkeletalBodySetups.Num() - 1;
	const int32 RemovedConstraints = NewAsset->ConstraintSetup.Num();

	USkeletalBodySetup* KeptSetup = NewAsset->SkeletalBodySetups[KeptIndex];
	NewAsset->SkeletalBodySetups.Empty();
	NewAsset->SkeletalBodySetups.Add(KeptSetup);
	NewAsset->ConstraintSetup.Empty();
	// Copied pair-wise table is keyed by body indices, all stale after the strip.
	NewAsset->CollisionDisableTable.Empty();
	NewAsset->UpdateBodySetupIndexMap();
	NewAsset->UpdateBoundsBodiesArray();

	NewAsset->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewAsset);

	const FString Filename = FPackageName::LongPackageNameToFilename(
		TargetPackage, FPackageName::GetAssetPackageExtension());
	const bool bSaved = UPackage::SavePackage(TargetPkg, NewAsset,
		RF_Public | RF_Standalone, *Filename, GError, nullptr, false, true, SAVE_NoError);
	if (!bSaved)
	{
		OutError = FString::Printf(TEXT("asset stripped but SavePackage failed for '%s'"), *Filename);
		return nullptr;
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetStringField(TEXT("asset"), TargetPackage + TEXT(".") + TargetShortName);
	Result->SetStringField(TEXT("kept_bone"), KeepBone.ToString());
	Result->SetNumberField(TEXT("removed_bodies"), RemovedBodies);
	Result->SetNumberField(TEXT("removed_constraints"), RemovedConstraints);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return Result;
}

TSharedPtr<FJsonObject> FMapForgeBridgeServer::CmdCreateBlueprint(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	FString ParentArg, PackageArg, NameArg;
	if (!Args->TryGetStringField(TEXT("parent"), ParentArg) || ParentArg.IsEmpty())
	{
		OutError = TEXT("missing 'parent'");
		return nullptr;
	}
	if (!Args->TryGetStringField(TEXT("package"), PackageArg) || PackageArg.IsEmpty())
	{
		OutError = TEXT("missing 'package'");
		return nullptr;
	}
	Args->TryGetStringField(TEXT("name"), NameArg);
	bool bOverwrite = false;
	Args->TryGetBoolField(TEXT("overwrite"), bOverwrite);

	UClass* ParentClass = ResolveClass(ParentArg);
	if (ParentClass == nullptr)
	{
		OutError = FString::Printf(TEXT("could not resolve parent class '%s'"), *ParentArg);
		return nullptr;
	}
	if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
	{
		OutError = FString::Printf(TEXT("class '%s' cannot be a Blueprint parent"), *ParentClass->GetName());
		return nullptr;
	}

	// Resolve target package + asset short name (mirrors forge_chassis_physasset).
	FString TargetPackage = FPackageName::ObjectPathToPackageName(PackageArg);
	FString AssetName;
	if (!NameArg.IsEmpty())
	{
		TargetPackage = FPackageName::GetLongPackagePath(TargetPackage) / NameArg;
		AssetName = NameArg;
	}
	else
	{
		AssetName = FPackageName::GetShortName(TargetPackage);
	}
	if (!FPackageName::IsValidLongPackageName(TargetPackage))
	{
		OutError = FString::Printf(TEXT("invalid target package name '%s'"), *TargetPackage);
		return nullptr;
	}

	UPackage* Pkg = CreatePackage(nullptr, *TargetPackage);
	if (Pkg == nullptr)
	{
		OutError = FString::Printf(TEXT("could not create package '%s'"), *TargetPackage);
		return nullptr;
	}
	Pkg->FullyLoad();

	if (UObject* Existing = StaticFindObjectFast(nullptr, Pkg, FName(*AssetName)))
	{
		if (!bOverwrite)
		{
			OutError = FString::Printf(TEXT("asset '%s' already exists (pass overwrite=true)"), *TargetPackage);
			return nullptr;
		}
		TrashObject(Existing);
	}

	const FScopedTransaction Transaction(FText::FromString(TEXT("MapForge Create Blueprint")));

	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		ParentClass, Pkg, FName(*AssetName), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (Blueprint == nullptr)
	{
		OutError = TEXT("CreateBlueprint returned null");
		return nullptr;
	}

	FAssetRegistryModule::AssetCreated(Blueprint);
	// Compile so GeneratedClass/CDO are populated before save (a fresh BP is
	// only a skeleton otherwise).
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	Blueprint->MarkPackageDirty();

	const FString Filename = FPackageName::LongPackageNameToFilename(
		TargetPackage, FPackageName::GetAssetPackageExtension());
	const bool bSaved = UPackage::SavePackage(Pkg, Blueprint,
		RF_Public | RF_Standalone, *Filename, GError, nullptr, false, true, SAVE_NoError);
	if (!bSaved)
	{
		OutError = FString::Printf(TEXT("blueprint created but SavePackage failed for '%s'"), *Filename);
		return nullptr;
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetStringField(TEXT("asset"), TargetPackage + TEXT(".") + AssetName);
	Result->SetStringField(TEXT("generated_class"),
		Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : FString());
	Result->SetStringField(TEXT("parent"), ParentClass->GetPathName());
	Result->SetBoolField(TEXT("saved"), bSaved);
	return Result;
}

TSharedPtr<FJsonObject> FMapForgeBridgeServer::CmdSetClassDefaults(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	FString AssetArg;
	if (!Args->TryGetStringField(TEXT("asset"), AssetArg) || AssetArg.IsEmpty())
	{
		OutError = TEXT("missing 'asset'");
		return nullptr;
	}
	const TSharedPtr<FJsonObject>* DefaultsPtr = nullptr;
	if (!Args->TryGetObjectField(TEXT("defaults"), DefaultsPtr) || DefaultsPtr == nullptr || !DefaultsPtr->IsValid())
	{
		OutError = TEXT("missing 'defaults' object");
		return nullptr;
	}

	// Normalize a bare package path to the Blueprint object path.
	FString BPObjectPath = AssetArg;
	if (!BPObjectPath.Contains(TEXT(".")))
	{
		BPObjectPath = AssetArg + TEXT(".") + FPackageName::GetShortName(AssetArg);
	}
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BPObjectPath);
	if (Blueprint == nullptr)
	{
		OutError = FString::Printf(TEXT("could not load Blueprint '%s'"), *AssetArg);
		return nullptr;
	}
	if (Blueprint->GeneratedClass == nullptr)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	if (Blueprint->GeneratedClass == nullptr)
	{
		OutError = TEXT("Blueprint has no GeneratedClass (compile failed?)");
		return nullptr;
	}
	// The class-defaults panel edits the generated class CDO; do the same and
	// serialize it. Inherited native props of the parent are valid targets.
	UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
	if (CDO == nullptr)
	{
		OutError = TEXT("Blueprint class has no default object");
		return nullptr;
	}

	const FScopedTransaction Transaction(FText::FromString(TEXT("MapForge Set Class Defaults")));
	CDO->Modify();

	TArray<TSharedPtr<FJsonValue>> Applied, Failed;
	for (const auto& Pair : (*DefaultsPtr)->Values)
	{
		UProperty* Prop = FindField<UProperty>(Blueprint->GeneratedClass, *Pair.Key);
		if (Prop == nullptr)
		{
			Failed.Add(MakeShareable(new FJsonValueString(Pair.Key + TEXT(" (no such property)"))));
			continue;
		}
		const FString ValueStr = JsonScalarToImportString(Pair.Value);
		void* Dest = Prop->ContainerPtrToValuePtr<void>(CDO);
		if (Prop->ImportText(*ValueStr, Dest, PPF_None, CDO) == nullptr)
		{
			Failed.Add(MakeShareable(new FJsonValueString(Pair.Key + TEXT(" (could not import '") + ValueStr + TEXT("')"))));
		}
		else
		{
			Applied.Add(MakeShareable(new FJsonValueString(Pair.Key)));
		}
	}

	Blueprint->MarkPackageDirty();

	UPackage* Pkg = Blueprint->GetOutermost();
	const FString Filename = FPackageName::LongPackageNameToFilename(
		Pkg->GetName(), FPackageName::GetAssetPackageExtension());
	const bool bSaved = UPackage::SavePackage(Pkg, Blueprint,
		RF_Public | RF_Standalone, *Filename, GError, nullptr, false, true, SAVE_NoError);

	if (Applied.Num() == 0 && Failed.Num() > 0)
	{
		OutError = TEXT("no defaults applied (all property names/values rejected)");
		return nullptr;
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetArrayField(TEXT("applied"), Applied);
	Result->SetArrayField(TEXT("failed"), Failed);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return Result;
}

TSharedPtr<FJsonObject> FMapForgeBridgeServer::CmdAddVariable(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	FString AssetArg, NameArg, TypeArg, DefaultArg;
	if (!Args->TryGetStringField(TEXT("asset"), AssetArg) || AssetArg.IsEmpty())
	{
		OutError = TEXT("missing 'asset'");
		return nullptr;
	}
	if (!Args->TryGetStringField(TEXT("name"), NameArg) || NameArg.IsEmpty())
	{
		OutError = TEXT("missing 'name'");
		return nullptr;
	}
	if (!Args->TryGetStringField(TEXT("type"), TypeArg) || TypeArg.IsEmpty())
	{
		OutError = TEXT("missing 'type'");
		return nullptr;
	}
	Args->TryGetStringField(TEXT("default"), DefaultArg);

	UBlueprint* Blueprint = LoadBlueprintByRef(AssetArg);
	if (Blueprint == nullptr)
	{
		OutError = FString::Printf(TEXT("could not load Blueprint '%s'"), *AssetArg);
		return nullptr;
	}

	FEdGraphPinType PinType;
	if (!BuildPinType(TypeArg, PinType, OutError))
	{
		return nullptr;
	}

	const FScopedTransaction Transaction(FText::FromString(TEXT("MapForge Add Variable")));
	const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*NameArg), PinType, DefaultArg);
	if (!bAdded)
	{
		OutError = FString::Printf(TEXT("AddMemberVariable failed for '%s' (name already taken, or invalid type)"), *NameArg);
		return nullptr;
	}

	const bool bSaved = SaveBlueprintPackage(Blueprint);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetStringField(TEXT("variable"), NameArg);
	Result->SetStringField(TEXT("type"), TypeArg);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return Result;
}

TSharedPtr<FJsonObject> FMapForgeBridgeServer::CmdImportGraph(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	FString AssetArg, T3D, GraphArg;
	if (!Args->TryGetStringField(TEXT("asset"), AssetArg) || AssetArg.IsEmpty())
	{
		OutError = TEXT("missing 'asset'");
		return nullptr;
	}
	if (!Args->TryGetStringField(TEXT("t3d"), T3D) || T3D.IsEmpty())
	{
		OutError = TEXT("missing 't3d'");
		return nullptr;
	}
	Args->TryGetStringField(TEXT("graph"), GraphArg);

	UBlueprint* Blueprint = LoadBlueprintByRef(AssetArg);
	if (Blueprint == nullptr)
	{
		OutError = FString::Printf(TEXT("could not load Blueprint '%s'"), *AssetArg);
		return nullptr;
	}

	UEdGraph* Dest = ResolveGraph(Blueprint, GraphArg, OutError);
	if (Dest == nullptr)
	{
		return nullptr;
	}

	// The K2 node factory that materializes K2Node_* from the text is registered
	// by the (loaded) BlueprintGraph module at runtime, so we resolve those
	// classes without linking against them.
	if (!FEdGraphUtilities::CanImportNodesFromText(Dest, T3D))
	{
		OutError = TEXT("graph text is not importable into the target graph (schema mismatch or malformed)");
		return nullptr;
	}

	const FScopedTransaction Transaction(FText::FromString(TEXT("MapForge Import Graph")));
	Dest->Modify();

	TSet<UEdGraphNode*> ImportedNodes;
	FEdGraphUtilities::ImportNodesFromText(Dest, T3D, ImportedNodes);

	if (ImportedNodes.Num() == 0)
	{
		OutError = TEXT("no nodes imported (malformed graph text?)");
		return nullptr;
	}

	// Same post-paste fix-up the Blueprint editor runs: fresh GUIDs to avoid
	// collisions on repeat import, then let each node re-resolve its pins.
	TArray<TSharedPtr<FJsonValue>> Nodes;
	for (UEdGraphNode* Node : ImportedNodes)
	{
		if (Node == nullptr)
		{
			continue;
		}
		Node->CreateNewGuid();
		Node->PostPasteNode();
		Node->ReconstructNode();

		TSharedRef<FJsonObject> Entry = MakeShareable(new FJsonObject());
		Entry->SetStringField(TEXT("name"), Node->GetName());
		Entry->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		Nodes.Add(MakeShareable(new FJsonValueObject(Entry)));
	}

	Dest->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	const bool bSaved = SaveBlueprintPackage(Blueprint);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetStringField(TEXT("graph"), Dest->GetName());
	Result->SetNumberField(TEXT("count"), ImportedNodes.Num());
	Result->SetArrayField(TEXT("nodes"), Nodes);
	Result->SetBoolField(TEXT("saved"), bSaved);
	// Nodes may still be invalid until compile_blueprint; that verb reports errors.
	return Result;
}

TSharedPtr<FJsonObject> FMapForgeBridgeServer::CmdCompileBlueprint(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	FString AssetArg;
	if (!Args->TryGetStringField(TEXT("asset"), AssetArg) || AssetArg.IsEmpty())
	{
		OutError = TEXT("missing 'asset'");
		return nullptr;
	}
	UBlueprint* Blueprint = LoadBlueprintByRef(AssetArg);
	if (Blueprint == nullptr)
	{
		OutError = FString::Printf(TEXT("could not load Blueprint '%s'"), *AssetArg);
		return nullptr;
	}

	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	// Collect node-level diagnostics straight off the graph nodes (bHasCompilerMessage
	// / ErrorType / ErrorMsg) -- avoids a KismetCompiler dependency for the results log.
	TArray<TSharedPtr<FJsonValue>> Messages;
	TArray<UEdGraph*> AllGraphs;
	AllGraphs.Append(Blueprint->UbergraphPages);
	AllGraphs.Append(Blueprint->FunctionGraphs);
	for (UEdGraph* G : AllGraphs)
	{
		if (G == nullptr)
		{
			continue;
		}
		for (UEdGraphNode* Node : G->Nodes)
		{
			if (Node != nullptr && Node->bHasCompilerMessage && !Node->ErrorMsg.IsEmpty())
			{
				TSharedRef<FJsonObject> Entry = MakeShareable(new FJsonObject());
				Entry->SetStringField(TEXT("graph"), G->GetName());
				Entry->SetStringField(TEXT("node"), Node->GetName());
				Entry->SetNumberField(TEXT("error_type"), Node->ErrorType);
				Entry->SetStringField(TEXT("message"), Node->ErrorMsg);
				Messages.Add(MakeShareable(new FJsonValueObject(Entry)));
			}
		}
	}

	const bool bSaved = SaveBlueprintPackage(Blueprint);
	const EBlueprintStatus Status = Blueprint->Status;

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetStringField(TEXT("status"), BlueprintStatusString(Status));
	Result->SetBoolField(TEXT("ok"), Status == BS_UpToDate || Status == BS_UpToDateWithWarnings);
	Result->SetArrayField(TEXT("messages"), Messages);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return Result;
}

TSharedPtr<FJsonObject> FMapForgeBridgeServer::CmdExportGraph(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	FString AssetArg, GraphArg;
	if (!Args->TryGetStringField(TEXT("asset"), AssetArg) || AssetArg.IsEmpty())
	{
		OutError = TEXT("missing 'asset'");
		return nullptr;
	}
	Args->TryGetStringField(TEXT("graph"), GraphArg);

	UBlueprint* Blueprint = LoadBlueprintByRef(AssetArg);
	if (Blueprint == nullptr)
	{
		OutError = FString::Printf(TEXT("could not load Blueprint '%s'"), *AssetArg);
		return nullptr;
	}
	UEdGraph* Graph = ResolveGraph(Blueprint, GraphArg, OutError);
	if (Graph == nullptr)
	{
		return nullptr;
	}

	// Same exporter the Blueprint editor's Copy uses -- the round-trip source
	// for capturing graph fixtures.
	TSet<UObject*> NodeSet;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node != nullptr)
		{
			NodeSet.Add(Node);
		}
	}
	FString Text;
	FEdGraphUtilities::ExportNodesToText(NodeSet, Text);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetStringField(TEXT("graph"), Graph->GetName());
	Result->SetStringField(TEXT("t3d"), Text);
	Result->SetNumberField(TEXT("node_count"), NodeSet.Num());
	Result->SetNumberField(TEXT("length"), Text.Len());
	return Result;
}

namespace
{
	// ---- Map-scoped recovery layout ------------------------------------------
	// Editable/regenerated recovery assets live under <RecoveryRoot>/<MapSlug>/...
	// in Unreal content; raw PAK extraction and other non-content recovery files
	// live under Saved/LiandriMapForge/Recoveries/<MapSlug>/. Stock /Game,
	// /Engine and extracted /Game/RestrictedAssets dependencies are never moved
	// here -- only regenerated assets are placed under the recovery root.

	const TCHAR* const DefaultRecoveryRoot = TEXT("/Game/RecoveredMaps");

	/** Keep only [A-Za-z0-9_-]; every other char becomes '_' (the MapSlug rule). */
	FString SanitizeMapSlug(const FString& In)
	{
		FString Out;
		for (int32 i = 0; i < In.Len(); ++i)
		{
			const TCHAR C = In[i];
			const bool bOk = (C >= '0' && C <= '9') || (C >= 'A' && C <= 'Z')
				|| (C >= 'a' && C <= 'z') || C == TEXT('_') || C == TEXT('-');
			Out.AppendChar(bOk ? C : TEXT('_'));
		}
		return Out;
	}

	/** Strip one recovery suffix (longest first). Applied ONLY to an inferred
	    map name, never to a caller-supplied map_name. */
	FString StripRecoverySuffix(const FString& In)
	{
		static const TCHAR* const Suffixes[] = {
			TEXT("-Recovered-Editable"), TEXT("_Recovered_Editable"),
			TEXT("-Recovered"), TEXT("_Recovered")
		};
		for (const TCHAR* Suffix : Suffixes)
		{
			if (In.EndsWith(Suffix, ESearchCase::IgnoreCase))
			{
				return In.LeftChop(FCString::Strlen(Suffix));
			}
		}
		return In;
	}

	/** Resolve the map slug: caller 'map_name', else the current map's short
	    package name with recovery suffixes stripped. Rejects path/traversal
	    input and names that sanitize to empty. */
	bool ResolveMapSlug(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutSlug, FString& OutErr)
	{
		FString MapName;
		const bool bProvided = Args->TryGetStringField(TEXT("map_name"), MapName) && !MapName.IsEmpty();
		if (!bProvided)
		{
			const FString Pkg = World->GetCurrentLevel()->GetOutermost()->GetName();
			MapName = StripRecoverySuffix(FPackageName::GetShortName(Pkg));
		}
		if (MapName.Contains(TEXT("..")) || MapName.Contains(TEXT("/")) || MapName.Contains(TEXT("\\")))
		{
			OutErr = FString::Printf(TEXT("invalid map_name '%s' (must be a bare name -- no path separators or traversal)"), *MapName);
			return false;
		}
		const FString Slug = SanitizeMapSlug(MapName);
		if (Slug.IsEmpty())
		{
			OutErr = TEXT("map_name is empty after sanitization");
			return false;
		}
		OutSlug = Slug;
		return true;
	}

	/** Resolve the recovery root: 'recovery_root' arg, else [MapForgeBridge]
	    RecoveryRoot= in the editor ini, else the default. Must be a /Game
	    content path (never /Engine), with no traversal. */
	bool ResolveRecoveryRoot(const TSharedRef<FJsonObject>& Args, FString& OutRoot, FString& OutErr)
	{
		FString Root;
		if (!(Args->TryGetStringField(TEXT("recovery_root"), Root) && !Root.IsEmpty()))
		{
			Root.Empty();
			if (!(GConfig != nullptr && GConfig->GetString(TEXT("MapForgeBridge"), TEXT("RecoveryRoot"), Root, GEditorPerProjectIni) && !Root.IsEmpty()))
			{
				Root = DefaultRecoveryRoot;
			}
		}
		Root = Root.Replace(TEXT("\\"), TEXT("/"));
		while (Root.EndsWith(TEXT("/"))) { Root = Root.LeftChop(1); }
		if (Root.Contains(TEXT("..")))
		{
			OutErr = TEXT("recovery_root must not contain traversal ('..')");
			return false;
		}
		if (!Root.StartsWith(TEXT("/Game/")))
		{
			OutErr = FString::Printf(TEXT("recovery_root must be under /Game/ (got '%s')"), *Root);
			return false;
		}
		OutRoot = Root;
		return true;
	}

	/** Accept only a canonical content category; return its canonical subfolder.
	    Empty -> the default Geometry/StaticMeshes. Rejects traversal and any
	    name outside the known set. */
	bool ResolveRecoveryCategory(const FString& Category, FString& OutSub, FString& OutErr)
	{
		static const TCHAR* const Known[] = {
			TEXT("Maps"), TEXT("Geometry/BSP"), TEXT("Geometry/StaticMeshes"),
			TEXT("Materials"), TEXT("Textures"), TEXT("VFX"), TEXT("Audio"),
			TEXT("Blueprints"), TEXT("Data")
		};
		if (Category.IsEmpty())
		{
			OutSub = TEXT("Geometry/StaticMeshes");
			return true;
		}
		FString C = Category.Replace(TEXT("\\"), TEXT("/"));
		while (C.StartsWith(TEXT("/"))) { C = C.RightChop(1); }
		while (C.EndsWith(TEXT("/"))) { C = C.LeftChop(1); }
		if (C.Contains(TEXT("..")))
		{
			OutErr = TEXT("category must not contain traversal ('..')");
			return false;
		}
		for (const TCHAR* K : Known)
		{
			if (C.Equals(K, ESearchCase::IgnoreCase)) { OutSub = K; return true; }
		}
		OutErr = FString::Printf(TEXT("unknown category '%s' (allowed: Maps, Geometry/BSP, Geometry/StaticMeshes, Materials, Textures, VFX, Audio, Blueprints, Data)"), *Category);
		return false;
	}

	/** Build the full recovery-layout result: content package paths under
	    <root>/<slug>, plus the on-disk Saved paths for non-content files. */
	TSharedRef<FJsonObject> MakeRecoveryLayout(const FString& Slug, const FString& Root)
	{
		const FString ContentRoot = Root + TEXT("/") + Slug;
		const FString FsRoot = FPaths::ConvertRelativePathToFull(
			FPaths::GameSavedDir() / TEXT("LiandriMapForge") / TEXT("Recoveries") / Slug);

		TSharedRef<FJsonObject> R = MakeShareable(new FJsonObject());
		R->SetStringField(TEXT("map_slug"), Slug);
		R->SetStringField(TEXT("content_root"), ContentRoot);
		R->SetStringField(TEXT("maps"),          ContentRoot + TEXT("/Maps"));
		R->SetStringField(TEXT("bsp"),           ContentRoot + TEXT("/Geometry/BSP"));
		R->SetStringField(TEXT("static_meshes"), ContentRoot + TEXT("/Geometry/StaticMeshes"));
		R->SetStringField(TEXT("materials"),     ContentRoot + TEXT("/Materials"));
		R->SetStringField(TEXT("textures"),      ContentRoot + TEXT("/Textures"));
		R->SetStringField(TEXT("vfx"),           ContentRoot + TEXT("/VFX"));
		R->SetStringField(TEXT("audio"),         ContentRoot + TEXT("/Audio"));
		R->SetStringField(TEXT("blueprints"),    ContentRoot + TEXT("/Blueprints"));
		R->SetStringField(TEXT("data"),          ContentRoot + TEXT("/Data"));
		R->SetStringField(TEXT("filesystem_root"), FsRoot);
		R->SetStringField(TEXT("raw_extract"), FsRoot / TEXT("RawExtract"));
		R->SetStringField(TEXT("interchange"), FsRoot / TEXT("Interchange"));
		R->SetStringField(TEXT("manifests"),   FsRoot / TEXT("Manifests"));
		R->SetStringField(TEXT("reports"),     FsRoot / TEXT("Reports"));
		return R;
	}

	const TCHAR* MobilityToString(EComponentMobility::Type M)
	{
		switch (M)
		{
			case EComponentMobility::Static:     return TEXT("Static");
			case EComponentMobility::Stationary: return TEXT("Stationary");
			case EComponentMobility::Movable:    return TEXT("Movable");
			default:                             return TEXT("Unknown");
		}
	}

	const TCHAR* CollisionEnabledToString(ECollisionEnabled::Type C)
	{
		switch (C)
		{
			case ECollisionEnabled::NoCollision:     return TEXT("NoCollision");
			case ECollisionEnabled::QueryOnly:       return TEXT("QueryOnly");
			case ECollisionEnabled::PhysicsOnly:     return TEXT("PhysicsOnly");
			case ECollisionEnabled::QueryAndPhysics: return TEXT("QueryAndPhysics");
			default:                                 return TEXT("Unknown");
		}
	}

	TSharedRef<FJsonObject> MakeVec3Json(const FVector& V)
	{
		TSharedRef<FJsonObject> O = MakeShareable(new FJsonObject());
		O->SetNumberField(TEXT("x"), V.X);
		O->SetNumberField(TEXT("y"), V.Y);
		O->SetNumberField(TEXT("z"), V.Z);
		return O;
	}

	TSharedRef<FJsonObject> MakeTransformJson(const FTransform& T)
	{
		const FRotator Rot = T.Rotator();
		TSharedRef<FJsonObject> RotObj = MakeShareable(new FJsonObject());
		RotObj->SetNumberField(TEXT("pitch"), Rot.Pitch);
		RotObj->SetNumberField(TEXT("yaw"), Rot.Yaw);
		RotObj->SetNumberField(TEXT("roll"), Rot.Roll);
		TSharedRef<FJsonObject> O = MakeShareable(new FJsonObject());
		O->SetObjectField(TEXT("location"), MakeVec3Json(T.GetLocation()));
		O->SetObjectField(TEXT("rotation"), RotObj);
		O->SetObjectField(TEXT("scale"), MakeVec3Json(T.GetScale3D()));
		return O;
	}
}

TSharedPtr<FJsonObject> FMapForgeBridgeServer::CmdImportStaticMesh(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	FString Source, Destination, NameArg;
	if (!Args->TryGetStringField(TEXT("source"), Source) || Source.IsEmpty())
	{
		OutError = TEXT("missing 'source'");
		return nullptr;
	}
	// 'destination' is optional: when omitted it resolves through the canonical
	// recovery layout below. An explicit destination is used verbatim.
	Args->TryGetStringField(TEXT("destination"), Destination);
	Args->TryGetStringField(TEXT("name"), NameArg);
	bool bOverwrite = false;
	Args->TryGetBoolField(TEXT("overwrite"), bOverwrite);
	bool bSave = true;
	Args->TryGetBoolField(TEXT("save"), bSave);

	// --- validate everything before touching editor state ---
	if (FPaths::IsRelative(Source))
	{
		OutError = TEXT("'source' must be an absolute path");
		return nullptr;
	}
	if (!IFileManager::Get().FileExists(*Source))
	{
		OutError = FString::Printf(TEXT("source file not found: '%s'"), *Source);
		return nullptr;
	}
	FString Ext;
	if (!MeshExtensionOk(Source, Ext, OutError))
	{
		return nullptr;
	}

	// Resolve a default map-scoped destination when none is supplied. Existing
	// callers that pass an explicit 'destination' are unaffected (used verbatim).
	// Empty -> <RecoveryRoot>/<MapSlug>/<category>, default Geometry/StaticMeshes.
	TSharedPtr<FJsonObject> ResolvedLayout;
	if (Destination.IsEmpty())
	{
		FString Slug, Root, CategorySub, Category;
		Args->TryGetStringField(TEXT("category"), Category);
		if (!ResolveMapSlug(World, Args, Slug, OutError)) { return nullptr; }
		if (!ResolveRecoveryRoot(Args, Root, OutError)) { return nullptr; }
		if (!ResolveRecoveryCategory(Category, CategorySub, OutError)) { return nullptr; }
		Destination = Root + TEXT("/") + Slug + TEXT("/") + CategorySub;
		ResolvedLayout = MakeRecoveryLayout(Slug, Root);
	}

	if (!ValidContentDestination(Destination, OutError))
	{
		return nullptr;
	}

	FString AssetName = SanitizeMeshAssetName(NameArg.IsEmpty() ? FPaths::GetBaseFilename(Source) : NameArg);
	if (AssetName.IsEmpty())
	{
		OutError = TEXT("asset name is empty after sanitization");
		return nullptr;
	}

	FString DestFolder = Destination;
	while (DestFolder.EndsWith(TEXT("/")))
	{
		DestFolder = DestFolder.LeftChop(1);
	}
	const FString PackagePath = DestFolder + TEXT("/") + AssetName;
	const FString ObjectPath = PackagePath + TEXT(".") + AssetName;
	if (!FPackageName::IsValidLongPackageName(PackagePath))
	{
		OutError = FString::Printf(TEXT("invalid target package '%s'"), *PackagePath);
		return nullptr;
	}

	const bool bExists = (LoadObject<UObject>(nullptr, *ObjectPath) != nullptr)
		|| FPackageName::DoesPackageExist(PackagePath);
	if (bExists && !bOverwrite)
	{
		OutError = FString::Printf(TEXT("asset '%s' already exists (pass overwrite=true)"), *PackagePath);
		return nullptr;
	}

	// --- options ---
	const TSharedPtr<FJsonObject>* OptPtr = nullptr;
	TSharedPtr<FJsonObject> Opt = (Args->TryGetObjectField(TEXT("options"), OptPtr) && OptPtr != nullptr && OptPtr->IsValid())
		? *OptPtr : MakeShareable(new FJsonObject());
	auto OptBool = [&Opt](const TCHAR* Key, bool Def) { bool V = Def; Opt->TryGetBoolField(Key, V); return V; };
	const bool bCombine        = OptBool(TEXT("combine_meshes"), true);
	const bool bImportMats     = OptBool(TEXT("import_materials"), false);
	const bool bImportTex      = OptBool(TEXT("import_textures"), false);
	const bool bGenLightmap    = OptBool(TEXT("generate_lightmap_uvs"), true);
	const bool bComputeNormals = OptBool(TEXT("compute_normals"), true);
	const bool bMikk           = OptBool(TEXT("use_mikk_tspace"), false);
	FString CollisionMode;
	Opt->TryGetStringField(TEXT("collision"), CollisionMode);
	const FString ModeLower = CollisionMode.ToLower();
	double LightmapIdxD = 1.0;
	Opt->TryGetNumberField(TEXT("lightmap_coordinate_index"), LightmapIdxD);
	const int32 LightmapIndex = (int32)LightmapIdxD;
	const bool bAutoSimpleCollision = (ModeLower == TEXT("simple"));

	// --- stage a temp copy named <AssetName>.<ext>, so the automated import
	// (which names the asset after the source file) yields our exact name.
	// Track every staged file so the cleanup below removes all of them. ---
	TArray<FString> Warnings;
	TArray<FString> StagedFiles;
	const FString TempDir = FPaths::Combine(*FPaths::GameIntermediateDir(), TEXT("MapForgeImport"));
	IFileManager::Get().MakeDirectory(*TempDir, true);
	const FString TempFile = FPaths::Combine(*TempDir, *(AssetName + TEXT(".") + Ext));
	if (IFileManager::Get().Copy(*TempFile, *Source, true, true) != COPY_OK)
	{
		OutError = FString::Printf(TEXT("failed to stage source to '%s'"), *TempFile);
		return nullptr;
	}
	StagedFiles.Add(TempFile);

	// OBJ material slots come from the FBX SDK reading the .mtl sidecar named
	// in each `mtllib` directive: with no MTL alongside the OBJ the SDK reports
	// zero materials and the mesh collapses to one slot (FbxStaticMeshImport.cpp
	// forces a single default slot when MaterialCount==0). Copy each referenced
	// MTL next to the temp OBJ under its original name so the temp OBJ's
	// unchanged `mtllib` line resolves. FBX embeds its materials -- OBJ only.
	if (Ext == TEXT("obj"))
	{
		FString ObjText;
		if (FFileHelper::LoadFileToString(ObjText, *Source))
		{
			const FString SourceDir = FPaths::GetPath(Source);
			TArray<FString> Lines;
			ObjText.ParseIntoArrayLines(Lines);
			for (const FString& Line : Lines)
			{
				TArray<FString> Tokens;
				Line.ParseIntoArrayWS(Tokens);
				if (Tokens.Num() < 2 || !Tokens[0].Equals(TEXT("mtllib"), ESearchCase::IgnoreCase))
				{
					continue;
				}
				// A single `mtllib` line may list several .mtl files.
				for (int32 i = 1; i < Tokens.Num(); ++i)
				{
					const FString& MtlName = Tokens[i];
					// Keep staging strictly inside TempDir: never follow an
					// absolute path or a `..` out of it, since cleanup deletes
					// whatever we stage.
					if (!FPaths::IsRelative(MtlName) || MtlName.Contains(TEXT("..")))
					{
						Warnings.Add(FString::Printf(TEXT("ignored unsafe mtllib path '%s'"), *MtlName));
						continue;
					}
					const FString SrcMtl = FPaths::Combine(*SourceDir, *MtlName);
					const FString DstMtl = FPaths::Combine(*TempDir, *MtlName);
					if (!IFileManager::Get().FileExists(*SrcMtl))
					{
						Warnings.Add(FString::Printf(TEXT("mtllib '%s' not found next to source; material slots may collapse"), *MtlName));
						continue;
					}
					IFileManager::Get().MakeDirectory(*FPaths::GetPath(DstMtl), true);
					if (IFileManager::Get().Copy(*DstMtl, *SrcMtl, true, true) == COPY_OK)
					{
						StagedFiles.Add(DstMtl);
					}
					else
					{
						Warnings.Add(FString::Printf(TEXT("failed to stage mtllib '%s'"), *MtlName));
					}
				}
			}
		}
		else
		{
			Warnings.Add(TEXT("could not read source OBJ to stage its .mtl sidecar(s)"));
		}
	}

	// --- configure a static-mesh-only, no-dialog FBX/OBJ factory ---
	UFbxFactory* Factory = NewObject<UFbxFactory>();
	Factory->AddToRoot();
	if (UFbxImportUI* UI = Factory->ImportUI)
	{
		UI->MeshTypeToImport    = FBXIT_StaticMesh;
		UI->bImportMesh         = true;
		UI->bImportAsSkeletal   = false;
		UI->bImportMaterials    = bImportMats;
		UI->bImportTextures     = bImportTex;
		UI->bImportAnimations   = false;
		UI->bCreatePhysicsAsset = false;
		UI->bCombineMeshes      = bCombine;
		if (UFbxStaticMeshImportData* SM = UI->StaticMeshImportData)
		{
			SM->bAutoGenerateCollision = bAutoSimpleCollision;
			SM->bGenerateLightmapUVs   = bGenLightmap;
			SM->NormalImportMethod     = bComputeNormals ? FBXNIM_ComputeNormals : FBXNIM_ImportNormals;
			SM->NormalGenerationMethod = bMikk ? EFBXNormalGenerationMethod::MikkTSpace : EFBXNormalGenerationMethod::BuiltIn;
		}
	}
	Factory->SetDetectImportTypeOnImport(false); // guarantee static-only regardless of file contents

	UAutomatedAssetImportData* Data = NewObject<UAutomatedAssetImportData>();
	Data->AddToRoot();
	Data->Filenames.Add(TempFile);
	Data->DestinationPath  = DestFolder;
	Data->Factory          = Factory;
	Data->bReplaceExisting = bOverwrite;

	const FScopedTransaction Transaction(FText::FromString(TEXT("MapForge Import Static Mesh")));

	FAssetToolsModule& AssetToolsModule = FModuleManager::Get().LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	TArray<UObject*> ImportedObjects = AssetToolsModule.Get().ImportAssetsAutomated(*Data);

	Factory->RemoveFromRoot();
	Data->RemoveFromRoot();
	for (const FString& Staged : StagedFiles)
	{
		IFileManager::Get().Delete(*Staged, false, true, true);
	}

	UStaticMesh* Mesh = nullptr;
	for (UObject* Obj : ImportedObjects)
	{
		Mesh = Cast<UStaticMesh>(Obj);
		if (Mesh != nullptr)
		{
			break;
		}
	}
	if (Mesh == nullptr)
	{
		OutError = TEXT("import produced no static mesh (unsupported file contents, or the import failed)");
		return nullptr;
	}

	// --- post-import configuration (Warnings was declared during staging) ---
	ApplyCollision(Mesh, CollisionMode, /*bClearSimple*/ ModeLower == TEXT("complex_as_simple"), Warnings);
	// The import already builds with DstLightmapIndex=1; only rebuild if a
	// different lightmap channel was requested.
	if (bGenLightmap && LightmapIndex != 1)
	{
		ApplyLightmapAndRebuild(Mesh, LightmapIndex, bGenLightmap);
	}
	else if (LightmapIndex >= 0)
	{
		Mesh->LightMapCoordinateIndex = LightmapIndex;
	}
	Mesh->MarkPackageDirty();

	const bool bSaved = bSave ? SaveAssetPackage(Mesh) : false;

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetStringField(TEXT("asset"), Mesh->GetPathName());
	Result->SetStringField(TEXT("package"), Mesh->GetOutermost()->GetName());
	Result->SetStringField(TEXT("class"), Mesh->GetClass()->GetName());
	Result->SetStringField(TEXT("source"), Source);
	Result->SetStringField(TEXT("destination"), DestFolder);
	Result->SetBoolField(TEXT("created"), !bExists);
	Result->SetBoolField(TEXT("overwritten"), bExists);
	Result->SetBoolField(TEXT("saved"), bSaved);
	// When the destination was auto-resolved, echo the full recovery layout so
	// callers can record/verify where the asset landed.
	if (ResolvedLayout.IsValid())
	{
		Result->SetObjectField(TEXT("recovery_layout"), ResolvedLayout.ToSharedRef());
	}
	FillMeshStats(Mesh, Result);
	TArray<TSharedPtr<FJsonValue>> WarnArr;
	for (const FString& W : Warnings)
	{
		WarnArr.Add(MakeShareable(new FJsonValueString(W)));
	}
	Result->SetArrayField(TEXT("warnings"), WarnArr);
	return Result;
}

TSharedPtr<FJsonObject> FMapForgeBridgeServer::CmdImportSound(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	FString Source, Destination, NameArg;
	if (!Args->TryGetStringField(TEXT("source"), Source) || Source.IsEmpty())
	{
		OutError = TEXT("missing 'source'");
		return nullptr;
	}
	if (!Args->TryGetStringField(TEXT("destination"), Destination) || Destination.IsEmpty())
	{
		OutError = TEXT("missing 'destination'");
		return nullptr;
	}
	Args->TryGetStringField(TEXT("name"), NameArg);
	bool bOverwrite = false;
	Args->TryGetBoolField(TEXT("overwrite"), bOverwrite);
	bool bSave = true;
	Args->TryGetBoolField(TEXT("save"), bSave);
	bool bLooping = false;
	Args->TryGetBoolField(TEXT("looping"), bLooping);

	if (FPaths::IsRelative(Source))
	{
		OutError = TEXT("'source' must be an absolute path");
		return nullptr;
	}
	if (!IFileManager::Get().FileExists(*Source))
	{
		OutError = FString::Printf(TEXT("source file not found: '%s'"), *Source);
		return nullptr;
	}
	if (!FPaths::GetExtension(Source).Equals(TEXT("wav"), ESearchCase::IgnoreCase))
	{
		OutError = TEXT("unsupported sound extension (only 16-bit mono/stereo .wav is allowed)");
		return nullptr;
	}
	if (!ValidContentDestination(Destination, OutError))
	{
		return nullptr;
	}

	FString AssetName = SanitizeMeshAssetName(NameArg.IsEmpty() ? FPaths::GetBaseFilename(Source) : NameArg);
	if (AssetName.IsEmpty())
	{
		OutError = TEXT("asset name is empty after sanitization");
		return nullptr;
	}
	FString DestFolder = Destination;
	while (DestFolder.EndsWith(TEXT("/")))
	{
		DestFolder = DestFolder.LeftChop(1);
	}
	const FString PackagePath = DestFolder + TEXT("/") + AssetName;
	const FString ObjectPath = PackagePath + TEXT(".") + AssetName;
	if (!FPackageName::IsValidLongPackageName(PackagePath))
	{
		OutError = FString::Printf(TEXT("invalid target package '%s'"), *PackagePath);
		return nullptr;
	}

	const bool bExists = (LoadObject<UObject>(nullptr, *ObjectPath) != nullptr)
		|| FPackageName::DoesPackageExist(PackagePath);
	if (bExists && !bOverwrite)
	{
		OutError = FString::Printf(TEXT("asset '%s' already exists (pass overwrite=true)"), *PackagePath);
		return nullptr;
	}

	// Stage under the requested asset name because automated imports derive the
	// object name from the source filename. USoundFactory is configured to avoid
	// cue creation and every overwrite prompt; cues are built by the dedicated
	// create_sound_cue verb instead.
	const FString TempDir = FPaths::Combine(*FPaths::GameIntermediateDir(), TEXT("MapForgeImport"), TEXT("Audio"));
	IFileManager::Get().MakeDirectory(*TempDir, true);
	const FString TempFile = FPaths::Combine(*TempDir, *(AssetName + TEXT(".wav")));
	if (IFileManager::Get().Copy(*TempFile, *Source, true, true) != COPY_OK)
	{
		OutError = FString::Printf(TEXT("failed to stage source to '%s'"), *TempFile);
		return nullptr;
	}

	USoundFactory* Factory = NewObject<USoundFactory>();
	Factory->AddToRoot();
	Factory->bAutoCreateCue = false;
	if (bOverwrite)
	{
		USoundFactory::SuppressImportOverwriteDialog();
	}

	UAutomatedAssetImportData* Data = NewObject<UAutomatedAssetImportData>();
	Data->AddToRoot();
	Data->Filenames.Add(TempFile);
	Data->DestinationPath = DestFolder;
	Data->Factory = Factory;
	Data->bReplaceExisting = bOverwrite;

	const FScopedTransaction Transaction(FText::FromString(TEXT("MapForge Import Sound")));
	FAssetToolsModule& AssetToolsModule = FModuleManager::Get().LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	TArray<UObject*> ImportedObjects = AssetToolsModule.Get().ImportAssetsAutomated(*Data);

	Factory->RemoveFromRoot();
	Data->RemoveFromRoot();
	IFileManager::Get().Delete(*TempFile, false, true, true);

	USoundWave* Wave = nullptr;
	for (UObject* Obj : ImportedObjects)
	{
		Wave = Cast<USoundWave>(Obj);
		if (Wave != nullptr)
		{
			break;
		}
	}
	if (Wave == nullptr && bOverwrite)
	{
		Wave = LoadObject<USoundWave>(nullptr, *ObjectPath);
	}
	if (Wave == nullptr)
	{
		OutError = TEXT("import produced no SoundWave (the WAV may not be 16-bit mono/stereo PCM)");
		return nullptr;
	}

	Wave->Modify();
	Wave->bLooping = bLooping;
	if (Wave->AssetImportData != nullptr)
	{
		Wave->AssetImportData->Update(Source);
	}
	Wave->PostEditChange();
	Wave->MarkPackageDirty();
	const bool bSaved = bSave ? SaveAssetPackage(Wave) : false;

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetStringField(TEXT("asset"), Wave->GetPathName());
	Result->SetStringField(TEXT("package"), Wave->GetOutermost()->GetName());
	Result->SetStringField(TEXT("class"), Wave->GetClass()->GetName());
	Result->SetStringField(TEXT("source"), Source);
	Result->SetStringField(TEXT("destination"), DestFolder);
	Result->SetBoolField(TEXT("created"), !bExists);
	Result->SetBoolField(TEXT("overwritten"), bExists);
	Result->SetBoolField(TEXT("looping"), Wave->bLooping != 0);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetNumberField(TEXT("duration"), Wave->Duration);
	Result->SetNumberField(TEXT("sample_rate"), Wave->SampleRate);
	Result->SetNumberField(TEXT("channels"), Wave->NumChannels);
	return Result;
}

TSharedPtr<FJsonObject> FMapForgeBridgeServer::CmdCreateSoundCue(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	FString Destination, NameArg, Mode;
	if (!Args->TryGetStringField(TEXT("destination"), Destination) || Destination.IsEmpty())
	{
		OutError = TEXT("missing 'destination'");
		return nullptr;
	}
	if (!Args->TryGetStringField(TEXT("name"), NameArg) || NameArg.IsEmpty())
	{
		OutError = TEXT("missing 'name'");
		return nullptr;
	}
	Args->TryGetStringField(TEXT("mode"), Mode);
	Mode = Mode.IsEmpty() ? TEXT("single") : Mode.ToLower();
	if (Mode != TEXT("single") && Mode != TEXT("random") && Mode != TEXT("mixer"))
	{
		OutError = FString::Printf(TEXT("unsupported cue mode '%s' (use single, random, or mixer)"), *Mode);
		return nullptr;
	}
	if (!ValidContentDestination(Destination, OutError))
	{
		return nullptr;
	}

	const TArray<TSharedPtr<FJsonValue>>* WaveValues = nullptr;
	if (!Args->TryGetArrayField(TEXT("waves"), WaveValues) || WaveValues == nullptr || WaveValues->Num() == 0)
	{
		OutError = TEXT("'waves' must contain at least one SoundWave asset path");
		return nullptr;
	}
	if (Mode == TEXT("single") && WaveValues->Num() != 1)
	{
		OutError = TEXT("single cue mode requires exactly one wave");
		return nullptr;
	}
	// USoundNode::MAX_ALLOWED_CHILD_NODES is private in this 4.15 fork.
	// Random and Mixer nodes both expose the same hard limit of 32 inputs.
	const int32 MaxCueInputs = 32;
	if (WaveValues->Num() > MaxCueInputs)
	{
		OutError = FString::Printf(TEXT("cue has %d waves; maximum is %d"), WaveValues->Num(), MaxCueInputs);
		return nullptr;
	}

	TArray<USoundWave*> Waves;
	TArray<TSharedPtr<FJsonValue>> ResolvedWavePaths;
	for (const TSharedPtr<FJsonValue>& Value : *WaveValues)
	{
		FString WavePath = Value->AsString();
		if (!WavePath.Contains(TEXT(".")))
		{
			WavePath += TEXT(".") + FPackageName::GetShortName(WavePath);
		}
		USoundWave* Wave = LoadObject<USoundWave>(nullptr, *WavePath);
		if (Wave == nullptr)
		{
			OutError = FString::Printf(TEXT("could not load SoundWave '%s'"), *WavePath);
			return nullptr;
		}
		Waves.Add(Wave);
		ResolvedWavePaths.Add(MakeShareable(new FJsonValueString(Wave->GetPathName())));
	}

	bool bOverwrite = false;
	Args->TryGetBoolField(TEXT("overwrite"), bOverwrite);
	bool bSave = true;
	Args->TryGetBoolField(TEXT("save"), bSave);
	bool bLooping = false;
	Args->TryGetBoolField(TEXT("looping"), bLooping);
	bool bWithoutReplacement = true;
	Args->TryGetBoolField(TEXT("random_without_replacement"), bWithoutReplacement);
	double CueVolume = 0.75;
	Args->TryGetNumberField(TEXT("volume"), CueVolume);
	double CuePitch = 1.0;
	Args->TryGetNumberField(TEXT("pitch"), CuePitch);
	const TArray<TSharedPtr<FJsonValue>>* MixerVolumeValues = nullptr;
	if (Mode == TEXT("mixer")
		&& Args->TryGetArrayField(TEXT("mixer_volumes"), MixerVolumeValues)
		&& MixerVolumeValues != nullptr
		&& MixerVolumeValues->Num() != Waves.Num())
	{
		OutError = TEXT("mixer_volumes must contain one value per wave");
		return nullptr;
	}

	FString AssetName = SanitizeMeshAssetName(NameArg);
	if (AssetName.IsEmpty())
	{
		OutError = TEXT("cue name is empty after sanitization");
		return nullptr;
	}
	FString DestFolder = Destination;
	while (DestFolder.EndsWith(TEXT("/")))
	{
		DestFolder = DestFolder.LeftChop(1);
	}
	const FString PackagePath = DestFolder + TEXT("/") + AssetName;
	const FString ObjectPath = PackagePath + TEXT(".") + AssetName;
	if (!FPackageName::IsValidLongPackageName(PackagePath))
	{
		OutError = FString::Printf(TEXT("invalid target package '%s'"), *PackagePath);
		return nullptr;
	}

	UObject* ExistingObject = LoadObject<UObject>(nullptr, *ObjectPath);
	const bool bExists = ExistingObject != nullptr || FPackageName::DoesPackageExist(PackagePath);
	if (bExists && !bOverwrite)
	{
		OutError = FString::Printf(TEXT("asset '%s' already exists (pass overwrite=true)"), *PackagePath);
		return nullptr;
	}
	USoundCue* Cue = Cast<USoundCue>(ExistingObject);
	if (ExistingObject != nullptr && Cue == nullptr)
	{
		OutError = FString::Printf(TEXT("target '%s' exists but is not a SoundCue"), *ObjectPath);
		return nullptr;
	}

	const FScopedTransaction Transaction(FText::FromString(TEXT("MapForge Create Sound Cue")));
	UPackage* Package = CreatePackage(nullptr, *PackagePath);
	if (Package == nullptr)
	{
		OutError = FString::Printf(TEXT("could not create package '%s'"), *PackagePath);
		return nullptr;
	}
	if (Cue == nullptr)
	{
		Cue = NewObject<USoundCue>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Cue);
	}
	else
	{
		Cue->Modify();
		Cue->ClearGraph();
		Cue->AllNodes.Empty();
		Cue->FirstNode = nullptr;
	}

	Cue->VolumeMultiplier = FMath::Max(0.0f, (float)CueVolume);
	Cue->PitchMultiplier = FMath::Clamp((float)CuePitch, 0.4f, 2.0f);

	USoundNode* ContentNode = nullptr;
	USoundNodeRandom* RandomNode = nullptr;
	USoundNodeMixer* MixerNode = nullptr;
	if (Mode == TEXT("random"))
	{
		RandomNode = Cue->ConstructSoundNode<USoundNodeRandom>();
		RandomNode->bRandomizeWithoutReplacement = bWithoutReplacement;
		RandomNode->CreateStartingConnectors();
		ContentNode = RandomNode;
	}
	else if (Mode == TEXT("mixer"))
	{
		MixerNode = Cue->ConstructSoundNode<USoundNodeMixer>();
		MixerNode->CreateStartingConnectors();
		ContentNode = MixerNode;
	}

	if (ContentNode != nullptr)
	{
		while (ContentNode->ChildNodes.Num() < Waves.Num())
		{
			ContentNode->InsertChildNode(ContentNode->ChildNodes.Num());
		}
		while (ContentNode->ChildNodes.Num() > Waves.Num())
		{
			ContentNode->RemoveChildNode(ContentNode->ChildNodes.Num() - 1);
		}
		ContentNode->PlaceNode(1, 0, 1);
	}

	TArray<USoundNodeWavePlayer*> Players;
	for (int32 Index = 0; Index < Waves.Num(); ++Index)
	{
		USoundNodeWavePlayer* Player = Cue->ConstructSoundNode<USoundNodeWavePlayer>();
		Player->SetSoundWave(Waves[Index]);
		Player->bLooping = bLooping;
		Player->PlaceNode(ContentNode != nullptr ? 2 : 1, Index, Waves.Num());
		Players.Add(Player);
		if (ContentNode != nullptr)
		{
			ContentNode->ChildNodes[Index] = Player;
		}
	}
	if (ContentNode == nullptr)
	{
		ContentNode = Players[0];
	}

	if (MixerNode != nullptr)
	{
		if (MixerVolumeValues != nullptr)
		{
			for (int32 Index = 0; Index < MixerVolumeValues->Num(); ++Index)
			{
				MixerNode->InputVolume[Index] = FMath::Max(0.0f, (float)(*MixerVolumeValues)[Index]->AsNumber());
			}
		}
	}

	const TSharedPtr<FJsonObject>* ModulatorPtr = nullptr;
	if (Args->TryGetObjectField(TEXT("modulator"), ModulatorPtr) && ModulatorPtr != nullptr && ModulatorPtr->IsValid())
	{
		TSharedPtr<FJsonObject> ModulatorArgs = *ModulatorPtr;
		auto ModNumber = [&ModulatorArgs](const TCHAR* Key, float DefaultValue)
		{
			double Value = DefaultValue;
			ModulatorArgs->TryGetNumberField(Key, Value);
			return (float)Value;
		};
		USoundNodeModulator* Modulator = Cue->ConstructSoundNode<USoundNodeModulator>();
		Modulator->PitchMin = FMath::Clamp(ModNumber(TEXT("pitch_min"), 0.95f), 0.4f, 2.0f);
		Modulator->PitchMax = FMath::Clamp(ModNumber(TEXT("pitch_max"), 1.05f), Modulator->PitchMin, 2.0f);
		Modulator->VolumeMin = FMath::Max(0.0f, ModNumber(TEXT("volume_min"), 0.9f));
		Modulator->VolumeMax = FMath::Max(Modulator->VolumeMin, ModNumber(TEXT("volume_max"), 1.0f));
		Modulator->CreateStartingConnectors();
		Modulator->ChildNodes[0] = ContentNode;
		Modulator->PlaceNode(0, 0, 1);
		Cue->FirstNode = Modulator;
	}
	else
	{
		Cue->FirstNode = ContentNode;
	}

	Cue->LinkGraphNodesFromSoundNodes();
	Cue->PostEditChange();
	Cue->MarkPackageDirty();
	const bool bSaved = bSave ? SaveAssetPackage(Cue) : false;

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetStringField(TEXT("asset"), Cue->GetPathName());
	Result->SetStringField(TEXT("package"), Cue->GetOutermost()->GetName());
	Result->SetStringField(TEXT("class"), Cue->GetClass()->GetName());
	Result->SetStringField(TEXT("destination"), DestFolder);
	Result->SetStringField(TEXT("mode"), Mode);
	Result->SetArrayField(TEXT("waves"), ResolvedWavePaths);
	Result->SetNumberField(TEXT("wave_count"), Waves.Num());
	Result->SetNumberField(TEXT("node_count"), Cue->AllNodes.Num());
	Result->SetBoolField(TEXT("looping"), bLooping);
	Result->SetBoolField(TEXT("created"), !bExists);
	Result->SetBoolField(TEXT("overwritten"), bExists);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return Result;
}

TSharedPtr<FJsonObject> FMapForgeBridgeServer::CmdConfigureStaticMesh(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	FString AssetArg;
	if (!Args->TryGetStringField(TEXT("asset"), AssetArg) || AssetArg.IsEmpty())
	{
		OutError = TEXT("missing 'asset'");
		return nullptr;
	}
	FString ObjectPath = AssetArg;
	if (!ObjectPath.Contains(TEXT(".")))
	{
		ObjectPath = AssetArg + TEXT(".") + FPackageName::GetShortName(AssetArg);
	}
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *ObjectPath);
	if (Mesh == nullptr)
	{
		OutError = FString::Printf(TEXT("could not load static mesh '%s'"), *AssetArg);
		return nullptr;
	}

	bool bSave = false;
	Args->TryGetBoolField(TEXT("save"), bSave);
	bool bClearSimple = false;
	Args->TryGetBoolField(TEXT("clear_simple_collision"), bClearSimple);
	FString CollisionMode;
	Args->TryGetStringField(TEXT("collision"), CollisionMode);

	const FScopedTransaction Transaction(FText::FromString(TEXT("MapForge Configure Static Mesh")));
	Mesh->Modify();

	TArray<FString> Warnings;
	if (!CollisionMode.IsEmpty() || bClearSimple)
	{
		ApplyCollision(Mesh, CollisionMode, bClearSimple, Warnings);
	}

	double LightmapIdxD = 0.0;
	if (Args->TryGetNumberField(TEXT("lightmap_coordinate_index"), LightmapIdxD))
	{
		ApplyLightmapAndRebuild(Mesh, (int32)LightmapIdxD, true);
	}

	// materials: { "0": "/Game/.../M_Foo", "2": "..." } -> explicit slot indices
	int32 MaterialsAssigned = 0;
	const TSharedPtr<FJsonObject>* MatsPtr = nullptr;
	if (Args->TryGetObjectField(TEXT("materials"), MatsPtr) && MatsPtr != nullptr && MatsPtr->IsValid())
	{
		for (const auto& Pair : (*MatsPtr)->Values)
		{
			const int32 SlotIdx = FCString::Atoi(*Pair.Key);
			FString MatPath;
			if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(MatPath) || MatPath.IsEmpty())
			{
				continue;
			}
			if (SlotIdx < 0 || SlotIdx >= Mesh->StaticMaterials.Num())
			{
				Warnings.Add(FString::Printf(TEXT("material slot %d out of range (0..%d)"), SlotIdx, Mesh->StaticMaterials.Num() - 1));
				continue;
			}
			UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MatPath);
			if (Mat == nullptr)
			{
				Warnings.Add(FString::Printf(TEXT("could not load material '%s' for slot %d"), *MatPath, SlotIdx));
				continue;
			}
			Mesh->StaticMaterials[SlotIdx].MaterialInterface = Mat;
			++MaterialsAssigned;
		}
		if (MaterialsAssigned > 0)
		{
			Mesh->PostEditChange();
		}
	}

	Mesh->MarkPackageDirty();
	const bool bSaved = bSave ? SaveAssetPackage(Mesh) : false;

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetStringField(TEXT("asset"), Mesh->GetPathName());
	Result->SetNumberField(TEXT("materials_assigned"), MaterialsAssigned);
	Result->SetBoolField(TEXT("saved"), bSaved);
	FillMeshStats(Mesh, Result);
	TArray<TSharedPtr<FJsonValue>> WarnArr;
	for (const FString& W : Warnings)
	{
		WarnArr.Add(MakeShareable(new FJsonValueString(W)));
	}
	Result->SetArrayField(TEXT("warnings"), WarnArr);
	return Result;
}

TSharedPtr<FJsonObject> FMapForgeBridgeServer::CmdPlaceStaticMesh(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	FString AssetArg;
	if (!Args->TryGetStringField(TEXT("asset"), AssetArg) || AssetArg.IsEmpty())
	{
		OutError = TEXT("missing 'asset'");
		return nullptr;
	}
	FString ObjectPath = AssetArg;
	if (!ObjectPath.Contains(TEXT(".")))
	{
		ObjectPath = AssetArg + TEXT(".") + FPackageName::GetShortName(AssetArg);
	}
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *ObjectPath);
	if (Mesh == nullptr)
	{
		OutError = FString::Printf(TEXT("could not load static mesh '%s'"), *AssetArg);
		return nullptr;
	}

	ULevel* Level = World->GetCurrentLevel();
	if (Level->bLocked)
	{
		OutError = TEXT("current level is locked");
		return nullptr;
	}

	const FTransform Xform = ParseTransform(Args);
	FString Label, Folder;
	Args->TryGetStringField(TEXT("label"), Label);
	Args->TryGetStringField(TEXT("folder"), Folder);

	const FScopedTransaction Transaction(FText::FromString(TEXT("MapForge Place Static Mesh")));

	FActorSpawnParameters SpawnParams;
	SpawnParams.OverrideLevel = Level;
	AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Xform.GetLocation(), Xform.Rotator(), SpawnParams);
	if (Actor == nullptr)
	{
		OutError = TEXT("failed to spawn StaticMeshActor");
		return nullptr;
	}

	Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
	Actor->SetActorTransform(Xform); // applies scale too
	if (!Label.IsEmpty())
	{
		Actor->SetActorLabel(Label);
	}
	if (!Folder.IsEmpty())
	{
		Actor->SetFolderPath(FName(*Folder));
	}

	Actor->MarkPackageDirty();
	Level->MarkPackageDirty();
	GEditor->RedrawLevelEditingViewports();

	// The bridge deliberately does NOT save the level here.
	const FTransform Final = Actor->GetActorTransform();
	const FVector FinalLoc = Final.GetLocation();
	const FRotator FinalRot = Final.Rotator();
	const FVector FinalScale = Final.GetScale3D();

	TSharedRef<FJsonObject> Location = MakeShareable(new FJsonObject());
	Location->SetNumberField(TEXT("x"), FinalLoc.X);
	Location->SetNumberField(TEXT("y"), FinalLoc.Y);
	Location->SetNumberField(TEXT("z"), FinalLoc.Z);
	TSharedRef<FJsonObject> Rotation = MakeShareable(new FJsonObject());
	Rotation->SetNumberField(TEXT("pitch"), FinalRot.Pitch);
	Rotation->SetNumberField(TEXT("yaw"), FinalRot.Yaw);
	Rotation->SetNumberField(TEXT("roll"), FinalRot.Roll);
	TSharedRef<FJsonObject> ScaleObj = MakeShareable(new FJsonObject());
	ScaleObj->SetNumberField(TEXT("x"), FinalScale.X);
	ScaleObj->SetNumberField(TEXT("y"), FinalScale.Y);
	ScaleObj->SetNumberField(TEXT("z"), FinalScale.Z);
	TSharedRef<FJsonObject> TransformObj = MakeShareable(new FJsonObject());
	TransformObj->SetObjectField(TEXT("location"), Location);
	TransformObj->SetObjectField(TEXT("rotation"), Rotation);
	TransformObj->SetObjectField(TEXT("scale"), ScaleObj);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetStringField(TEXT("actor"), Actor->GetName());
	Result->SetStringField(TEXT("label"), Actor->GetActorLabel());
	Result->SetStringField(TEXT("path"), Actor->GetPathName());
	Result->SetStringField(TEXT("mesh"), Mesh->GetPathName());
	Result->SetObjectField(TEXT("transform"), TransformObj);
	return Result;
}

TSharedPtr<FJsonObject> FMapForgeBridgeServer::CmdRecoveryLayout(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	FString Slug, Root;
	if (!ResolveMapSlug(World, Args, Slug, OutError)) { return nullptr; }
	if (!ResolveRecoveryRoot(Args, Root, OutError)) { return nullptr; }
	const FString ContentRoot = Root + TEXT("/") + Slug;
	if (!FPackageName::IsValidLongPackageName(ContentRoot))
	{
		OutError = FString::Printf(TEXT("resolved content root is not a valid package path: '%s'"), *ContentRoot);
		return nullptr;
	}
	return MakeRecoveryLayout(Slug, Root);
}

TSharedPtr<FJsonObject> FMapForgeBridgeServer::CmdInspectStaticMeshActors(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	ULevel* Level = World->GetCurrentLevel();

	FString NameFilter, FolderFilter;
	Args->TryGetStringField(TEXT("name_contains"), NameFilter);
	Args->TryGetStringField(TEXT("folder_contains"), FolderFilter);
	int32 Offset = 0;
	int32 Limit = 1000;
	double NumTmp = 0.0;
	if (Args->TryGetNumberField(TEXT("offset"), NumTmp)) { Offset = FMath::Max(0, (int32)NumTmp); }
	if (Args->TryGetNumberField(TEXT("limit"), NumTmp)) { Limit = FMath::Max(0, (int32)NumTmp); }

	// Summaries span every matching actor, independent of the pagination window.
	int32 Total = 0, NullMeshCount = 0, HiddenCount = 0, UnresolvedCount = 0;
	TSet<FString> UniqueMeshes;

	TArray<TSharedPtr<FJsonValue>> Items;
	int32 MatchIndex = 0;

	// Read-only pass: no Modify()/spawn/property edits, so the level never dirties.
	for (AActor* Actor : Level->Actors)
	{
		AStaticMeshActor* SMA = Cast<AStaticMeshActor>(Actor);
		if (SMA == nullptr || SMA->IsPendingKill())
		{
			continue;
		}
		const FString ActorName = SMA->GetName();
		const FString ActorLabel = SMA->GetActorLabel();
		const FString FolderPath = SMA->GetFolderPath().ToString();
		if (!NameFilter.IsEmpty() && !ActorName.Contains(NameFilter) && !ActorLabel.Contains(NameFilter))
		{
			continue;
		}
		if (!FolderFilter.IsEmpty() && !FolderPath.Contains(FolderFilter))
		{
			continue;
		}

		UStaticMeshComponent* SMC = SMA->GetStaticMeshComponent();
		UStaticMesh* Mesh = (SMC != nullptr) ? SMC->GetStaticMesh() : nullptr;
		const bool bHidden = SMA->bHidden || SMA->IsHiddenEd();
		const int32 NumMats = (SMC != nullptr) ? SMC->GetNumMaterials() : 0;
		bool bUnresolved = false;
		for (int32 i = 0; i < NumMats; ++i)
		{
			if (SMC->GetMaterial(i) == nullptr) { bUnresolved = true; break; }
		}

		++Total;
		if (Mesh == nullptr) { ++NullMeshCount; } else { UniqueMeshes.Add(Mesh->GetPathName()); }
		if (bHidden) { ++HiddenCount; }
		if (bUnresolved) { ++UnresolvedCount; }

		const int32 ThisIndex = MatchIndex++;
		if (ThisIndex < Offset) { continue; }
		if (Limit > 0 && Items.Num() >= Limit) { continue; }

		TSharedRef<FJsonObject> Item = MakeShareable(new FJsonObject());
		Item->SetStringField(TEXT("name"), ActorName);
		Item->SetStringField(TEXT("label"), ActorLabel);
		Item->SetStringField(TEXT("path"), SMA->GetPathName());
		Item->SetStringField(TEXT("folder"), FolderPath);
		if (Mesh != nullptr) { Item->SetStringField(TEXT("mesh"), Mesh->GetPathName()); }
		else { Item->SetField(TEXT("mesh"), MakeShareable(new FJsonValueNull())); }
		Item->SetObjectField(TEXT("transform"), MakeTransformJson(SMA->GetActorTransform()));
		Item->SetBoolField(TEXT("hidden"), SMA->bHidden);
		Item->SetBoolField(TEXT("hidden_ed"), SMA->IsHiddenEd());
		Item->SetBoolField(TEXT("has_null_mesh"), Mesh == nullptr);
		Item->SetBoolField(TEXT("has_unresolved_materials"), bUnresolved);
		if (SMC != nullptr)
		{
			Item->SetObjectField(TEXT("component_transform"), MakeTransformJson(SMC->GetRelativeTransform()));
			Item->SetStringField(TEXT("mobility"), MobilityToString(SMC->Mobility));
			Item->SetStringField(TEXT("collision"), CollisionEnabledToString(SMC->GetCollisionEnabled()));
			Item->SetBoolField(TEXT("visible"), SMC->IsVisible());

			TArray<TSharedPtr<FJsonValue>> Slots;
			for (int32 i = 0; i < NumMats; ++i)
			{
				UMaterialInterface* Mat = SMC->GetMaterial(i);
				const bool bIsOverride = (i < SMC->OverrideMaterials.Num()) && (SMC->OverrideMaterials[i] != nullptr);
				TSharedRef<FJsonObject> Slot = MakeShareable(new FJsonObject());
				Slot->SetNumberField(TEXT("slot"), i);
				if (Mat != nullptr) { Slot->SetStringField(TEXT("material"), Mat->GetPathName()); }
				else { Slot->SetField(TEXT("material"), MakeShareable(new FJsonValueNull())); }
				Slot->SetBoolField(TEXT("is_override"), bIsOverride);
				Slots.Add(MakeShareable(new FJsonValueObject(Slot)));
			}
			Item->SetArrayField(TEXT("material_overrides"), Slots);
		}
		Items.Add(MakeShareable(new FJsonValueObject(Item)));
	}

	TSharedRef<FJsonObject> Summary = MakeShareable(new FJsonObject());
	Summary->SetNumberField(TEXT("total_static_mesh_actors"), Total);
	Summary->SetNumberField(TEXT("actors_null_mesh"), NullMeshCount);
	Summary->SetNumberField(TEXT("unique_mesh_paths"), UniqueMeshes.Num());
	Summary->SetNumberField(TEXT("actors_hidden"), HiddenCount);
	Summary->SetNumberField(TEXT("actors_unresolved_materials"), UnresolvedCount);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetArrayField(TEXT("actors"), Items);
	Result->SetNumberField(TEXT("returned"), Items.Num());
	Result->SetNumberField(TEXT("offset"), Offset);
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetObjectField(TEXT("summary"), Summary);
	return Result;
}
