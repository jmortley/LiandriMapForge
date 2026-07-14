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
- **Codex MCP registration added (restart required):**
  ```toml
  [mcp_servers.mapforge]
  command = "python"
  args = ['C:\UnrealTournament\UnrealTournament\Plugins\LiandriMapForge\Tools\mapgen\mapforge_mcp.py']
  ```
- **Builds: phantaci runs them himself — do NOT invoke UBT/msbuild/UE4Editor.**
  Known-good command is in README (unset `VULKAN_SDK` — the machine's 1.4 SDK
  breaks 4.15 VulkanRHI; bundled headers compile clean; `-NoUBTMakefiles` if a
  new plugin isn't picked up).
- Real map bounds for true scale: postgres `utstats` DB, `utstats_match.map_bounds`.
  Memory: [[mapforge-project]], [[ut4stats-map-bounds]], [[phantaci-identity]],
  [[ut4-cli-build-vulkan-gotcha]], [[dont-run-builds]].

## Next tasks (in rough order — scope-check with phantaci; he drives)
1. **Verify the audit-fix batch** (2026-07-13: Codex audited the bridge — all
   16 findings accepted and patched in one pass: reversed TCP close detection
   [this fork's Recv returns true/0 for would-block, FALSE for close —
   SocketsBSD.cpp:187], queued non-blocking sends, framing caps, PIE/build
   gating, FScopedTransaction undo, selection restore, current-level scoping
   for list/delete/export, lighting poll via `IsLightingBuildCurrentlyRunning`
   [NOT IsBuildCurrentlyRunning — that only tracks InProgressBuildId],
   FGCObject preload pinning, save fail-fast, strict error propagation,
   `loaders=False` actually working, bare `Texture=/Game/...` ref harvesting).
   **Needs: phantaci builds, then `python Tools/mapgen/tests/bridge_smoke.py`
   against a running editor.** Auth deliberately left out (localhost-only,
   trusted box — his explicit call).
2. **Emitter authoring for goo pools + lifts** — the bridge *imports* them
   (proven); `emit.py`/MapSpec can't *author* them yet. Grammar fixtures are
   TRACKED at `Tools/mapgen/capture/fixtures/utpainvolume.t3d` (brush actor:
   polylist + BodySetup AggGeom + DamagePerSec/sounds) and
   `fixtures/generic_lift.t3d` (BP actor: SCS components + `Lift Destination`/
   `Lift Time`). Then make_deckstyle's twin sludge channels become real
   recessed lethal pools and its jump pads can become lifts. Prefer a minimal
   stable property subset over cloning whole construction-script blocks.
3. **Retire the paste workflow in docs/examples** once (2) lands; keep it as
   fallback.
4. **Layout → MapSpec vision step** (elimplus PNG → structured spec).
5. Deferred by explicit scope decision: structure recognition (geometry-graph
   semantic labels, lift lift-and-shift, ramp-from-slope).

Also uncommitted: **`forge_chassis_physasset`** bridge verb (asset authoring, not
level) — duplicates a skeletal mesh's default UPhysicsAsset, keeps one bone's
body, drops the rest + constraints, saves. Optional convenience for UTVehicles'
UT3 imports (runtime already neutralizes extra bodies). Adds `AssetRegistry`
to the module deps. APIs verified in-tree: PhysicsAsset.h:66/73/153/156,
SkeletalMesh.h:688/732, StaticDuplicateObject (UObjectGlobals.h:281). Smoke:
run on SK_VH_Scorpion_001 with defaults → 1 body / 0 constraints, rerun to
confirm clean overwrite; all 10 vehicles under
/Game/RestrictedAssets/Proto/UT3_Vehicles/ are valid inputs.
See memory [[utvehicles-project]].

Also uncommitted: **Blueprint authoring Tiers 1 + 2** — bridge verbs
`create_blueprint`, `set_class_defaults` (Tier 1: subclass AUTMutator/AUTGameMode
+ CDO property overrides, the config-variant path) and `add_variable`,
`import_graph`, `compile_blueprint`, `export_graph` (Tier 2: graph logic via the
clipboard-text round-trip). MCP tools: `bridge_create_blueprint`,
`bridge_set_class_defaults`, `forge_bp_variant`, `bridge_add_variable`,
`bridge_import_graph`, `bridge_compile_blueprint`, `bridge_export_graph`,
`forge_bp_graph` (one-shot create→add-vars→rehome→import→compile), plus the
`rehome_graph_text` helper (repoints owning-class refs into the new BP).

All APIs in UnrealEd (already a dep; **zero new module deps**) — verified in-tree:
CreateBlueprint/CompileBlueprint (KismetEditorUtilities.h:66/108), AddMemberVariable
(BlueprintEditorUtils.h:709), ImportNodesFromText/ExportNodesToText/CanImportNodesFromText
(EdGraphUtilities.h:107/98/115), FEdGraphPinType (EdGraphPin.h:126, Engine),
node error fields bHasCompilerMessage/ErrorType/ErrorMsg (EdGraphNode.h:141/167/171).
K2Node_* are resolved by the loaded BlueprintGraph module's text factory at import
time, so we never link them (sidesteps their missing dllexports). All asset-writing
verbs are in IsMutatingCmd (SavePackage → PIE-gated). `compile_blueprint` reports
node-level errors so import-success≠validity is caught.

Smoke (after build + editor restart):
- Tier 1: `forge_bp_variant("UTMutator", "/Game/Mods/Mutators/Mutator_Test", {...})`
  → reopen, confirm Class Defaults panel shows the override.
- Tier 2 round-trip (no hand-authoring): `bridge_export_graph` a shipped BP mutator
  (e.g. `Mutator_LowGrav`) → `forge_bp_graph(parent="UTMutator", package=..., graph_t3d=<that>)`
  → expect compiled.status up_to_date (add its self-context vars via `variables=[...]`
  if any). See `capture/fixtures/README.md` for capture guidance.

Also uncommitted: **static mesh import/configure/place + exec hardening**. Bridge
verbs `import_static_mesh`, `configure_static_mesh`, `place_static_mesh` (+ MCP
`bridge_import_static_mesh`/`bridge_configure_static_mesh`/`bridge_place_static_mesh`).
Import path = `IAssetTools::ImportAssetsAutomated(const UAutomatedAssetImportData&)`
driving a configured `UFbxFactory` — **no dialog, no Exec, no OBJ IMPORT**; `.obj`
rides the same factory as `.fbx` (`FbxFactory.cpp:33` registers `obj`→`UStaticMesh`;
there is no `UStaticMeshFactory`). Verified-in-tree API notes: `ImportAssetsAutomated`
is `const&`→`TArray<UObject*>` (IAssetTools.h:158); `bCombineMeshes` is on
`UFbxImportUI` not the import data; dialog gated on `IsAutomatedImport()`
(Factory.h:175); config uses `StaticMaterials`/`BodySetupEnums.h` (`CTF_UseComplexAsSimple`,
note different value order — named enums only)/`SourceModels[0].BuildSettings`/guarded
`RenderData`. Asset named exactly via a temp-copy staged as `<name>.<ext>`
(`FPaths::GameIntermediateDir()` — 4.15 name). Build.cs adds only `AssetTools`
(include-path + dynamically-loaded, per ContentBrowser precedent). All three verbs
in `IsMutatingCmd`; import/place never save the level.

`exec` hardened with a blocklist (`IsDangerousExecCommand`): `MAP LOAD/NEW/IMPORT`,
`OBJ IMPORT/LOAD`, `QUIT/EXIT` rejected *before* `GEditor->Exec` — the `MAP LOAD`
of a bad `.umap` OOM-crashed the editor and fatal engine failures can't be caught
in C++. `rebuild_geometry`/`save_level` keep their own dedicated paths (not routed
through `exec`), so they're unaffected.

Smoke (after build + editor restart): `python Tools/mapgen/tests/bridge_smoke.py`
covers import/stats/complex-as-simple/place-at-identity/overwrite/validation +
exec rejects against the tiny OBJ fixture (`tests/fixtures/tiny_mesh.obj`).
**Manual acceptance** (not in the suite): import
`C:\Users\MrJmo\Documents\Codex\2026-07-13\...\SM_DM_Andok_Scaled_BSP_Recovered_UE4_UV.obj`
with combine_meshes/generate_lightmap_uvs/compute_normals on, MikkTSpace off,
auto simple collision off, collision=complex_as_simple, then place at identity.

Deferred (unchanged): structure recognition (geometry-graph semantic labels,
lift lift-and-shift, ramp-from-slope).

Working state note: bridge v2 is committed/pushed (`1b2e1ec`: audit hardening +
physics-asset forge + Blueprint Tiers 1–2). The **static-mesh import + exec
hardening** batch above is uncommitted, pending phantaci's build + smoke test.
