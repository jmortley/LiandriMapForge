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
SlateRemote pattern), because editor APIs are game-thread-only.

| Verb | What it does |
|---|---|
| `ping` / `status` | engine version, current map, dirty flag, actor count, build-running flag |
| `preload_assets` | `StaticLoadObject` each path — **this retires the material-preloader hack** and makes BP actors (lifts, pickups), sounds and damage types resolve on a fresh level |
| `import_t3d` | `edactPasteSelected` with an in-memory buffer (no clipboard) + the editor's own post-import fixups; returns created actors |
| `export_t3d` | whole level (or selection) back as T3D — the read-back half of the loop |
| `rebuild_geometry` | `MAP REBUILD ALLVISIBLE` (the real CSG path; `BSP`/`LIGHT` exec are deprecated no-ops in 4.15) |
| `build` | lighting / geometry / paths / all via `FEditorBuildUtils`; async — poll `status` |
| `save_level` | `FEditorFileUtils::SaveLevel`; pass `filename` for never-saved levels |
| `set_surface_material` | direct `Surfs[i].Material` + `polyUpdateMaster` (selection-independent, survives CSG rebuild); filter by brush name and/or face normal |
| `list_actors` / `delete_actors` | query + clear for iterative re-import |
| `exec` | raw `GEditor->Exec` escape hatch, output captured |

The MCP proxy exposes these as `bridge_*` tools plus **`forge_mapspec`** — the
one-shot MapSpec → preload → import → CSG rebuild loop, no paste, no preloader
cubes (`emit.emit(..., loaders=False)`).

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
