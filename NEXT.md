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
(`FPaths::GameIntermediateDir()` — 4.15 name). **Multi-material OBJs:** each
`mtllib` `.mtl` sidecar the OBJ names is staged next to the temp copy under its
referenced name (`FFileHelper::LoadFileToString` + per-line `ParseIntoArrayWS`,
OBJ-only — FBX embeds materials), so `usemtl` groups map to distinct slots.
Without the sidecar the FBX SDK reports 0 materials and forces one slot
(`FbxStaticMeshImport.cpp:297,325` — `MaterialCount = Node->GetMaterialCount()`
then the `MaterialCount==0` default-slot branch); slot count is **independent of
`import_materials`**, which only controls UMaterial-asset creation. All staged
temp files are tracked and deleted post-import; missing/unsafe sidecars go to the
result `warnings`. Build.cs adds only `AssetTools` (include-path +
dynamically-loaded, per ContentBrowser precedent). All three verbs in
`IsMutatingCmd`; import/place never save the level.

`exec` hardened with a blocklist (`IsDangerousExecCommand`): `MAP LOAD/NEW/IMPORT`,
`OBJ IMPORT/LOAD`, `QUIT/EXIT` rejected *before* `GEditor->Exec` — the `MAP LOAD`
of a bad `.umap` OOM-crashed the editor and fatal engine failures can't be caught
in C++. `rebuild_geometry`/`save_level` keep their own dedicated paths (not routed
through `exec`), so they're unaffected.

Smoke (after build + editor restart): `python Tools/mapgen/tests/bridge_smoke.py`
covers import/stats/complex-as-simple/place-at-identity/overwrite/validation +
exec rejects against the tiny OBJ fixture (`tests/fixtures/tiny_mesh.obj`), plus a
two-`usemtl` cube (`tests/fixtures/cube_two_materials.obj`) asserting
`material_slot_count == 2` — the MTL-sidecar-staging regression guard.
**Manual acceptance** (not in the suite): import
`C:\Users\MrJmo\Documents\Codex\2026-07-13\...\SM_DM_Andok_Scaled_BSP_Recovered_UE4_UV.obj`
with combine_meshes/generate_lightmap_uvs/compute_normals on, MikkTSpace off,
auto simple collision off, collision=complex_as_simple, then place at identity.

Also uncommitted: **map-scoped recovery layout + read-only actor audit**. Makes a
per-map layout the default for every recovery so two maps sharing asset names
never collide. Two roots, hard-separated:
- **Editable content** under `<RecoveryRoot>/<MapSlug>/` (default
  `/Game/RecoveredMaps`): `Maps/`, `Geometry/BSP`, `Geometry/StaticMeshes`,
  `Materials/`, `Textures/`, `VFX/`, `Audio/`, `Blueprints/`, `Data/`.
- **Non-content** under `Saved/LiandriMapForge/Recoveries/<MapSlug>/`:
  `RawExtract/` (preserves original `Content/…` hierarchy — extracted
  `/Game/RestrictedAssets/…` never flattened/moved), `Interchange/`, `Manifests/`,
  `Reports/`. Stock `/Game`/`/Engine` never moved; only regenerated assets go
  under the recovery root.

New **read-only** verbs (deliberately NOT in `IsMutatingCmd`): `recovery_layout`
(resolve content + Saved paths; `map_name` inferred from current map short name
with recovery suffixes stripped when omitted — inference-only) and
`inspect_static_mesh_actors` (per-`AStaticMeshActor`: mesh-or-null, actor +
component transform, hidden/visible, mobility, collision, material overrides by
slot; summary counts; name/folder filters + offset/limit; excludes pending-kill;
**never dirties the map** — verified in-tree: `AActor::GetActorLabel() const`
returns `const FString&` [no lazy create], `GetFolderPath`/`IsHiddenEd`,
`UStaticMeshComponent::GetStaticMesh`/`GetRelativeTransform`/`Mobility`/
`GetCollisionEnabled`/`OverrideMaterials`, `EComponentMobility`/`ECollisionEnabled`).
Root config: default `/Game/RecoveredMaps`, `[MapForgeBridge] RecoveryRoot=` ini,
`MAPFORGE_RECOVERY_ROOT` env in the MCP (forwarded to the bridge — single
resolver, so both agree). Slug sanitized to `[A-Za-z0-9_-]`; traversal / path
separators / `/Engine` root / invalid package paths rejected.

`import_static_mesh` `destination` is now **optional**: omit it + pass
`map_name`/`category` (default `Geometry/StaticMeshes`, only canonical categories
accepted) to resolve `<root>/<slug>/<category>`; explicit `destination` unchanged.
Result echoes the resolved `destination` + `recovery_layout`.

MCP helpers: `bridge_recovery_layout`, `bridge_inspect_static_mesh_actors`,
`write_recovery_manifest` (atomic JSON via tempfile+`os.replace` to
`…/Manifests/recovery_manifest.json`, never in Content; records source PAK/map,
layout, imported assets, source SHA-256s, material substitutions, actor counts,
warnings, UTC timestamp, engine version), and `audit_static_mesh_actors`
(compares live inspect vs `actors_manifest.json` → missing/extra/null/
substituted/transform-mismatch/missing-override; pure `_diff_actors` unit-tested).

Tests: `tests/test_recovery_offline.py` (pure `_diff_actors`/`_transform_close`/
atomic-write, no editor — run now: **passes**) + `test_recovery()` in
`bridge_smoke.py` (layout derivation/stability/suffix rules, two-map separation,
traversal+`/Engine` rejects, default StaticMeshes/BSP destinations, inspect
no-dirty, live audit substitution+missing). New fixture `traversal_mtllib.obj`
(mtllib `../` → rejected, 1 slot + warning).

Deferred (unchanged): structure recognition (geometry-graph semantic labels,
lift lift-and-shift, ramp-from-slope).

Working state note: bridge v2 is committed/pushed (`1b2e1ec`: audit hardening +
physics-asset forge + Blueprint Tiers 1–2). The **static-mesh import + exec
hardening** batch (now including **MTL-sidecar staging**) and the **map-scoped
recovery layout + actor audit** batch above are uncommitted, pending phantaci's
build + smoke test (then re-run the 14-material Andok acceptance OBJ, confirm all
14 slots, and drive a real recovery through `recovery_layout` +
`write_recovery_manifest` + `audit_static_mesh_actors`).
