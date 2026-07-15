# LiandriMapForge

AI-authored maps for **UnrealTournament (UE4 4.15 fork)**. Point an LLM at the game, get a playable map — but for UT4, seeded
by the [ElimPlus map layouts](https://ut4stats.com/elimplus_maps).

> Standalone **editor-only** plugin, its own repo (sibling to NetcodePlus under
> `Plugins/`). Nothing here ships in a runtime/game build. The AI stays *outside*
> the editor — LiandriMapForge only exposes the editor's own command/import machinery.

## The lever

`.umap` is a binary UE4 package and this 4.15 fork has **no Python editor plugin**,
so the mainstream "UnrealMCP" path is out. But this engine's editor round-trips
**BSP brushes and actors as plain T3D text** — verified in-source:

| Piece | Source (this fork) |
|---|---|
| Polylist grammar | `EditorExporters.cpp:295-337` |
| Brush import + finalize | `EditorObject.cpp:293-320`, `:641-655` |
| `BrushType=Brush_Add` | `Brush.h:82` (enum `:33`) |
| Brush component `BrushComponent0` | `Brush.cpp:38` |
| Non-qualified `Class=` resolution | `EditorObject.cpp:245` (`PPF_AttemptNonQualifiedSearch`) |
| Editor command server (later, for the bridge) | `UnrealEdSrv.cpp:1961` `Exec_Actor`, `EditorServer.cpp:664` `Exec_Brush` |

So an LLM authors text; it becomes real geometry in a `.umap`.

> **Caution / lineage note:** the OldUnreal "editor MCP" precedent is **UT99 /
> UnrealEd 1-2** — its command *concept* survived into UE4 (verified above), but its
> exact verbs, class-path syntax, and BSP-first workflow do **not** transfer. We
> anchor only on symbols confirmed in this fork's tree.

## Pipeline

```
elimplus layout PNG ──(vision)──▶ MapSpec (JSON) ──(emit.py)──▶ T3D ──▶ .umap
```

- **MapSpec** (`Tools/mapgen/mapspec.schema.json`) — the LLM-authoring interface.
  A boxy UT arena is a 2D floorplan extruded to a wall height — which is exactly
  what an ElimPlus top-down icon is.
- **emit.py** (`Tools/mapgen/emit.py`) — MapSpec → T3D. Geometry verified; box
  winding/normals computed deterministically. Python now (forward-compatible with
  an MCP server); portable to a C++ commandlet/editor module later.

## Roadmap

- [x] Scope + lock T3D grammar from engine source
- [x] MapSpec v0 schema + example
- [x] Emitter: rooms → BSP box brushes (verified geometry)
- [x] Paste-test proven — `SmokeCube` imported + built solid, playable in ElimPlus
- [x] Ground-truth capture → actor serialization locked (`capture/ground-truth.t3d`)
- [x] Emitter: doorway gaps + actors (PlayerStart / PointLight / BP pickups)
- [x] MCP connector, layer A (stateless) — `Tools/mapgen/mapforge_mcp.py` wraps the emitter as MCP tools
- [x] **MCP connector, layer B — the live editor bridge.** C++ editor-only module (`MapForgeBridge`) inside UnrealEd: localhost JSON-lines TCP → T3D import, asset preload, CSG rebuild, lighting/nav build, save, T3D export, BSP surface materials, actor query/delete, raw exec. Fronted by `bridge_*` / `forge_mapspec` MCP tools. The UE4.15 analog of Epic's UE5.8 editor MCP.
- [x] Bridge hardening pass (external audit) — reversed-close fix, queued non-blocking sends, framing caps, PIE/build gating, undo transactions, current-level scoping, GC-pinned preloads, strict error propagation
- [x] **Asset authoring, not just levels** — the bridge grew past map building:
  - `forge_chassis_physasset` — duplicate-and-strip a skeletal mesh's `UPhysicsAsset` to a clean chassis-only asset (UTVehicles `PhysicsAssetOverride`)
  - **Blueprint authoring Tier 1** — `create_blueprint` a subclass of `AUTMutator`/`AUTGameMode` + `set_class_defaults` (CDO overrides): the config-variant path, no graph
  - **Blueprint authoring Tier 2** — `add_variable` + `import_graph` (clipboard-text) + `compile_blueprint` + `export_graph`: real graph logic, seeded by re-homing a graph captured from a shipped BP
- [ ] Emitter authoring for goo POOLS (`UTPainVolume`) + functional lifts (`Generic_Lift_C`) — the bridge imports both correctly (verified against `capture/deck.t3d` blocks); MapSpec/emit.py just can't author them yet
- [ ] Verticality + weapons — jump pads, lifts, `WeaponBase_C` + ammo (serialization captured from a real map)
- [ ] **Layout → MapSpec vision step** (elimplus PNG → structured spec)
- [ ] Structure recognition (geometry-graph: earn semantic labels like "bridge"; lift lift-and-shift; ramp detection) — deferred past bridge v1 by design

## Layer B — the live editor bridge

`Source/MapForgeBridge/` is an **Editor-type** C++ module that runs inside
UnrealEd and listens on **localhost only** (default `127.0.0.1:8765`; override
with `[MapForgeBridge] Port=` in the per-project editor ini, `-MapForgePort=`
on the editor command line, or `MAPFORGE_BRIDGE_PORT` for the Python side).
Protocol: JSON-lines over TCP — `{"id","cmd","args"}` in, `{"id","ok","result"|"error"}` out.
Everything executes on the game thread via the core ticker (the engine's own
SlateRemote pattern), because editor APIs are game-thread-only. Mutating verbs
are rejected while PIE/simulate or a lighting/editor build is active, run
inside undo transactions, and fail loudly (`ok=false`) instead of reporting
partial work as success. **Security posture (deliberate):** localhost-only
bind, no auth token — a trusted single-user dev box; any local process can
drive the editor while it runs.

The verbs span three areas: **level editing** (`import_t3d`/`export_t3d`/
`rebuild_geometry`/`build`/`save_level`/`set_surface_material`/`list_actors`/
`delete_actors`/`place_static_mesh`), **asset authoring** (`forge_chassis_physasset`
physics assets; `create_blueprint`/`set_class_defaults`/`add_variable`/`import_graph`/
`compile_blueprint`/`export_graph` for Blueprint mutators & gamemodes;
`import_static_mesh`/`configure_static_mesh` for meshes), and the restricted
`exec` escape hatch. Asset-authoring verbs touch packages, not the level.

| Verb | What it does |
|---|---|
| `ping` / `status` | engine version, current map, dirty + locked flags, actor count, `pie_active`, and build flags — poll `lighting_building` for Lightmass, `editor_building` for other builds |
| `preload_assets` | `StaticLoadObject` each path, **pinned against GC** via FGCObject — retires the material-preloader hack and makes BP actors (lifts, pickups), sounds and damage types resolve on a fresh level |
| `import_t3d` | `edactPasteSelected` with an in-memory buffer (no clipboard) + the editor's own post-import fixups; errors if nothing imports; new actors stay selected (paste UX) |
| `export_t3d` | current level (or selection) back as T3D — the read-back half of the loop; prior selection restored |
| `rebuild_geometry` | `MAP REBUILD ALLVISIBLE` (the real CSG path; `BSP`/`LIGHT` exec are deprecated no-ops in 4.15) |
| `build` | lighting / geometry / paths / all via `FEditorBuildUtils`; lighting is async (poll `status`), explicit nav builds are synchronous in 4.15 |
| `save_level` | `FEditorFileUtils::SaveLevel`; never-saved levels need `filename` (fails fast instead of blocking on the modal Save As) |
| `set_surface_material` | direct `Surfs[i].Material` + `ModifySurf` + `polyUpdateMaster` (selection-independent, survives CSG rebuild, undoable); filter by brush name and/or face normal |
| `list_actors` / `delete_actors` | query + clear for iterative re-import — **current level scope** (actor names are only unique per level); builder brush/WorldSettings protected, locked levels rejected |
| `exec` | restricted `GEditor->Exec` escape hatch, output captured. **Blocklisted:** `MAP LOAD`/`MAP NEW`/`MAP IMPORT`, `OBJ IMPORT`/`OBJ LOAD`, and `QUIT`/`EXIT` are rejected *before* Exec — a bad `MAP LOAD` of an incompatible `.umap` fatally OOM-crashed the editor, and fatal engine failures can't be caught in C++, so they're prevented. Use dedicated verbs (`import_t3d`, `import_static_mesh`) instead |
| `forge_chassis_physasset` | asset authoring (not level): duplicate a skeletal mesh's default `UPhysicsAsset`, keep only one bone's body, drop the rest + all constraints, save the result — clean chassis-only asset for UTVehicles `PhysicsAssetOverride`. Never mutates the source or the mesh |
| `create_blueprint` | asset authoring: `FKismetEditorUtilities::CreateBlueprint` a subclass of a native/BP parent (e.g. `AUTMutator`), compile, save. **Tier-1 BP authoring** |
| `set_class_defaults` | write CDO property defaults on a Blueprint class (compile → `GeneratedClass` CDO → `UProperty::ImportText` → save); inherited mutator/gamemode props are valid targets — the config-variant path, no graph |
| `add_variable` | `FBlueprintEditorUtils::AddMemberVariable` — scalar/object/class/struct types (+`[]`); needed before importing a graph that references the var by self-context |
| `import_graph` | `FEdGraphUtilities::ImportNodesFromText` into the event/function graph + the editor's post-paste fix-up. **Tier-2 graph authoring** (clipboard-text) |
| `compile_blueprint` | `CompileBlueprint` + node-level error/warning report (`bHasCompilerMessage`/`ErrorType`/`ErrorMsg`); import success ≠ validity, so always compile |
| `export_graph` | `FEdGraphUtilities::ExportNodesToText` of a graph — read-back + the way to capture graph fixtures from a real Blueprint |
| `import_static_mesh` | import a `.obj`/`.fbx` as a `UStaticMesh` via the automated `UFbxFactory` (no dialog, no Exec); static-mesh-only, no materials by default, combine on; configures collision + lightmap + returns full mesh stats. `.obj`/`.fbx` only — never `.umap`/packages |
| `configure_static_mesh` | set collision mode, clear simple collision, lightmap UV channel, and assign materials to explicit slots on an imported mesh; optional per-asset save |
| `place_static_mesh` | spawn a `AStaticMeshActor` for a mesh into the current level at a transform (identity default), with label/folder; undoable, marks level dirty + redraws, **never saves the level** |
| `recovery_layout` | **read-only**: resolve the canonical map-scoped recovery layout (content paths under `<RecoveryRoot>/<MapSlug>/…` + on-disk Saved paths) for a `map_name` (inferred from the current map, recovery suffixes stripped, when omitted). Sanitizes the slug, rejects traversal/`/Engine`/invalid roots. Not in `IsMutatingCmd` |
| `inspect_static_mesh_actors` | **read-only** audit of every `StaticMeshActor` in the current level: mesh-or-null, actor + component transform, hidden/visible, mobility, collision, material overrides by slot, plus summary counts. Name/folder filters, offset/limit. Excludes pending-kill; **never dirties the map**. Not in `IsMutatingCmd` |

Limits: 4 clients, 64 MiB per request line, bounded per-tick socket work — a
wedged or hostile local client gets dropped, never a frozen editor. Regression
suite: `python Tools/mapgen/tests/bridge_smoke.py` against a running editor.

### Static mesh import

`import_static_mesh` brings an OBJ or FBX in as a `UStaticMesh` through the
editor's own automated `UFbxFactory` path (`.obj` is registered there too, so
both extensions share one code path) — **no modal dialog, no `GEditor->Exec`,
no `OBJ IMPORT`**. Request schema:

```json
{
  "source": "C:\\absolute\\path\\mesh.obj",
  "destination": "/Game/Developers/MrJmo/Recovered",
  "name": "SM_RecoveredShell",
  "overwrite": false,
  "save": true,
  "options": {
    "combine_meshes": true, "import_materials": false, "import_textures": false,
    "generate_lightmap_uvs": true, "compute_normals": true, "use_mikk_tspace": false,
    "collision": "complex_as_simple", "lightmap_coordinate_index": 1
  }
}
```

Safety / behavior:
- Validated **before** any editor mutation: `source` must be an existing
  absolute `.obj`/`.fbx` file; `destination` a valid `/Game/…` folder (never
  `/Engine`, `.umap`, or a package path); `name` is sanitized. Existing target
  with `overwrite:false` fails cleanly — never a prompt.
- The asset is named exactly `name` (a temp copy is staged, since the automated
  importer names by filename). Materials/textures/skeletal/animation are off by
  default; `collision:"complex_as_simple"` sets `CTF_UseComplexAsSimple` and
  strips generated simple collision.
- **Multi-material OBJs:** each `mtllib` sidecar `.mtl` referenced by the OBJ is
  staged next to the temp copy under its original name, so every `usemtl` group
  lands in its own material slot. Without the sidecar the FBX SDK sees no
  materials and collapses the whole mesh to a single slot — so this removes the
  old need to split a mesh into N single-material OBJs. Slot count comes from the
  `usemtl` groups, so `import_materials` can stay off; missing sidecars are
  reported in the result `warnings`. FBX embeds its materials and needs none.
- Importing a mesh **does not save the current map**. `place_static_mesh` spawns
  an actor and marks the level dirty but likewise never saves it — save the
  level yourself via `save_level` when ready.
- MCP tools: `bridge_import_static_mesh`, `bridge_configure_static_mesh`,
  `bridge_place_static_mesh`. The smoke test uses tiny tracked OBJ fixtures
  (`tests/fixtures/tiny_mesh.obj`, plus `cube_two_materials.obj` which asserts a
  two-`usemtl` cube imports as two slots); the large Andok acceptance OBJ is
  exercised manually, not in the automated suite.

The MCP proxy exposes these as `bridge_*` tools plus one-shot convenience tools:
**`forge_mapspec`** (MapSpec → preload → import → CSG rebuild, no paste/preloader
cubes via `emit.emit(..., loaders=False)`), **`forge_bp_variant`** (create BP +
set CDO defaults), and **`forge_bp_graph`** (create BP → add vars → re-home graph
text → import → compile; `rehome_graph_text` repoints owning-class refs into the
new class). Blueprint graph fixtures are captured from real BPs via `export_graph`,
never hand-authored — see `Tools/mapgen/capture/fixtures/README.md`.

### Map-scoped recovery layout

Every recovery gets its own map-scoped home so two maps that happen to share
asset names (`SM_Shell`, `M_Wall`, …) never collide. There are **two roots** with
a hard separation:

**Editable / regenerated Unreal assets** — under the content recovery root
(default `/Game/RecoveredMaps`), per map slug:

```
/Game/RecoveredMaps/<MapSlug>/
    Maps/  Geometry/BSP/  Geometry/StaticMeshes/  Materials/
    Textures/  VFX/  Audio/  Blueprints/  Data/
```

**Non-content recovery files** (raw PAK extraction, interchange, manifests,
reports) — under the project's `Saved/`, **never inside the Content tree**:

```
<Project>/Saved/LiandriMapForge/Recoveries/<MapSlug>/
    RawExtract/  Interchange/  Manifests/  Reports/
```

Rules the resolver enforces:

- Raw PAK extraction preserves its original `Content/…` hierarchy under
  `RawExtract/` — extracted `/Game/RestrictedAssets/…` dependencies are **never
  flattened, relocated, or moved**. Only *regenerated/editable* assets are placed
  under `/Game/RecoveredMaps/<MapSlug>`; stock `/Game` and `/Engine` assets are
  left where they are.
- `MapSlug` is sanitized to `[A-Za-z0-9_-]`. Traversal (`..`), path separators,
  empty names, `/Engine` roots, and invalid package paths are rejected.
- Empty content folders aren't force-created; importing the first asset
  materializes them.

**Resolve it** with `recovery_layout` / `bridge_recovery_layout` (read-only):

```python
bridge_recovery_layout(map_name="DM-Andok_Scaled")
# -> content_root=/Game/RecoveredMaps/DM-Andok_Scaled,
#    static_meshes=/Game/RecoveredMaps/DM-Andok_Scaled/Geometry/StaticMeshes,
#    filesystem_root=<Project>/Saved/LiandriMapForge/Recoveries/DM-Andok_Scaled, ...
```

Omit `map_name` to infer it from the current map's short name, stripping one
recovery suffix (`-Recovered-Editable`, `_Recovered_Editable`, `-Recovered`,
`_Recovered`) — inference only; an explicit `map_name` is used verbatim.

**Configure the root** (bridge and MCP agree — the bridge is the single
resolver): default `/Game/RecoveredMaps`, overridable by `[MapForgeBridge]
RecoveryRoot=` in the editor ini, or the `MAPFORGE_RECOVERY_ROOT` env var in the
MCP layer (forwarded to the bridge).

**Default mesh destination.** `import_static_mesh` now takes `destination` as
*optional*. Omit it and pass `map_name` + `category` to land the asset in the
recovery layout (default category `Geometry/StaticMeshes`); an explicit
`destination` still works exactly as before.

```python
# two different recovered maps, same source asset name, no collision:
bridge_import_static_mesh(source=r"C:\Recovery\SM_Shell.obj",
                          map_name="DM-Andok_Scaled", category="Geometry/BSP")
#   -> /Game/RecoveredMaps/DM-Andok_Scaled/Geometry/BSP/SM_Shell
bridge_import_static_mesh(source=r"C:\Recovery\SM_Shell.obj",
                          map_name="DM-Deck_Scaled")   # default category
#   -> /Game/RecoveredMaps/DM-Deck_Scaled/Geometry/StaticMeshes/SM_Shell
```

Allowed `category` values are the canonical folders only (`Maps`, `Geometry/BSP`,
`Geometry/StaticMeshes`, `Materials`, `Textures`, `VFX`, `Audio`, `Blueprints`,
`Data`); anything else — including traversal — is rejected.

**Recovery manifest.** `write_recovery_manifest` (MCP) writes an atomic JSON to
`…/Recoveries/<MapSlug>/Manifests/recovery_manifest.json` (never inside Content)
recording the source PAK/map, resolved layout, imported asset paths, source
filenames + SHA-256 hashes, material substitutions, actor placement counts,
warnings, a UTC timestamp, and the plugin/engine version.

### Recovery audit workflow

`inspect_static_mesh_actors` / `bridge_inspect_static_mesh_actors` returns a
read-only snapshot of every `StaticMeshActor` (mesh-or-null, actor + component
transform, hidden/visible, mobility, collision, material overrides by slot) plus
summary counts (`total_static_mesh_actors`, `actors_null_mesh`,
`unique_mesh_paths`, `actors_hidden`, `actors_unresolved_materials`). It excludes
pending-kill actors, supports name/folder filters + pagination, and **never
dirties the map**, so it's safe at editor idle.

`audit_static_mesh_actors(actors_manifest_path)` (MCP) compares that live
snapshot against an `actors_manifest.json` and reports `missing_actors`,
`extra_actors`, `null_mesh_actors`, `mesh_substitutions`, `transform_mismatches`,
and `missing_material_overrides` — the precise remaining-gap list for a recovery.
It mutates nothing.

Build (from the UT4 fork root; **unset `VULKAN_SDK`** — a modern Vulkan SDK
breaks 4.15's VulkanRHI, and without the env var the engine uses its bundled
headers):

```
env -u VULKAN_SDK Engine/Binaries/DotNET/UnrealBuildTool.exe \
    UnrealTournamentEditor Win64 Development \
    -project="<fork>/UnrealTournament/UnrealTournament.uproject" -waitmutex
```

## Try the PoC (layer A, paste workflow)

```
cd Tools/mapgen
python emit.py examples/DM-Box01.mapspec.json --out out/
```

Then in UnrealEd: new level → **Edit > Paste** `out/smoke_cube.t3d` (one box first).
If a solid cube appears, the pipeline is proven — paste `DM-Box01.geometry.t3d` for
the room shell. **Post-paste:** Build Geometry, then Build Lighting (else unlit).
The paste path still works but the bridge supersedes it: same T3D, minus the
manual steps and minus the preloader-cube workaround.
