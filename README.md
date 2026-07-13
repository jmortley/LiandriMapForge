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
- [ ] Verticality + weapons — jump pads, lifts, `WeaponBase_C` + ammo (serialization captured from a real map)
- [ ] Materials — walls still default checker; wire real UT4 material paths
- [ ] **Layout → MapSpec vision step** (elimplus PNG → structured spec)
- [ ] MCP connector, layer B — C++ editor-only module (socket → `GEditor->Exec` + T3D import + build) fronted by an MCP proxy. The UE4.15 analog of Epic's UE5.8 editor MCP.

## Try the PoC

```
cd Tools/mapgen
python emit.py examples/DM-Box01.mapspec.json --out out/
```

Then in UnrealEd: new level → **Edit > Paste** `out/smoke_cube.t3d` (one box first).
If a solid cube appears, the pipeline is proven — paste `DM-Box01.geometry.t3d` for
the room shell. **Post-paste:** Build Geometry, then Build Lighting (else unlit).
