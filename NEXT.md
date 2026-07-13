# LiandriMapForge — continuation prompt

Paste this to continue in a fresh session.

---

You're continuing **LiandriMapForge**, an AI map-generation tool for **UnrealTournament
(UE4 4.15 fork)**, living in its own git repo at `Plugins/LiandriMapForge`
(github.com/jmortley/LiandriMapForge), built by **phantaci**. Standalone
**editor-only** — nothing ships in the game runtime. The AI stays *outside* the
editor (the bridge only exposes the editor's own command/import machinery).

## What exists (working)
Pipeline: **layout PNG + real map bounds → MapSpec (JSON) → T3D → live editor via the bridge.**
- `Tools/mapgen/trace.py` / `emit.py` / `make_deckstyle.py` — layer A, unchanged
  (see README). `emit(spec, actors=True, loaders=True)`: pass `loaders=False`
  when importing through the bridge (drops the preloader-cube hack).
- **Layer B is BUILT and smoke-verified** (2026-07-13): `Source/MapForgeBridge/`
  — Editor-type C++ module; localhost-only JSON-lines TCP on 127.0.0.1:8765
  (config: `[MapForgeBridge] Port=` in editor ini / `-MapForgePort=` /
  `MAPFORGE_BRIDGE_PORT` for Python). Polled on the game thread via
  `FTicker::GetCoreTicker()` (SlateRemote pattern). Verbs: ping/status,
  preload_assets, import_t3d, export_t3d, list_actors, delete_actors, exec,
  rebuild_geometry, build (async — poll status), save_level,
  set_surface_material (direct `Surfs[i].Material` + `polyUpdateMaster`,
  brush/normal filters). MCP side: `bridge_*` tools + `forge_mapspec` one-shot
  in `Tools/mapgen/mapforge_mcp.py`.
- **Smoke test passed end-to-end** in a live editor: preloaded 8 asset refs and
  imported a real `UTPainVolume` + functional `Generic_Lift_C` (blocks lifted
  from `capture/deck.t3d`) into a fresh level — the exact things paste couldn't
  do. Surface retexture hit 1 targeted floor face of 434. Export/delete/MAP
  CHECK all good. Test script pattern: poll ping, import, verify, delete, never
  save.

## Hard-won facts (don't re-derive)
- **Root cause unified**: checker materials, lifts, pain-volume sounds/damage
  types all failed under paste because *paste only resolves already-loaded
  objects*. Bridge `preload_assets` (StaticLoadObject) fixes all of them;
  Python `_asset_refs()` regex-harvests every `/Game/`+`/Engine/` ref from the
  T3D (trims `:subobject` suffixes).
- `edactPasteSelected(World, false, false, false, &T3D)` does import + all
  post-fixups with an in-memory string (no clipboard). Buffer must start
  `Begin Map` or ULevelFactory silently imports nothing (EditorFactories.cpp:478).
  Clear the *component* selection first or it pastes components.
- `Exec("BSP ...")`/`Exec("LIGHT ...")` are deprecated no-ops in 4.15
  (EditorServer.cpp:5598). Use `MAP REBUILD ALLVISIBLE`,
  `FEditorBuildUtils::EditorBuild` (FName constants on `FBuildOptions`),
  `FEditorFileUtils::SaveLevel` (SaveMap isn't UNREALED_API).
- No HTTP server module in 4.15 (arrives 4.20); `FTicker` not `FTSTicker`;
  Build.cs uses `ModuleRules(TargetInfo)`; UnrealEd has `UnrealEdSharedPCH.h`
  so `UseExplicitOrSharedPCHs` just works; no UCLASSes in the module → no UHT.
- **Builds: phantaci runs them himself — do NOT invoke UBT/msbuild/UE4Editor.**
  Known-good command is in README (unset `VULKAN_SDK` — the machine's 1.4 SDK
  breaks 4.15 VulkanRHI; bundled headers compile clean; `-NoUBTMakefiles` if a
  new plugin isn't picked up).
- Real map bounds for true scale: postgres `utstats` DB, `utstats_match.map_bounds`.
  Memory: [[mapforge-project]], [[ut4stats-map-bounds]], [[phantaci-identity]],
  [[ut4-cli-build-vulkan-gotcha]], [[dont-run-builds]].

## Next tasks (in rough order — scope-check with phantaci; he drives)
1. **Triage the external audit** (a Codex audit of the bridge was planned —
   check for its findings/report before touching the C++). Known soft spots
   already on the list: no auth token on the socket (any local process can
   drive the editor), selection clobbering, no FScopedTransaction (undo),
   RecvBuf growth on garbage input, SendLine can stall the game thread ~10s on
   a wedged client, behavior during PIE/modal dialogs untested.
2. **Emitter authoring for goo pools + lifts** — the bridge *imports* them
   (proven); `emit.py`/MapSpec can't *author* them yet. Grammar to replicate is
   in `capture/deck.t3d`: `UTPainVolume` = brush actor (polylist + BodySetup
   AggGeom + DamagePerSec/sounds), lines ~33083; `Generic_Lift_C` = BP actor
   (SCS components + `Lift Destination`/`Lift Time`), lines ~35512. Then
   make_deckstyle's twin sludge channels become real recessed lethal pools and
   its jump pads can become lifts.
3. **Retire the paste workflow in docs/examples** once (2) lands; keep it as
   fallback.
4. **Layout → MapSpec vision step** (elimplus PNG → structured spec).
5. Deferred by explicit scope decision: structure recognition (geometry-graph
   semantic labels, lift lift-and-shift, ramp-from-slope).

Working state note: Layer B changes are **uncommitted** in the plugin repo
(new .uplugin, Source/, mapforge_mcp.py + emit.py + README/NEXT edits) —
phantaci reviews/commits.
