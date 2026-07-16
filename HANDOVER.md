# LiandriMapForge — Handover

> Orientation + integration handover for **LiandriMapForge** and the planned
> **UT4X-Converter** integration. Read this first if you're picking the project
> up cold. Companion docs: `README.md` (user-facing reference), `NEXT.md`
> (continuation prompt / uncommitted-work log).
>
> _Last updated 2026-07-15 (recovery pipeline + sound verbs committed & pushed as
> `80b8272`). **Current state, LA-pickup order, and the Clutch map task: §8–§9.**_

---

## 0. TL;DR

LiandriMapForge is a **standalone, editor-only** plugin for **UnrealTournament
(the UE4 4.15 fork)** that lets you author *and recover* UT4 maps through **T3D
text** and a **live in-editor bridge** — the UT4 analog of Epic's UE 5.8 editor
MCP, for the engine Epic's tooling will never touch. Nothing ships in the game
runtime. Two layers: a stateless **emitter** (MapSpec → T3D) and a C++
**MapForgeBridge** module that exposes UnrealEd's import/build/save machinery over
a localhost JSON-lines socket, fronted by the `mapforge` **MCP** connector.

Recent work added a **map-scoped recovery pipeline** (canonical layout + mesh &
sound import + read-only audit + provenance manifest). The next big thread is
wiring **UT4X-Converter** (a Java tool that converts legacy Unreal/UT maps to
UT4 but stops at "manual editor import") to use this bridge as its **automated
import + verification backend** — see §7.

**Operational rules:** builds are run by phantaci, never by the assistant
(hand him the command). Verify every API against this fork's headers
(`C:\UnrealTournament\Engine`) — no UE5 assumptions.

---

## 1. What it is & why

**Goal:** generate and recover playable UT4 maps — geometry, spawns, items,
lights, real ShellResources materials — driven by text (MapSpec/T3D) and an
LLM, without ever putting the AI *inside* the editor.

**Why standalone (phantaci's call):** its own repo / editor-only plugin at
`Plugins/LiandriMapForge` (sibling to NetcodePlus), **not** inside NetcodePlus.
The AI stays *outside* the editor — the bridge only ever exposes UnrealEd's
existing command/import machinery over a thin protocol. (OldUnreal's UT99
"editor MCP" drew backlash for perceived AI-in-editor; this design defuses that.)

**Positioning:** Epic shipped an official editor MCP for **UE 5.8**. It does not
threaten this project — UT4 is frozen on UE 4.15, which Epic's tooling will never
run on, so MapForge is the only option for UT4 (same relationship as OldUnreal to
UT99). Epic shipping it officially *validates* the approach. The moat is
UT4-specific + ElimPlus-layout seeding, not feature breadth.

**Scope:** deliverable = a playable **whitebox** (true-scale geometry + spawns +
items + lights + real materials). Art-pass polish is deliberately **out of
scope** — left to whoever uses the tool.

---

## 2. Architecture

Two layers plus the MCP connector:

### Layer A — stateless emitter (no editor required)
`MapSpec (JSON)` → `emit.py` → **UT4 T3D**. Pastable via Edit > Paste, or handed
to the bridge. Pure Python; no engine dependency.

- `Tools/mapgen/mapspec.schema.json` — the authoring contract.
- `Tools/mapgen/emit.py` — the emitter. `emit(spec, actors=True, loaders=True)`;
  pass `loaders=False` when importing through the bridge (drops the old
  preloader-cube hack).
- `Tools/mapgen/trace.py`, `make_deckstyle.py` — layout tracing / deck-style gen.

### Layer B — live editor bridge (C++ `MapForgeBridge` module)
An **Editor-type** C++ module. UnrealEd (with the plugin) listens on
**127.0.0.1:8765**, JSON-lines protocol: one `{"id","cmd","args"}` per line in,
one `{"id","ok","result"|"error"}` back. Polled on the **game thread** via
`FTicker::GetCoreTicker()` (the engine's SlateRemote pattern — editor APIs are
game-thread-only). Non-blocking socket. Implements `FGCObject` to root assets
pinned by `preload_assets` so they survive GC between preload and import.

- `Source/MapForgeBridge/Private/MapForgeBridgeServer.{h,cpp}` — the server + all
  verbs (this is the heart of the plugin; ~2800 lines of `.cpp`).
- `Source/MapForgeBridge/Private/MapForgeBridgeModule.cpp` — module lifecycle.
- `Source/MapForgeBridge/MapForgeBridge.Build.cs` — deps (UnrealEd + AssetTools).

### MCP connector (stateless proxy)
`Tools/mapgen/mapforge_mcp.py` — FastMCP server exposing Layer A + every bridge
verb as `bridge_*` tools, plus one-shot convenience tools and the recovery
helpers. Any MCP client (Claude Code, Codex, Claude Desktop) drives it.
`pip install mcp`; register: `claude mcp add mapforge -- python "<abs>/Tools/mapgen/mapforge_mcp.py"`.

### Tests
- `Tools/mapgen/tests/bridge_smoke.py` — end-to-end regression against a **running
  editor** (non-destructive: imports in-memory, never saves).
- `Tools/mapgen/tests/test_recovery_offline.py` — **editor-free** unit tests for
  the recovery diff/manifest helpers.
- `Tools/mapgen/tests/fixtures/` — `tiny_mesh.obj/.mtl`, `cube_two_materials.obj/.mtl`
  (multi-material slot guard), `traversal_mtllib.obj` (MTL-traversal reject).
- `Tools/mapgen/capture/fixtures/` — real T3D fixtures (pain volume, lift) + BP
  graph fixtures captured via `export_graph`.

---

## 3. Bridge command surface

All verbs live in `Dispatch()` in `MapForgeBridgeServer.cpp`. **Mutating** verbs
(gated on editor state — rejected during PIE/simulate and lighting/editor builds)
are listed in `IsMutatingCmd()`. **Read-only** verbs stay available always and
never dirty the map.

| Verb | Kind | Purpose |
|---|---|---|
| `ping` / `status` | read | engine ver, current map, dirty flag, level lock, actor count, PIE/build flags |
| `preload_assets` | read | `StaticLoadObject` + GC-root a list of `/Game`/`/Engine` paths |
| `import_t3d` | mutate | `edactPasteSelected` a T3D buffer (must start `Begin Map`); auto-fixups |
| `export_t3d` | read | export selected/all actors to T3D text |
| `list_actors` / `delete_actors` | read / mutate | query / clear by name+class (current level scope) |
| `exec` | mutate | restricted `GEditor->Exec`; **blocklist** rejects `MAP LOAD/NEW/IMPORT`, `OBJ IMPORT/LOAD`, `QUIT/EXIT` before Exec |
| `rebuild_geometry` | mutate | `MAP REBUILD ALLVISIBLE` (run after importing brushes) |
| `build` | mutate | lighting/geometry/paths via `FEditorBuildUtils`; lighting is async (poll `status`) |
| `save_level` | mutate | `FEditorFileUtils::SaveLevel`; never-saved levels need `filename` (no modal) |
| `set_surface_material` | mutate | direct `Surfs[i].Material` + `polyUpdateMaster`; brush/normal filters; survives CSG rebuild |
| `forge_chassis_physasset` | mutate | asset authoring: duplicate a skeletal mesh's PhysicsAsset, keep one bone's body |
| `create_blueprint` / `set_class_defaults` | mutate | **BP Tier 1** — subclass + CDO defaults (config-variant path) |
| `add_variable` / `import_graph` / `compile_blueprint` / `export_graph` | mutate/read | **BP Tier 2** — graph logic via clipboard-text round-trip |
| `import_static_mesh` | mutate | `.obj`/`.fbx` → `UStaticMesh` via automated `UFbxFactory` (no dialog/Exec); **MTL-sidecar staging**; optional map-scoped destination |
| `configure_static_mesh` | mutate | collision mode, lightmap channel, slot material assignment |
| `place_static_mesh` | mutate | spawn `AStaticMeshActor` at a transform (never saves) |
| `import_sound` | mutate | 16-bit PCM WAV → `USoundWave` (no dialog) |
| `create_sound_cue` | mutate | build a modal-free `USoundCue` graph from imported waves |
| **`recovery_layout`** | **read** | resolve the canonical map-scoped recovery layout (§4); not in `IsMutatingCmd` |
| **`inspect_static_mesh_actors`** | **read** | audit every `StaticMeshActor` (§4); **never dirties the map** |

MCP tools mirror these as `bridge_*`, plus one-shots: **`forge_mapspec`**
(MapSpec → preload → import → rebuild), **`forge_bp_variant`**, **`forge_bp_graph`**,
**`generate_t3d`**, **`smoke_cube`**, **`list_pickups`**, **`mapspec_schema`**,
and the recovery helpers **`write_recovery_manifest`** / **`audit_static_mesh_actors`**.

---

## 4. The recovery pipeline (most recent work)

Purpose: give every map recovery its own **map-scoped home** so two maps that
share asset names never collide, plus a **read-only audit** to pinpoint remaining
gaps. Two roots, hard-separated:

**Editable / regenerated Unreal assets** — under the content recovery root
(default `/Game/RecoveredMaps`), per slug:
```
/Game/RecoveredMaps/<MapSlug>/
    Maps/  Geometry/BSP/  Geometry/StaticMeshes/  Materials/
    Textures/  VFX/  Audio/  Blueprints/  Data/
```
**Non-content files** (raw PAK extraction, interchange, manifests, reports) —
under the project's `Saved/`, **never inside the Content tree**:
```
<Project>/Saved/LiandriMapForge/Recoveries/<MapSlug>/
    RawExtract/  Interchange/  Manifests/  Reports/
```

Rules the resolver enforces: raw extraction preserves its original `Content/…`
hierarchy under `RawExtract/`; extracted `/Game/RestrictedAssets/…` deps are
never flattened/moved; stock `/Game`/`/Engine` never moved; **only regenerated
assets** go under the recovery root; slug sanitized to `[A-Za-z0-9_-]`; traversal
/ path separators / `/Engine` roots / invalid package paths rejected.

**Key pieces:**
- `recovery_layout` / `bridge_recovery_layout` — resolve content + Saved paths.
  `map_name` inferred from the current map's short name (recovery suffixes
  `-Recovered-Editable`/`_Recovered_Editable`/`-Recovered`/`_Recovered` stripped
  — *inference only*) when omitted.
- **Configurable root** (bridge is the single resolver, so bridge + MCP agree):
  default `/Game/RecoveredMaps` → `[MapForgeBridge] RecoveryRoot=` ini →
  `MAPFORGE_RECOVERY_ROOT` env (MCP forwards it to the bridge).
- `import_static_mesh` `destination` is **optional**: omit it + pass
  `map_name`/`category` (default `Geometry/StaticMeshes`; only canonical
  categories accepted) → `<root>/<slug>/<category>`. Explicit destination is
  byte-for-byte unchanged (backward compatible). Result echoes the resolved
  destination + full `recovery_layout`.
- `inspect_static_mesh_actors` — per actor: mesh-or-null, actor + component
  transform, hidden/visible, mobility, collision, material overrides by slot;
  excludes pending-kill; name/folder filters + offset/limit; summary counts
  (`total_static_mesh_actors`, `actors_null_mesh`, `unique_mesh_paths`,
  `actors_hidden`, `actors_unresolved_materials`). Read-only, no dirtying.
- `write_recovery_manifest` (MCP) — atomic JSON (tempfile + `os.replace`) to
  `…/Manifests/recovery_manifest.json`, never in Content; records source PAK/map,
  layout, imported assets, source SHA-256s, material substitutions, actor counts,
  warnings, UTC timestamp, engine version.
- `audit_static_mesh_actors` (MCP) — compares a live inspect against an
  `actors_manifest.json`: `missing_actors`, `extra_actors`, `null_mesh_actors`,
  `mesh_substitutions`, `transform_mismatches`, `missing_material_overrides`.
  Mutates nothing.

**MTL-sidecar staging (import correctness):** for a `.obj`, each `mtllib` sidecar
is staged next to the temp OBJ under its referenced name so every `usemtl` group
keeps its own material slot. Without it the FBX SDK reports 0 materials and
collapses to one slot (`FbxStaticMeshImport.cpp:297,325` — `MaterialCount =
Node->GetMaterialCount()` then the `MaterialCount==0` default-slot branch); slot
count is **independent of `import_materials`**. Absolute/`..` MTL paths are
rejected (warning); all staged temp files are deleted post-import.

---

## 5. Build, run, test

**Do NOT build/cook/launch — phantaci runs builds.** Hand him the command.

Build the editor target (CLI; **unset `VULKAN_SDK`** — the machine's Vulkan
1.4.x SDK breaks 4.15's VulkanRHI; empty → the engine's bundled 2016 headers
compile clean). Add `-NoUBTMakefiles` once if a new plugin/module isn't picked up.
```
env -u VULKAN_SDK "C:/UnrealTournament/Engine/Binaries/DotNET/UnrealBuildTool.exe" \
  UnrealTournamentEditor Win64 Development \
  -project="C:/UnrealTournament/UnrealTournament/UnrealTournament.uproject" -waitmutex
```

Tests (from `Plugins/LiandriMapForge`):
```
python Tools/mapgen/tests/test_recovery_offline.py     # editor-free, run any time
python Tools/mapgen/tests/bridge_smoke.py              # needs a running editor + bridge
```

**Two-tree operational gotcha:** phantaci **builds** in `C:\UnrealTournament` but
**runs the editor** from a separate tree at **`C:\LAEditorUT4`** (its own,
drifting `Content/`). The bridge's `/Game/` therefore resolves to the
**LAEditorUT4** Content tree, and `FPaths::GameSavedDir()` (used for recovery
`filesystem_root`) resolves under whichever tree the running editor lives in.
Mind this when reasoning about where recovery assets/manifests actually land on
disk vs. where the code is built.

Config knobs (per-project editor ini `[MapForgeBridge]`, or env):
`Port=` / `-MapForgePort=` / `MAPFORGE_BRIDGE_PORT` (default 8765);
`RecoveryRoot=` / `MAPFORGE_RECOVERY_ROOT` (default `/Game/RecoveredMaps`).

---

## 6. Hard-won 4.15 facts (don't re-derive)

- **T3D round-trip is the lever.** `.umap` is binary and there's no Python editor
  plugin in 4.15, so BSP+actors go through UnrealEd's T3D text. `edactPasteSelected
  (World, false,false,false, &T3D)` does import + all post-fixups from an in-memory
  string; the buffer **must** start `Begin Map` or `ULevelFactory` silently
  imports nothing (`EditorFactories.cpp:478`). Clear the component selection first.
- **Paste only resolves already-loaded objects** — hence `preload_assets`
  (`StaticLoadObject` + FGCObject root). Python `_asset_refs()` regex-harvests
  every `/Game/`+`/Engine/` ref (quoted or bare `Texture=/Game/...`), trimming
  `:subobject` suffixes. `import_t3d` auto-preloads and fails-fast on missing refs
  — which is also a useful **audit signal** (missing asset = incomplete import).
- No HTTP server module in 4.15 (arrives 4.20) → raw sockets + `FTicker` (not
  `FTSTicker`). `Build.cs` uses `ModuleRules(TargetInfo)`. UnrealEd has
  `UnrealEdSharedPCH.h` so `UseExplicitOrSharedPCHs` just works; **no UCLASSes in
  the module → no UHT.**
- `exec("BSP …")`/`exec("LIGHT …")` are deprecated no-ops in 4.15
  (`EditorServer.cpp:5598`). Use `MAP REBUILD ALLVISIBLE`, `FEditorBuildUtils`,
  `FEditorFileUtils::SaveLevel`.
- Socket semantics in this fork (`SocketsBSD.cpp`): `Recv` returns **true/0** for
  would-block, **FALSE** for orderly close/error. `Send` doesn't translate
  would-block — check `GetLastErrorCode()==SE_EWOULDBLOCK`.
- Static-mesh import: `.obj` rides the same `UFbxFactory` as `.fbx`
  (`FbxFactory.cpp:33`); dialog gated on `IsAutomatedImport()`; asset named via a
  temp-copy staged as `<name>.<ext>` under `FPaths::GameIntermediateDir()` (4.15
  name). `FPaths::GameSavedDir()` (not `ProjectSavedDir`) for Saved paths.
- Actor audit is safe read-only: `AActor::GetActorLabel() const` returns
  `const FString&` (no lazy-create → no dirty); `GetFolderPath`/`IsHiddenEd`,
  `UStaticMeshComponent::GetStaticMesh`/`GetRelativeTransform`/`Mobility`/
  `GetCollisionEnabled`/`OverrideMaterials`, `EComponentMobility`/`ECollisionEnabled`.
- Real map world-bounds for true scale live in the **Postgres** `utstats` DB
  (`utstats_match.map_bounds`), not the sqlite Mods.db.

---

## 7. UT4X-Converter integration scope

**Target:** `github.com/xtremexp/UT4X-Converter` (Thomas "XtremeXp" P., v1.4.10,
Feb 2023). *All facts below were verified against the repo source/wiki — not
memory; two points where sources disagreed are flagged.*

### 7.1 What UT4X-Converter is
A **Java / Maven / JavaFX** desktop app that converts legacy Unreal/UT maps
(U1/UT99/UT2003-4/UT3/UDK) to a **UT4** map. It shells out to external tools —
**UModel** (`umodel_64.exe`), **SoX** (`sox.exe`), texture tools — extracts
assets, then post-processes them. It **explicitly stops at "manual operations
using Unreal Editor to get results"**: no editor automation, no verification.

Verified output under `~/Documents/UT4X-Converter/Converted/<MapName>/`:

| Output | Format | Folder |
|---|---|---|
| Level | `PersistentLevel.t3d` (full `Begin Map …`) | map root |
| Static meshes | **`.obj` + `.mtl`** (UModel `.pskx` → obj+mtl in `MapConverter.convertStaticMeshFiles()`) | `StaticMesh/` |
| Textures | `.tga` | `Texture/` |
| Sounds | `.wav` (UE1-2) / `.ogg` (UE3) | `Sound/` |

Documented manual import sequence: import TGAs → *Create Material*; import sounds
→ *Create Cue*; import the OBJ meshes ("Import all", Save); then **File → Import
Into Level** the `.t3d` → **Build**. Generated T3D references converted asset
paths (materials renamed `<Pkg>_<Group>_<Name>_Mat`). Asset-path convention: code
(`getUt4ReferenceBaseFolder`) says **`/Game/RestrictedAssets/Map/<Map>`**; an
older doc said **`/Game/Converted`** — **⚠ sources disagreed; confirm against a
live run.** Limitations (don't scope in): no blueprints/scripts, no shader
materials, non-lift movers unsupported.

### 7.2 Thesis
That manual sequence is **exactly** what the MapForge bridge automates, and the
recovery pipeline adds verification UT4X-Converter entirely lacks.

| UT4X-Converter step (manual today) | MapForge capability | Fit |
|---|---|---|
| Import `.obj`+`.mtl` meshes | `import_static_mesh` **+ MTL staging** + recovery layout | ✅ direct (MTL fix lands here) |
| Import sounds → Create Cue | `import_sound` (WAV) + `create_sound_cue` | ✅ for WAV; **OGG needs a decode step** (SoX ogg→wav) |
| File→Import Into Level `.t3d` + Build | `import_t3d` + `rebuild_geometry` | ✅ import_t3d expects `Begin Map` |
| Verify the result | `inspect_static_mesh_actors` + `audit_static_mesh_actors` | ✅ new capability UT4X has none of |
| Provenance / reproducibility | `write_recovery_manifest` | ✅ new |
| Import TGA → **Create Material** | *no verb* | ⚠ **Gap G3** |

### 7.3 Recommended seam
A **JSON "import-job" descriptor** as the contract — don't couple the apps
directly. UT4X-Converter (or a reader of its output folder) emits the descriptor
(mesh list, texture list, sound list, t3d path, target slug/root); a MapForge-side
**driver** executes it: import meshes (recovery layout) → [textures/materials] →
import sounds/cues → `import_t3d` → `rebuild_geometry` → `inspect`/`audit` →
`write_recovery_manifest`, feeding a gap report back.

```
UT4X-Converter ──▶ Converted/<Map>/{StaticMesh,Texture,Sound}/*, PersistentLevel.t3d (+ import_job.json)
        ▼
  MapForge driver ──MCP/bridge──▶ UE4.15 editor: meshes → [tex/mat] → sounds → import_t3d → rebuild
        ▲                                              │
        └──────── inspect + audit + manifest ◀─────────┘
```

Two homes for the glue (a **decision**):
- **(A) Python driver in `Tools/mapgen`** reading `Converted/<Map>/`. Fast, no
  upstream dependency, matches the "thin protocol / AI outside the editor" ethos.
  **Recommended for the MVP.**
- **(B) Native Java connector in UT4X-Converter** (`…export.MapForgeBridgeExporter`)
  speaking the JSON-lines socket. One app/UX, but a PR to a third-party GPL repo +
  a Java socket/JSON client. Better as a **later** native integration.

### 7.4 Scoped gaps (not yet built)
| # | Gap | For | Status |
|---|---|---|---|
| G1 | **Path reconciliation**: UT4X targets `/Game/RestrictedAssets/Map/<Map>` (mixes into the protected stock tree); MapForge quarantines to `/Game/RecoveredMaps/<slug>`. Either point `RecoveryRoot` at UT4X's root, or retarget into RecoveredMaps + **rewrite the T3D asset refs** (recommended). | MVP | open |
| G2 | **Output-folder reader** → import-job descriptor | MVP | open |
| G3 | **Texture + material import** verb(s): `import_texture` (TGA→UTexture2D) + minimal material create (Masked/TwoSided) | Phase 2 | open |
| G4 | **Sound import** | Phase 3 | **✅ done** (`import_sound` WAV + `create_sound_cue`); OGG→WAV decode still needed for UE3 sources |
| G5 | Populate manifest provenance from the converter (source PAK/map, UModel ver) | MVP+ | partial (manifest fields exist) |

Until G3, Phase-1 geometry imports with **default/missing materials** — which the
audit correctly flags as `actors_unresolved_materials`. Acceptable for an MVP.

### 7.5 Phased plan
- **Phase 0 — Spike (½ day):** run one real conversion; point `import_static_mesh`
  at its `StaticMesh/*.obj`, `import_t3d` the level, `rebuild`, `audit`. **Confirms
  engine/T3D/OBJ parity before any glue.** No code — existing tools only.
- **Phase 1 — Geometry+mesh MVP:** reader (G2) + path reconciliation (G1) + Python
  driver (A) → automated mesh + level import + audit + manifest. Materials deferred.
- **Phase 2 — Textures/materials (G3):** full visual parity.
- **Phase 3 — Sounds:** wire `import_sound`/`create_sound_cue` (+ OGG decode).
- **Phase 4 — Native Java connector (B):** optional upstream.

### 7.6 Risks / open decisions
- **Engine parity (verify in Phase 0):** UT4X says "UT4 (2015)" but doesn't pin
  the UE4 version; MapForge is the **4.15 fork**. Confirm same editor build / T3D
  dialect.
- **`/Game/RestrictedAssets/Map` vs `/Game/Converted`:** sources disagreed —
  confirm the real target root; it drives G1.
- **RestrictedAssets philosophy:** MapForge deliberately avoids RestrictedAssets
  (rule: never move stock/RestrictedAssets). Retargeting into RecoveredMaps (with
  T3D rewrite) keeps converted content out of the protected tree — recommended.
- **Upstream/licensing:** Option A needs no upstream changes; Option B is a PR to
  a third-party GPL repo — check the license first.
- **UModel OBJ quality** (pivots/scale/smoothing/multi-material): the
  multi-material slot problem is already solved by the MTL fix; the rest is
  UT4X-Converter's domain.

**Decisions needed from phantaci:** (1) glue home — Python (A, rec.) vs Java (B);
(2) asset root — RecoveredMaps + T3D rewrite (rec.) vs match RestrictedAssets/Map;
(3) MVP scope — geometry+meshes+audit first vs wait for materials.

---

## 8. Clutch map build (CL-Crucible) — creative next task

Build a **Clutch (CL)** map through the editor connector. Clutch is NetcodePlus's
ShootMania-*Elite* mode. A full copy-paste build prompt was drafted in the
2026-07-15 session (ask the assistant to re-emit "the CL-Crucible build prompt");
the essentials are captured here so the task survives a fresh session.

**Authoritative mechanics** (`Plugins/NetcodePlus/Source/{Public,Private}/ClutchGameMode.{h,cpp}`,
`ClutchRoundState.h`):
- **3v3, one attacker per round** (sniper/rail, dies in **3** defender hits ≈300hp)
  vs **3 defenders** (rockets); teams alternate attacking.
- **60s** round, pole **locked the first 45s** (`PoleUnlockDelay`), then **7s**
  uncontested to capture; capture cylinder **radius 180uu / half-height 160uu**.
- Objective = one central actor **tagged `ClutchPole`** (an FName tag —
  `place_static_mesh` can't set tags, so author the pole as a **T3D
  `StaticMeshActor` with `Tags(0)="ClutchPole"`**). `bUseRecoveredPoleVisual=true`
  may spawn its own pole visual — watch for a double-up at playtest.
- Win priority: timeout→defenders, all-defenders-dead→attacker, attacker-dead→
  defenders, pole→attacker.

**Design brief (constraints):**
- **Point-symmetric** (either team attacks fairly) but not boring — asymmetric
  routes inside the symmetry. **No flat rectangle.**
- **Scale = CTF-Quick** (measure it in-editor; ~16k×11k uu working target — it's
  compact/symmetrical, not a giant field).
- **Cover-dense à la FR-Meltdown** (a UT4 Flag-Run map — attacker-vs-defender, like
  Clutch): staggered chest-high peek-cover, densest **around the pole** (defenders
  hold it vs a one-shot sniper); long sightlines **broken by pillars** (sniper
  viable, never a gallery — Elite "peek-and-cover").
- **Verticality + flanks:** ramps, a ±Y catwalk/trench pair, 2–3 jump pads, two
  contestable sniper perches. The lone attacker needs repositioning vs 3.
- **Materials = deck + solo:** deck (Liandri concrete/orange) base shell + floors;
  solo (Shell tech-grey) for cover / high-ground / the dais (readability); orange
  trim for orientation. Optional `SlimePit` sludge in the central moat.
- **Pole mesh:** `SM_DarkHell_Pole`, object path
  `/NetcodePlus/Clutch/Objective/SM_DarkHell_Pole.SM_DarkHell_Pole` (plugin content
  mounts as `/NetcodePlus/…`, **not** `/Game/…` — verify with `preload_assets`).
- **Spawns:** `AUTTeamPlayerStart`, TeamNum 0/1, 3 per bastion.

**Build path (connector):** geometry as additive BSP — MapSpec + `emit.py`
`box_brush`/ramp T3D → `bridge_import_t3d` + `bridge_rebuild_geometry`; pole +
spawns + lights as tagged T3D actors; cover props via `bridge_place_static_mesh`;
verify with `bridge_inspect_static_mesh_actors` (no null meshes); don't save the
level unless asked. Creative refs: [ShootMania Elite](https://liquipedia.net/arenafps/Shootmania_Storm),
[FR-Meltdown](https://unreal.fandom.com/wiki/FR-Meltdown), [CTF-Quick](https://unreal.fandom.com/wiki/CTF-Quick).

---

## 9. Current state & next steps

**Committed & pushed** (`80b8272` on `main`, `github.com/jmortley/LiandriMapForge`) **and
built** — `Binaries/Win64/UE4Editor-MapForgeBridge.dll` (2026-07-14 21:30) is newer than
every source input, so it contains everything below. Shipped as release
[`v0.2`](https://github.com/jmortley/LiandriMapForge/releases/tag/v0.2) (editor DLL only;
`Binaries/` is gitignored). **The live smoke test is still unconfirmed:**
- Static-mesh import/configure/place + `exec` hardening.
- **MTL-sidecar staging** for multi-material OBJs (+ `cube_two_materials` &
  `traversal_mtllib` fixtures).
- **Map-scoped recovery layout + read-only actor audit** (`recovery_layout`,
  `inspect_static_mesh_actors`, optional map-scoped import destination,
  `write_recovery_manifest`, `audit_static_mesh_actors`, offline tests).
- **Sound import**: `import_sound` (WAV) + `create_sound_cue`.

Prior baseline: bridge v2 (`1b2e1ec`: audit hardening + physics-asset forge +
Blueprint Tiers 1–2).

**Immediate next steps (LA-pickup order):**
1. Build the editor (§5) + restart + run both suites — `test_recovery_offline.py`
   is editor-free (passes now); `bridge_smoke.py` needs the running editor.
2. Re-run the 14-material **Andok acceptance OBJ**; confirm all 14 slots.
3. **Build the Clutch map** (§8) via the connector — the fun one.
4. **UT4X-Converter Phase 0 spike** (§7.5) against a real `Converted/<Map>/` folder;
   then resolve the three integration decisions (§7.6) and start Phase 1.

---

## 10. References

- **Plugin:** `Plugins/LiandriMapForge` — repo `github.com/jmortley/LiandriMapForge` (`main`).
- **Docs:** `README.md` (reference), `NEXT.md` (continuation prompt / work log),
  `Tools/mapgen/capture/fixtures/README.md` (fixture capture guidance).
- **UT4X-Converter:** `github.com/xtremexp/UT4X-Converter` —
  [`MapConverter.java`](https://github.com/xtremexp/UT4X-Converter/blob/master/src/main/java/org/xtx/ut4converter/MapConverter.java),
  [`UModelExporter.java`](https://github.com/xtremexp/UT4X-Converter/blob/master/src/main/java/org/xtx/ut4converter/export/UModelExporter.java),
  [wiki: Conversion to UT4](https://github.com/xtremexp/UT4X-Converter/wiki/Conversion-to-UT4).
- **Assistant memory** (persists across sessions):
  `C:\Users\MrJmo\.claude\projects\C--UnrealTournament-UnrealTournament\memory\` —
  `mapforge-project.md`, `ut4x-converter-integration.md`, `laeditorut4-two-tree-setup.md`,
  `ut4-cli-build-vulkan-gotcha.md`, `dont-run-builds.md`, `ut4stats-map-bounds.md`.
- **Clutch mode (for §8):** `Plugins/NetcodePlus/Source/{Public,Private}/ClutchGameMode.{h,cpp}`,
  `ClutchRoundState.h`; pole asset
  `Plugins/NetcodePlus/Content/Clutch/Objective/SM_DarkHell_Pole.uasset`
  (mounts as `/NetcodePlus/Clutch/Objective/SM_DarkHell_Pole`).
- **Related projects:** NetcodePlus (sibling plugin), UTVehicles
  (`Plugins/UTVehicles`, repo `jmortley/UT4Vehicles`).
