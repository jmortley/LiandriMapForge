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
			|| Cmd == TEXT("compile_blueprint");
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
