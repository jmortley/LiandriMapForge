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
#include "GameFramework/WorldSettings.h"
#include "Model.h"
#include "Materials/MaterialInterface.h"
#include "Misc/EngineVersion.h"
#include "Misc/CommandLine.h"

#include "Networking.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

#include "Json.h"

DEFINE_LOG_CATEGORY_STATIC(LogMapForgeBridge, Log, All);

namespace
{
	const uint16 DefaultPort = 8765;
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
		PumpClients();
	}
	return true;
}

bool FMapForgeBridgeServer::StartListening()
{
	// Localhost only, by design: the bridge is a local authoring tool and must
	// never be reachable from the network.
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

void FMapForgeBridgeServer::PumpClients()
{
	for (int32 ClientIdx = Clients.Num() - 1; ClientIdx >= 0; --ClientIdx)
	{
		FMapForgeClient& Client = Clients[ClientIdx];

		// Drain everything the socket has for us.
		uint32 PendingSize = 0;
		while (Client.Socket->HasPendingData(PendingSize) && PendingSize > 0)
		{
			const int32 Offset = Client.RecvBuf.Num();
			Client.RecvBuf.AddUninitialized((int32)PendingSize);
			int32 BytesRead = 0;
			if (Client.Socket->Recv(Client.RecvBuf.GetData() + Offset, (int32)PendingSize, BytesRead) && BytesRead > 0)
			{
				Client.RecvBuf.SetNum(Offset + BytesRead, false);
			}
			else
			{
				Client.RecvBuf.SetNum(Offset, false);
				break;
			}
		}

		// Handle every complete line in the buffer.
		int32 NewlineIdx;
		while ((NewlineIdx = Client.RecvBuf.IndexOfByKey((uint8)'\n')) != INDEX_NONE)
		{
			int32 LineLen = NewlineIdx;
			while (LineLen > 0 && Client.RecvBuf[LineLen - 1] == (uint8)'\r')
			{
				--LineLen;
			}

			FString Line;
			if (LineLen > 0)
			{
				FUTF8ToTCHAR Converter((const ANSICHAR*)Client.RecvBuf.GetData(), LineLen);
				Line = FString(Converter.Length(), Converter.Get());
			}
			Client.RecvBuf.RemoveAt(0, NewlineIdx + 1, false);

			if (!Line.IsEmpty())
			{
				SendLine(Client, HandleRequest(Line));
			}
		}

		// A successful zero-byte peek after draining means the peer closed.
		uint8 Dummy = 0;
		int32 PeekRead = 0;
		if (Client.Socket->Recv(&Dummy, 1, PeekRead, ESocketReceiveFlags::Peek) && PeekRead == 0)
		{
			CloseClient(Client);
			Clients.RemoveAt(ClientIdx);
		}
	}
}

void FMapForgeBridgeServer::SendLine(FMapForgeClient& Client, const FString& Line)
{
	const FString Payload = Line + TEXT("\n");
	FTCHARToUTF8 Converter(*Payload);
	const uint8* Data = (const uint8*)Converter.Get();
	const int32 Total = Converter.Length();

	// Non-blocking socket: on a full send buffer, wait for the (localhost)
	// client to drain. Bounded so a wedged client can't hang the editor.
	int32 Sent = 0;
	int32 Stalls = 0;
	while (Sent < Total)
	{
		int32 ThisSent = 0;
		if (Client.Socket->Send(Data + Sent, Total - Sent, ThisSent) && ThisSent > 0)
		{
			Sent += ThisSent;
			Stalls = 0;
		}
		else if (++Stalls > 10000)
		{
			UE_LOG(LogMapForgeBridge, Warning, TEXT("Send stalled; dropping %d unsent bytes"), Total - Sent);
			break;
		}
		else
		{
			FPlatformProcess::Sleep(0.001f);
		}
	}
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

	OutError = FString::Printf(TEXT("unknown cmd '%s'"), *Cmd);
	return nullptr;
}

/* Verbs
 *****************************************************************************/

TSharedPtr<FJsonObject> FMapForgeBridgeServer::CmdStatus(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	ULevel* Level = World->GetCurrentLevel();
	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetStringField(TEXT("engine"), FEngineVersion::Current().ToString());
	Result->SetStringField(TEXT("map"), Level->GetOutermost()->GetName());
	Result->SetBoolField(TEXT("map_dirty"), Level->GetOutermost()->IsDirty());
	Result->SetNumberField(TEXT("actors"), Level->Actors.Num());
	Result->SetBoolField(TEXT("building"), FEditorBuildUtils::IsBuildCurrentlyRunning());
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

	TArray<TSharedPtr<FJsonValue>> Failed;
	int32 LoadedCount = 0;
	for (const TSharedPtr<FJsonValue>& Value : *Paths)
	{
		FString Path;
		if (!Value.IsValid() || !Value->TryGetString(Path) || Path.IsEmpty())
		{
			continue;
		}
		// Loading the object pins it in memory, so the subsequent T3D import
		// can resolve it by path. This is what retires the paste workflow's
		// hidden material-preloader meshes.
		UObject* Loaded = StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
		if (Loaded != nullptr)
		{
			++LoadedCount;
		}
		else
		{
			Failed.Add(MakeShareable(new FJsonValueString(Path)));
		}
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetNumberField(TEXT("loaded"), LoadedCount);
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

	// ULevelFactory silently imports nothing unless the buffer starts with
	// "Begin Map" (EditorFactories.cpp:478), so wrap bare actor lists.
	FString Probe = T3D.Left(200);
	Probe.Trim();
	if (!Probe.StartsWith(TEXT("Begin Map")))
	{
		T3D = FString(TEXT("Begin Map\n   Begin Level\n")) + T3D + TEXT("\n   End Level\nEnd Map\n");
	}

	// edactPasteSelected pastes *components* instead of actors if any are
	// selected, so clear both selection sets. It selects what it creates,
	// which is how we enumerate the new actors afterwards.
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
		Imported.Add(MakeShareable(new FJsonValueObject(Entry)));
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

	if (!bSelectedOnly)
	{
		GEditor->GetSelectedComponents()->DeselectAll();
		GEditor->SelectNone(false, true, false);
		for (FActorIterator It(World); It; ++It)
		{
			GEditor->SelectActor(*It, true, false, true);
		}
	}

	// Note: edactCopySelected also places the T3D on the OS clipboard
	// (engine behavior); it deselects builder brushes and world settings
	// itself before exporting.
	FString ExportedT3D;
	GUnrealEd->edactCopySelected(World, &ExportedT3D);

	if (!bSelectedOnly)
	{
		GEditor->SelectNone(true, true, false);
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetStringField(TEXT("t3d"), ExportedT3D);
	Result->SetNumberField(TEXT("length"), ExportedT3D.Len());
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

	int32 Matched = 0;
	TArray<TSharedPtr<FJsonValue>> Actors;
	for (FActorIterator It(World); It; ++It)
	{
		AActor* Actor = *It;
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

	TArray<FString> Names;
	for (const TSharedPtr<FJsonValue>& Value : *NamesArray)
	{
		FString Name;
		if (Value.IsValid() && Value->TryGetString(Name) && !Name.IsEmpty())
		{
			Names.Add(Name);
		}
	}

	GEditor->GetSelectedComponents()->DeselectAll();
	GEditor->SelectNone(false, true, false);

	TArray<TSharedPtr<FJsonValue>> MatchedNames;
	for (FActorIterator It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (FActorEditorUtils::IsABuilderBrush(Actor) || Actor->IsA(AWorldSettings::StaticClass()))
		{
			continue;
		}
		for (const FString& Name : Names)
		{
			if (Actor->GetName().Equals(Name, ESearchCase::IgnoreCase)
				|| Actor->GetActorLabel().Equals(Name, ESearchCase::IgnoreCase))
			{
				GEditor->SelectActor(Actor, true, false, true);
				MatchedNames.Add(MakeShareable(new FJsonValueString(Actor->GetName())));
				break;
			}
		}
	}

	bool bDeleted = false;
	if (MatchedNames.Num() > 0)
	{
		bDeleted = GUnrealEd->edactDeleteSelected(World, false, false);
	}

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetNumberField(TEXT("matched"), MatchedNames.Num());
	Result->SetArrayField(TEXT("matched_names"), MatchedNames);
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
	GEditor->Exec(World, TEXT("MAP REBUILD ALLVISIBLE"), Output);
	GEditor->RedrawLevelEditingViewports();

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

	const bool bStarted = FEditorBuildUtils::EditorBuild(World, BuildId, /*bAllowLightingDialog*/ false);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("started"), bStarted);
	// Lighting/nav complete asynchronously; poll 'status' until building=false.
	Result->SetBoolField(TEXT("building"), FEditorBuildUtils::IsBuildCurrentlyRunning());
	return Result;
}

TSharedPtr<FJsonObject> FMapForgeBridgeServer::CmdSaveLevel(UWorld* World, const TSharedRef<FJsonObject>& Args, FString& OutError)
{
	FString Filename;
	Args->TryGetStringField(TEXT("filename"), Filename);

	// With no filename, a never-saved level pops a modal Save-As dialog in the
	// editor — pass 'filename' to keep the round-trip headless.
	FString SavedFilename;
	const bool bSaved = FEditorFileUtils::SaveLevel(World->GetCurrentLevel(), Filename, &SavedFilename);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("saved"), bSaved);
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

	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	if (Material == nullptr)
	{
		OutError = FString::Printf(TEXT("could not load material '%s'"), *MaterialPath);
		return nullptr;
	}

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
	// material + selected surfaces) — hostile to external driving. Set the
	// surface directly and let polyUpdateMaster sync the brush's master poly
	// so the change survives a CSG rebuild (same as EditorServer.cpp:4379).
	UModel* Model = World->GetCurrentLevel()->Model;
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

		Model->ModifySurf(SurfIdx, false);
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
