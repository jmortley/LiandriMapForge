# LiandriMapForge — continuation prompt

Paste this to continue in a fresh session.

---

You're continuing **LiandriMapForge**, an AI map-generation tool for **UnrealTournament
(UE4 4.15 fork)**, living in its own git repo at `Plugins/LiandriMapForge`
(github.com/jmortley/LiandriMapForge), built by **phantaci**. It is a **standalone
editor-only** effort — nothing ships in the game runtime.

## What it does (all working + committed)
Pipeline: **layout PNG + real map bounds → MapSpec (JSON) → T3D → paste into UnrealEd.**
- `Tools/mapgen/trace.py` — pixel-traces an elimplus layout PNG to a MapSpec; `--world-width`
  scales to real size (the PNG is the 1024px UTHUD minimap of the NavMesh bounds).
- `Tools/mapgen/emit.py` — MapSpec → UnrealEd T3D. Handles: BSP box brushes (verified grammar),
  doorway gaps, PlayerStarts, point/directional/sky lights (**always-on Sun+Sky+AtmosphericFog
  rig** so maps are never dark), Blueprint pickups (health/armor), StaticMeshActors
  (ShellResources kit), `BaseJumpPad_C`, and **art profiles** (`--profile solo|deck`).
- `Tools/mapgen/make_deckstyle.py` — hand-authored **multi-level** builder (lower arena + twin
  toxic-sludge channels, upper catwalk ring, stairs + jump pads), at real UT4 scale.
- Examples in `Tools/mapgen/examples/`; ground-truth captures in `Tools/mapgen/capture/`
  (`solo.t3d`, `deck.t3d` are gitignored reference exports of real maps).

## Hard-won facts (don't re-derive)
- **No Python editor plugin** in this 4.15 fork; `.umap` is binary. The lever is UnrealEd's
  **T3D text round-trip** (verified: `EditorExporters.cpp:295`, `EditorObject.cpp:293/641`,
  `Brush.h:82`, `Brush.cpp:38`).
- **Pasted BSP only resolves already-loaded materials** → a fresh level shows checker. `emit.py`
  works around it with tiny hidden **material-preloader** meshes emitted before the brushes.
- **Real map bounds** (for true scale) live in the **postgres `utstats` DB**, table
  `utstats_match`, `map_bounds` JSONField (NOT the sqlite Mods.db). `uu_per_cell = (max_x-min_x)/cells`.
  See memory [[ut4stats-map-bounds]].
- **Art profiles**: `deck` = Liandri concrete + orange (`M_ConcreteWall`, `SlimePit` sludge);
  `solo` = ShellResources tech (`M_Shell_City_Wall_B`, `IND_Floor_A`). Mined from real map exports.
- Memory: [[mapforge-project]], [[ut4stats-map-bounds]], [[phantaci-identity]].

## Next task: Layer B — the editor bridge (the graduation)
Build a C++ **editor-only** module (its own module, NOT NetcodePlus's runtime one) that runs
inside UnrealEd and exposes an HTTP/socket endpoint, fronted by an MCP server (extend
`mapforge_mcp.py`). It should:
- import T3D (`ULevelFactory`), run editor commands (`GEditor->Exec` — verified
  `UnrealEdSrv.cpp:1961 Exec_Actor`, `EditorServer.cpp:664 Exec_Brush`), `SavePackage`, and Build.
- Retire the paste workflow, and fix what T3D-paste can't do cleanly:
  - **Recessed goo POOLS** with lethal `UTPainVolume` (serialization in `capture/deck.t3d`).
  - **Functional `Generic_Lift_C`** lifts (also in `deck.t3d`; better than reconstructed jump pads).
  - **Apply materials to BSP surfaces post-import** — retire the preloader hack.
- Note: NetcodePlus already depends on `Http` + `Json`; the bridge is a small **Editor**-type module.

**Structure recognition in a real map export** (tested on `capture/deck.t3d`): lifts are explicit
(`Generic_Lift_C` + `UTLiftExit` — read base/travel/exit directly); ramps are detectable
geometrically (BSP polys with a tilted normal, |Z| ~0.45–0.98); Deck's verticality is lifts, zero
jump pads. **The upshot for Layer B: lifts I can lift-and-shift from the real map (extract →
re-place as functional actors), ramps I can detect/author from slope, but "bridge" is a semantic
label that needs a geometry-graph analysis to earn — worth building if we want the bridge to
actually reason about a map's structure, not just its brushes.**

Scope check with phantaci before building; he drives, verify against this fork's source, and
keep the AI *outside* the editor (only translating intent → the editor's own command set).
