#!/usr/bin/env python3
"""
MapForge MCP connector (stateless layer).

Exposes the MapForge emitter as MCP tools, so any MCP client (Claude Code, Codex,
Claude Desktop) can turn a MapSpec into UT4 T3D directly -- the UT4 (UE4 4.15)
analog of Epic's UE5.8 editor MCP, for the engine Epic's tooling will never touch.

No editor required for this layer: the returned T3D is pasted via Edit > Paste (or
handed to the live-editor bridge -- layer B -- once that C++ module exists).

Setup:
    pip install mcp
Register with Claude Code (stdio):
    claude mcp add mapforge -- python "<abs path>/Tools/mapgen/mapforge_mcp.py"
"""
import datetime, hashlib, json, os, re, socket, tempfile
from mcp.server.fastmcp import FastMCP
import emit  # same directory; emit.py guards __main__ so import is side-effect-free

HERE = os.path.dirname(os.path.abspath(__file__))
mcp = FastMCP("mapforge")

# Optional recovery-root override for this MCP layer. When set, it is forwarded
# to the bridge's recovery_layout resolver so the two agree on the resolved root
# (the bridge is the single source of truth; [MapForgeBridge] RecoveryRoot= is
# its ini-level override, this env var is the MCP-level one).
RECOVERY_ROOT = os.environ.get("MAPFORGE_RECOVERY_ROOT", "").strip()

# ---- Layer B: live editor bridge (MapForgeBridge C++ module) -----------------
# UnrealEd (with the LiandriMapForge plugin) listens on localhost with a
# JSON-lines protocol: one {"id","cmd","args"} object per line, one
# {"id","ok","result"|"error"} object back. Override the port with the
# MAPFORGE_BRIDGE_PORT env var, [MapForgeBridge] Port= in the editor ini,
# or -MapForgePort= on the editor command line.

BRIDGE_HOST = "127.0.0.1"
BRIDGE_PORT = int(os.environ.get("MAPFORGE_BRIDGE_PORT", "8765"))


class BridgeError(RuntimeError):
    """The editor bridge is unreachable, or refused/failed a command."""


_MAX_RESPONSE_BYTES = 256 * 1024 * 1024


def _bridge_call(cmd, args=None, timeout=600.0):
    """One request/response round-trip with the in-editor bridge. Raises
    BridgeError with the failing phase for every transport problem."""
    request = json.dumps({"id": 1, "cmd": cmd, "args": args or {}})
    try:
        sock = socket.create_connection((BRIDGE_HOST, BRIDGE_PORT), timeout=10.0)
    except OSError as e:
        raise BridgeError(
            "MapForge bridge not reachable on %s:%d -- is UnrealEd running with "
            "the LiandriMapForge plugin enabled? (%s)" % (BRIDGE_HOST, BRIDGE_PORT, e))
    try:
        sock.settimeout(timeout)
        try:
            sock.sendall(request.encode("utf-8") + b"\n")
        except OSError as e:
            raise BridgeError("send failed for '%s': %s" % (cmd, e))
        buf = bytearray()
        while not buf.endswith(b"\n"):
            try:
                chunk = sock.recv(1 << 16)
            except socket.timeout:
                raise BridgeError(
                    "'%s' timed out after %.0fs (editor busy in a modal task?)" % (cmd, timeout))
            except OSError as e:
                raise BridgeError("receive failed for '%s': %s" % (cmd, e))
            if not chunk:
                raise BridgeError("bridge closed the connection mid-response to '%s'" % cmd)
            buf += chunk
            if len(buf) > _MAX_RESPONSE_BYTES:
                raise BridgeError("'%s' response exceeded %d bytes" % (cmd, _MAX_RESPONSE_BYTES))
    finally:
        sock.close()
    try:
        response = json.loads(buf.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as e:
        raise BridgeError("malformed response to '%s': %s" % (cmd, e))
    if not response.get("ok"):
        raise BridgeError(response.get("error", "unknown bridge error"))
    return response.get("result")


_ASSET_REF = re.compile(r"'(/(?:Game|Engine)/[^']+)'")
# BSP polygons reference materials UNQUOTED (Texture=/Game/... -- emit.py and
# real map exports alike); TextureStreamingData carries bare paths too.
_ASSET_REF_BARE = re.compile(r"=(/(?:Game|Engine)/[^\s'\",)]+)")


def _asset_refs(t3d):
    """Every /Game/ and /Engine/ object path a T3D document references,
    quoted (Class'/Game/...') or bare (Texture=/Game/...). Subobject refs
    (Pkg.Obj:Sub) are trimmed to Pkg.Obj -- loading the outer object is what
    pins the package."""
    refs = set(_ASSET_REF.findall(t3d)) | set(_ASSET_REF_BARE.findall(t3d))
    return sorted({m.split(":", 1)[0] for m in refs})


def _import_t3d(t3d, preload=True, require_all_assets=True):
    preloaded = {"loaded": 0, "failed": []}
    if preload:
        refs = _asset_refs(t3d)
        if refs:
            preloaded = _bridge_call("preload_assets", {"paths": refs})
        # Fail fast: importing with unresolved refs silently drops properties
        # or whole actors, then reports success.
        if preloaded.get("failed") and require_all_assets:
            raise BridgeError("assets failed to load, aborting import: %s"
                              % ", ".join(preloaded["failed"]))
    result = _bridge_call("import_t3d", {"t3d": t3d})
    if preload:
        result["preload"] = preloaded
    return result


@mcp.tool()
def list_pickups() -> list:
    """List the pickup 'type' values a MapSpec may use (verified health/armor BPs)."""
    return sorted(emit.PICKUPS.keys())


@mcp.tool()
def mapspec_schema() -> str:
    """Return the MapSpec JSON schema -- the authoring contract for a UT4 arena."""
    with open(os.path.join(HERE, "mapspec.schema.json"), encoding="utf-8") as f:
        return f.read()


@mcp.tool()
def generate_t3d(mapspec, include_actors: bool = True) -> str:
    """Compile a MapSpec (object matching mapspec.schema.json, or its JSON string)
    into UnrealEd T3D for UT4 (UE4 4.15). Paste the result into UnrealEd via
    Edit > Paste, then Build Geometry. Returns the full T3D document."""
    if isinstance(mapspec, str):
        mapspec = json.loads(mapspec)
    return emit.emit(mapspec, actors=include_actors)


@mcp.tool()
def smoke_cube() -> str:
    """Return a one-brush additive box T3D -- the paste round-trip smoke test."""
    return emit.emit_smoke_cube()


# ---- Layer B tools: drive the live editor ------------------------------------

@mcp.tool()
def bridge_status() -> dict:
    """Ping the live UnrealEd bridge: engine version, current map, dirty flag,
    level lock, actor count, PIE state, and build flags. Poll
    'lighting_building' for Lightmass completion ('editor_building' tracks
    other FEditorBuildUtils builds; 'building' is the union)."""
    return _bridge_call("ping")


@mcp.tool()
def bridge_import_t3d(t3d: str, preload: bool = True,
                      require_all_assets: bool = True) -> dict:
    """Import a T3D document into the level open in UnrealEd. First preloads
    every /Game/ and /Engine/ asset the T3D references (quoted or bare), so
    materials, Blueprint actors (lifts, pickups), sounds and damage types all
    resolve on a fresh level -- no preloader-cube hack. Errors if any required
    asset fails to load (set require_all_assets=False to import anyway) or if
    the import creates no actors. Follow with bridge_rebuild_geometry()."""
    return _import_t3d(t3d, preload=preload, require_all_assets=require_all_assets)


@mcp.tool()
def forge_mapspec(mapspec, rebuild: bool = True, include_actors: bool = True) -> dict:
    """One-shot MapSpec -> live geometry: compile to T3D (no preloader meshes),
    preload referenced assets, import into the open level, and rebuild BSP.
    The full author->geometry loop with no paste step."""
    if isinstance(mapspec, str):
        mapspec = json.loads(mapspec)
    t3d = emit.emit(mapspec, actors=include_actors, loaders=False)
    result = _import_t3d(t3d, preload=True)
    if rebuild:
        _bridge_call("rebuild_geometry")
        result["rebuilt"] = True
    return result


@mcp.tool()
def bridge_rebuild_geometry() -> dict:
    """Rebuild BSP/CSG for all visible levels (the editor's MAP REBUILD
    ALLVISIBLE). Run after importing brushes."""
    return _bridge_call("rebuild_geometry")


@mcp.tool()
def bridge_build(what: str = "lighting") -> dict:
    """Kick an editor build: 'lighting', 'geometry', 'paths' (AI/nav), or
    'all'. Lighting and nav complete asynchronously -- poll bridge_status()
    until building=false."""
    return _bridge_call("build", {"what": what})


@mcp.tool()
def bridge_save(filename: str = "", allow_dialog: bool = False) -> dict:
    """Save the current level. A never-saved level needs an explicit
    'filename'; without one the bridge fails fast instead of blocking on a
    modal Save-As dialog (pass allow_dialog=True to permit the dialog)."""
    args = {"allow_dialog": allow_dialog}
    if filename:
        args["filename"] = filename
    return _bridge_call("save_level", args)


@mcp.tool()
def bridge_exec(command: str) -> dict:
    """Run a raw UnrealEd console command (ACTOR / MAP / POLY families) and
    return its captured output. Escape hatch for anything the bridge hasn't
    wrapped. Note: BSP and LIGHT exec commands are deprecated no-ops in this
    engine version."""
    return _bridge_call("exec", {"command": command})


@mcp.tool()
def bridge_export_t3d(selected_only: bool = False, save_to: str = "") -> dict:
    """Export the current level (or just the selected actors) as T3D -- the
    read-back half of the author->build->inspect loop. Whole-map exports are
    huge; pass 'save_to' (absolute path) to write to disk and return only the
    path + length instead of the full text."""
    result = _bridge_call("export_t3d", {"selected_only": selected_only})
    if save_to:
        # newline="" -- the T3D already carries \r\n; text-mode translation
        # would double it to \r\r\n
        with open(save_to, "w", encoding="utf-8", newline="") as f:
            f.write(result.pop("t3d"))
        result["saved_to"] = save_to
    return result


@mcp.tool()
def bridge_list_actors(name_contains: str = "", class_contains: str = "",
                       limit: int = 200, all_levels: bool = False) -> dict:
    """List actors in the current level (name, label, class, location),
    filtered by case-insensitive substrings of name/label and/or class.
    all_levels=True widens to every loaded streaming level (entries then
    include their level)."""
    return _bridge_call("list_actors", {
        "name_contains": name_contains,
        "class_contains": class_contains,
        "limit": limit,
        "all_levels": all_levels,
    })


@mcp.tool()
def bridge_delete_actors(names: list) -> dict:
    """Delete actors in the CURRENT level by name or label (case-insensitive
    exact match; actor names are only unique per level). Builder brush and
    WorldSettings are always protected, locked levels are rejected, and the
    deletion is a single undoable transaction."""
    return _bridge_call("delete_actors", {"names": names})


@mcp.tool()
def bridge_set_surface_material(material: str, brush: str = "",
                                normal: list = None, tolerance: float = 0.1) -> dict:
    """Apply a material to BSP surfaces post-import (selection-independent;
    survives CSG rebuild). Filter by 'brush' (substring of the source brush
    actor's name/label) and/or 'normal' ([x,y,z]; dot-product tolerance).
    E.g. normal=[0,0,1] retextures floors, [0,0,-1] ceilings."""
    args = {"material": material, "brush": brush, "tolerance": tolerance}
    if normal:
        args["normal"] = normal
    return _bridge_call("set_surface_material", args)


@mcp.tool()
def forge_chassis_physasset(mesh: str, bone: str = "", target: str = "") -> dict:
    """Forge a clean chassis-only UPhysicsAsset next to a skeletal mesh, so a
    vehicle can opt into it via PhysicsAssetOverride. Duplicates the mesh's
    default physics asset (never modifies the source or the mesh), keeps only
    the chosen bone's body, drops every other body + all constraints, and
    saves the result.

    mesh:   skeletal mesh package path, e.g.
            /Game/RestrictedAssets/Proto/UT3_Vehicles/VH_Scorpion/Meshes/SK_VH_Scorpion_001
            (bare package path or full Package.Asset object path both work).
    bone:   bone whose body to keep; default = the mesh's root bone (index 0).
            Errors listing the available body bones if this bone has no body.
    target: package path for the new asset; default = "<mesh>_Physics".
            Re-running overwrites the target cleanly.

    Returns {asset, kept_bone, removed_bodies, removed_constraints, saved}."""
    args = {"mesh": mesh}
    if bone:
        args["bone"] = bone
    if target:
        args["target"] = target
    return _bridge_call("forge_chassis_physasset", args)


@mcp.tool()
def bridge_create_blueprint(parent: str, package: str, name: str = "",
                            overwrite: bool = False) -> dict:
    """Create a Blueprint subclass in the open editor, compile, and save.
    'parent' is a bare native class name ("UTMutator"), a full class path
    ("/Script/UnrealTournament.UTMutator"), or a Blueprint package path (whose
    generated class becomes the parent). 'package' is the target asset path
    (e.g. "/Game/Mods/Mutators/Mutator_Foo"); 'name' overrides the leaf.
    AUTMutator is Blueprintable; AUTGameMode is Blueprintable by inheritance.
    Returns {asset, generated_class, parent, saved}."""
    args = {"parent": parent, "package": package, "overwrite": overwrite}
    if name:
        args["name"] = name
    return _bridge_call("create_blueprint", args)


@mcp.tool()
def bridge_set_class_defaults(asset: str, defaults: dict, component: str = "") -> dict:
    """Override default (CDO) property values on a Blueprint class and save --
    the Tier-1 'config variant' path (no graph logic). 'defaults' maps property
    name -> value; bools/numbers are converted, and strings pass through as UE
    property literals (e.g. "(X=1,Y=2,Z=3)" for a struct, an enum name, or a
    class path). Inherited native properties of the parent mutator/gamemode
    (bForceRespawn, TimeLimit, GoalScore, ...) are valid targets. Optional
    'component' names an object-pointer property on the class whose default
    SUBOBJECT receives the defaults instead -- e.g. component="Mesh",
    defaults={"SkeletalMesh": "/Game/.../mesh.mesh"} repoints a character
    content BP's mesh component. Returns {applied, failed, saved}."""
    args = {"asset": asset, "defaults": defaults}
    if component:
        args["component"] = component
    return _bridge_call("set_class_defaults", args)


@mcp.tool()
def forge_bp_variant(parent: str, package: str, defaults: dict = None,
                     name: str = "", overwrite: bool = False) -> dict:
    """One-shot Tier-1 authoring: create a Blueprint subclass then set its CDO
    defaults in a single call -- the config-variant mutator/gamemode workflow.
    Returns the create result plus a 'defaults' field with the apply outcome."""
    created = bridge_create_blueprint(parent, package, name=name, overwrite=overwrite)
    if defaults:
        created["defaults"] = bridge_set_class_defaults(created["asset"], defaults)
    return created


# ---- Tier 2: Blueprint graph authoring (clipboard-text round-trip) -----------

_BPGC_REF = re.compile(r"BlueprintGeneratedClass'([^']+)'")


def rehome_graph_text(text, new_bpgc, old_bpgc=None):
    """Repoint self/owning-class references in exported graph text to a new
    BlueprintGeneratedClass path, so a graph captured from one Blueprint imports
    cleanly into another. With old_bpgc, only that path is rewritten; otherwise
    every BlueprintGeneratedClass ref is repointed to new_bpgc (the single-source
    fixture case). Self-context variable/event refs carry no class path -- they
    resolve against the target BP's own members, which is why those variables
    must be added (bridge_add_variable) before import."""
    if old_bpgc:
        return text.replace("BlueprintGeneratedClass'%s'" % old_bpgc,
                            "BlueprintGeneratedClass'%s'" % new_bpgc)
    return _BPGC_REF.sub("BlueprintGeneratedClass'%s'" % new_bpgc, text)


@mcp.tool()
def bridge_add_variable(asset: str, name: str, type: str, default: str = "") -> dict:
    """Add a member variable to a Blueprint (needed before importing a graph
    that references it by self-context). 'type' is a scalar ("bool", "int",
    "float", "string", "name", "byte") or "object:<class>" / "class:<class>" /
    "struct:<path>", with an optional "[]" array suffix. Returns
    {variable, type, saved}."""
    return _bridge_call("add_variable", {
        "asset": asset, "name": name, "type": type, "default": default,
    })


@mcp.tool()
def bridge_import_graph(asset: str, t3d: str, graph: str = "") -> dict:
    """Import Blueprint graph nodes (clipboard-text T3D, e.g. from
    bridge_export_graph) into a Blueprint. 'graph' defaults to the event graph;
    name a function graph to target it. Runs the editor's post-paste fix-up.
    Follow with bridge_compile_blueprint to surface node errors. Returns
    {graph, count, nodes, saved}."""
    return _bridge_call("import_graph", {"asset": asset, "t3d": t3d, "graph": graph})


@mcp.tool()
def bridge_compile_blueprint(asset: str) -> dict:
    """Compile a Blueprint and save. Returns {status, ok, messages, saved};
    'messages' lists node-level compiler errors/warnings (node, error_type,
    message) -- import success is not validity, so always compile and read
    these before trusting a graph."""
    return _bridge_call("compile_blueprint", {"asset": asset})


@mcp.tool()
def bridge_reparent_blueprint(asset: str, new_parent: str) -> dict:
    """Reparent a Blueprint to a new parent class, recompile, and save.

    *** MUTATOR / GAMEMODE (no instanced default-subobjects) ONLY. ***
    HARD-REFUSED (ok=false) for UTWeapon/UTProjectile-derived BPs: a headless
    recompile reinstances their CreateDefaultSubobject state machine
    (StateActive/StateEquipping/...) with a broken Outer -> fatal "created in
    Package instead of <Class>" assert that crashed a live editor 2026-07-21.
    Reparent those in the editor UI (Class Settings -> Parent Class) instead.

    'new_parent' resolves like create_blueprint's parent: a native class name
    ("AUTMutator"), a /Script/ path, or a BP asset path (uses its generated
    class); abstract native parents are valid. Refuses inheritance cycles.
    Returns {asset, old_parent, new_parent, changed, status, ok, messages,
    saved}. Property values that existed only on the OLD parent chain are
    dropped by the recompile -- OBJ DUMP before/after when parents' defaults
    diverge."""
    return _bridge_call("reparent_blueprint", {"asset": asset, "new_parent": new_parent})


@mcp.tool()
def bridge_export_graph(asset: str, graph: str = "", save_to: str = "") -> dict:
    """Export a Blueprint graph as clipboard-text T3D -- the read-back half of
    the loop and the way to capture graph fixtures from a real Blueprint.
    'graph' defaults to the event graph. Pass 'save_to' (absolute path) to write
    the text to disk and return path + length instead of the full body."""
    result = _bridge_call("export_graph", {"asset": asset, "graph": graph})
    if save_to:
        with open(save_to, "w", encoding="utf-8", newline="") as f:
            f.write(result.pop("t3d"))
        result["saved_to"] = save_to
    return result


@mcp.tool()
def forge_bp_graph(parent: str, package: str, graph_t3d: str, variables: list = None,
                   rehome: bool = True, name: str = "", overwrite: bool = False) -> dict:
    """Full Tier-2 one-shot: create a Blueprint subclass, add member variables,
    import an event graph (clipboard-text), and compile -- returning each step's
    result. 'variables' is a list of {name, type, default?} added before import
    (so self-context references resolve). 'graph_t3d' is exported graph text;
    with rehome=True its owning-class references are repointed to the new class.
    Seed 'graph_t3d' from bridge_export_graph on a real Blueprint rather than
    hand-authoring node text."""
    created = bridge_create_blueprint(parent, package, name=name, overwrite=overwrite)
    asset = created["asset"]
    gen = created.get("generated_class") or ""
    added = []
    for v in (variables or []):
        added.append(bridge_add_variable(asset, v["name"], v["type"], v.get("default", "")))
    text = rehome_graph_text(graph_t3d, gen) if (rehome and gen) else graph_t3d
    imported = bridge_import_graph(asset, text)
    compiled = bridge_compile_blueprint(asset)
    return {"created": created, "variables": added, "imported": imported, "compiled": compiled}


# ---- Static mesh import / configure / place ---------------------------------

@mcp.tool()
def bridge_import_static_mesh(source: str, destination: str = "", name: str = "",
                              overwrite: bool = False, save: bool = True,
                              options: dict = None, map_name: str = "",
                              category: str = "", recovery_root: str = "") -> dict:
    """Import an OBJ or FBX file as a UStaticMesh into the open editor -- no
    modal dialog, no GEditor->Exec (uses the automated FBX/OBJ factory path).
    'source' is an absolute local .obj/.fbx path.

    Destination: pass an explicit 'destination' /Game folder (unchanged, backward
    compatible), OR leave it empty to route through the canonical map-scoped
    recovery layout: <RecoveryRoot>/<MapSlug>/<category>. 'map_name' selects the
    slug (inferred from the current map when omitted); 'category' is one of Maps,
    Geometry/BSP, Geometry/StaticMeshes (default), Materials, Textures, VFX,
    Audio, Blueprints, Data (arbitrary/traversal categories are rejected).
    e.g. map_name='DM-Andok_Scaled', category='Geometry/BSP' ->
    /Game/RecoveredMaps/DM-Andok_Scaled/Geometry/BSP/<name>.

    'name' the asset name (defaults to the file's base name). 'options' (all
    optional, with defaults): combine_meshes(true), import_materials(false),
    import_textures(false), generate_lightmap_uvs(true), compute_normals(true),
    use_mikk_tspace(false), collision ("none"|"simple"|"complex_as_simple"),
    lightmap_coordinate_index(1). For multi-material OBJs the referenced .mtl
    sidecar(s) are staged beside the OBJ so every usemtl group keeps its own
    material slot. Fails cleanly (no prompt) if the target exists and overwrite
    is false. Only .obj/.fbx are allowed. Returns asset/package path, class,
    source, resolved destination (+ recovery_layout when auto-resolved),
    created/overwritten, saved, mesh stats (material_slot_count,
    lod0_vertices/triangles, uv_channels, bounds, collision_trace_mode,
    simple_collision_primitives) and warnings. Does NOT save the current map."""
    args = {"source": source, "overwrite": overwrite, "save": save}
    if destination:
        args["destination"] = destination
    if name:
        args["name"] = name
    if options:
        args["options"] = options
    if map_name:
        args["map_name"] = map_name
    if category:
        args["category"] = category
    root = recovery_root or RECOVERY_ROOT
    if root:
        args["recovery_root"] = root
    return _bridge_call("import_static_mesh", args)


@mcp.tool()
def bridge_import_particle_t3d(source: str, destination: str, name: str = "",
                               materials: dict = None, overwrite: bool = False,
                               save: bool = True) -> dict:
    """Import a Cascade ParticleSystem exported as object-text T3D into the
    open editor. This is intentionally narrower than a generic object importer:
    the root must be ParticleSystem and every nested object must be a standard
    particle emitter/LOD/module/distribution (plus Cascade's curve editor data).
    'source' must be an absolute .t3d path; 'destination' is a /Game folder.
    'materials' maps legacy object paths to existing UT4 material paths, e.g.
    {"WP_RocketLauncher.Materials.M_FireTrail": "/Game/.../M_Rocket_TrailFire"}.
    Every legacy material reference must be mapped before import. Existing
    assets fail unless overwrite=True. Returns emitter names/counts, applied
    substitutions, and save state; it never saves the current map."""
    args = {
        "source": source,
        "destination": destination,
        "overwrite": overwrite,
        "save": save,
    }
    if name:
        args["name"] = name
    if materials:
        args["materials"] = materials
    return _bridge_call("import_particle_t3d", args)


@mcp.tool()
def bridge_configure_static_mesh(asset: str, collision: str = "",
                                 clear_simple_collision: bool = False,
                                 lightmap_coordinate_index: int = None,
                                 materials: dict = None, save: bool = False) -> dict:
    """Reconfigure an imported UStaticMesh. 'collision' sets the trace mode
    ("none"|"simple"|"complex_as_simple"); 'clear_simple_collision' strips
    auto-generated simple primitives; 'lightmap_coordinate_index' sets the
    lightmap UV channel (rebuilds the asset); 'materials' assigns material asset
    paths to explicit slot indices, e.g. {"0": "/Game/.../M_Foo"}. save=True
    writes the asset package. Returns updated mesh stats + warnings."""
    args = {"asset": asset, "save": save}
    if collision:
        args["collision"] = collision
    if clear_simple_collision:
        args["clear_simple_collision"] = True
    if lightmap_coordinate_index is not None:
        args["lightmap_coordinate_index"] = lightmap_coordinate_index
    if materials:
        args["materials"] = materials
    return _bridge_call("configure_static_mesh", args)


@mcp.tool()
def bridge_place_static_mesh(asset: str, location: list = None, rotation: list = None,
                             scale: list = None, label: str = "", folder: str = "") -> dict:
    """Spawn a StaticMeshActor for an imported mesh into the current editor
    level. 'location' [x,y,z], 'rotation' [pitch,yaw,roll], 'scale' [x,y,z] each
    default to identity (0/0/1). Optional actor 'label' and world-outliner
    'folder'. Undoable; marks the level dirty and redraws viewports, but does
    NOT save the level. Returns the actor name/label/path and final transform."""
    args = {"asset": asset}
    if location:
        args["location"] = location
    if rotation:
        args["rotation"] = rotation
    if scale:
        args["scale"] = scale
    if label:
        args["label"] = label
    if folder:
        args["folder"] = folder
    return _bridge_call("place_static_mesh", args)


# ---- SoundWave import / SoundCue authoring ----------------------------------

@mcp.tool()
def bridge_import_sound(source: str, destination: str, name: str = "",
                        overwrite: bool = False, save: bool = True,
                        looping: bool = False) -> dict:
    """Import one 16-bit mono/stereo PCM WAV as a SoundWave without opening an
    editor dialog. 'source' must be an absolute .wav path and 'destination' a
    /Game content folder. 'name' defaults to the WAV basename. 'looping' sets
    the SoundWave's native loop flag. Existing assets fail cleanly unless
    overwrite=true; overwrite suppresses UE4's modal overwrite prompt. Returns
    asset/package paths, duration, sample rate, channels, loop and save state.
    This imports only the wave; use bridge_create_sound_cue for cue graphs."""
    args = {
        "source": source,
        "destination": destination,
        "overwrite": overwrite,
        "save": save,
        "looping": looping,
    }
    if name:
        args["name"] = name
    return _bridge_call("import_sound", args)


@mcp.tool()
def bridge_create_sound_cue(destination: str, name: str, waves: list,
                            mode: str = "single", looping: bool = False,
                            volume: float = 0.75, pitch: float = 1.0,
                            modulator: dict = None, mixer_volumes: list = None,
                            random_without_replacement: bool = True,
                            overwrite: bool = False, save: bool = True) -> dict:
    """Create a modal-free SoundCue graph from imported SoundWave asset paths.
    mode='single' uses exactly one wave; 'random' selects one wave per play;
    'mixer' layers all waves and optionally accepts one mixer_volumes value per
    wave. looping marks every WavePlayer node as looping. Optional modulator keys
    are pitch_min, pitch_max, volume_min and volume_max. The cue-level volume and
    pitch multipliers are also configurable. Existing cues fail unless
    overwrite=true. Returns the cue path, resolved waves, mode and node count."""
    args = {
        "destination": destination,
        "name": name,
        "waves": waves,
        "mode": mode,
        "looping": looping,
        "volume": volume,
        "pitch": pitch,
        "random_without_replacement": random_without_replacement,
        "overwrite": overwrite,
        "save": save,
    }
    if modulator:
        args["modulator"] = modulator
    if mixer_volumes is not None:
        args["mixer_volumes"] = mixer_volumes
    return _bridge_call("create_sound_cue", args)


# ---- Map-scoped recovery layout / audit -------------------------------------

@mcp.tool()
def bridge_recovery_layout(map_name: str = "", recovery_root: str = "") -> dict:
    """Resolve the canonical map-scoped recovery layout (read-only). Returns the
    editable Unreal content paths under <RecoveryRoot>/<MapSlug>/ (content_root,
    maps, bsp, static_meshes, materials, textures, vfx, audio, blueprints, data)
    plus the on-disk non-content paths under Saved/LiandriMapForge/Recoveries/
    <MapSlug>/ (filesystem_root, raw_extract, interchange, manifests, reports).
    'map_name' is inferred from the current map (recovery suffixes stripped) when
    omitted. The root defaults to /Game/RecoveredMaps, overridable by the
    recovery_root arg, the MAPFORGE_RECOVERY_ROOT env var, or [MapForgeBridge]
    RecoveryRoot= in the editor ini."""
    args = {}
    if map_name:
        args["map_name"] = map_name
    root = recovery_root or RECOVERY_ROOT
    if root:
        args["recovery_root"] = root
    return _bridge_call("recovery_layout", args)


@mcp.tool()
def bridge_inspect_static_mesh_actors(name_contains: str = "", folder_contains: str = "",
                                      offset: int = 0, limit: int = 1000) -> dict:
    """Read-only audit of every StaticMeshActor in the current level. Per actor:
    name/label, path/folder, mesh object path (or null), actor + component
    relative transform, hidden/visible state, mobility, collision enabled state,
    and material overrides by slot. Pending-kill actors are excluded. Optional
    name/folder substring filters and offset/limit pagination. Also returns
    summary counts (total_static_mesh_actors, actors_null_mesh, unique_mesh_paths,
    actors_hidden, actors_unresolved_materials). Does NOT dirty the map."""
    return _bridge_call("inspect_static_mesh_actors", {
        "name_contains": name_contains, "folder_contains": folder_contains,
        "offset": offset, "limit": limit})


@mcp.tool()
def bridge_set_material_camera_fade(asset: str, fade_start: float = 180.0,
                                    fade_length: float = 120.0, save: bool = True) -> dict:
    """Inject a camera-distance attenuation into a BASE material's graph:
    every connected emissive/opacity/opacity-mask output is multiplied by
    clamp((distance(camera, object origin) - fade_start) / fade_length, 0, 1),
    so the material contributes nothing when the camera is within fade_start
    units of the object/decal origin. Built for screen-space decals whose
    projection box can swallow the camera (the bio-goo full-screen tint).
    Re-running with new values UPDATES the previously injected chain
    (marker-tagged) instead of stacking. save=True writes the .uasset.
    Requires a UMaterial (not an instance). Returns applied inputs + counts."""
    return _bridge_call("set_material_camera_fade", {
        "asset": asset, "fade_start": fade_start, "fade_length": fade_length, "save": save})


@mcp.tool()
def bridge_duplicate_asset(source: str, target: str, save: bool = True) -> dict:
    """Duplicate any asset (mesh, material instance, sound, ...) into a new
    package via StaticDuplicateObject. 'source' is a package or object path;
    'target' is the new long package path (e.g. /Game/A/B_Bright -- the object
    takes the package's short name). Overwrites an existing object of that name
    in the target package. save=True (default) writes the new .uasset to disk.
    Returns {asset, class, source, saved}."""
    return _bridge_call("duplicate_asset", {"source": source, "target": target, "save": save})


@mcp.tool()
def bridge_configure_skeletal_mesh(asset: str, materials: dict = None, save: bool = True) -> dict:
    """Inspect or reassign a USkeletalMesh's material slots. With no
    'materials' this is a read-only slot listing (index, slot_name, material)
    and never dirties the asset. 'materials' maps slot index -> material object
    path, e.g. {"0": "/Game/.../MI_Bright_0"}; all slots/paths are validated
    before anything is applied. save=True (default) writes the mesh package.
    Returns {asset, slot_count, slots, assigned, saved}."""
    args = {"asset": asset, "save": save}
    if materials:
        args["materials"] = materials
    return _bridge_call("configure_skeletal_mesh", args)


@mcp.tool()
def bridge_inspect_material(asset: str) -> dict:
    """Read-only inspect of a material or material instance: class, parent,
    root master material, the instance's current scalar/vector/texture
    parameter overrides, and every scalar/vector parameter the root master
    exposes with the value the queried asset actually resolves to (through
    the parent chain). Accepts a package path (/Game/A/M_Foo) or object path
    (/Game/A/M_Foo.M_Foo). Params defined inside material functions are not
    enumerable in 4.15 but can still be set. Never dirties anything."""
    return _bridge_call("inspect_material", {"asset": asset})


@mcp.tool()
def bridge_set_material_params(asset: str, scalars: dict = None,
                               vectors: dict = None, save: bool = True) -> dict:
    """Set parameter overrides on a UMaterialInstanceConstant asset (persists,
    unlike runtime MID edits). 'scalars' maps parameter name -> float;
    'vectors' maps name -> [r,g,b] or [r,g,b,a] linear floats (HDR values > 1
    allowed -- useful to brighten dark albedo via tint params). Names not
    exposed by the root master are still applied but flagged in warnings.
    save=True (default) writes the .uasset to disk. Mutating: rejected during
    PIE/simulate and editor builds. Returns applied params + saved flag."""
    args = {"asset": asset, "save": save}
    if scalars:
        args["scalars"] = scalars
    if vectors:
        args["vectors"] = vectors
    return _bridge_call("set_material_params", args)


def _sha256_file(path):
    """SHA-256 of a file, streamed so large PAK/OBJ sources are fine."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _atomic_write_json(path, data):
    """Write JSON to 'path' atomically (temp file in the same dir + os.replace),
    creating parent dirs. Never leaves a partially written file at 'path'."""
    directory = os.path.dirname(path) or "."
    os.makedirs(directory, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=directory, prefix=".manifest-", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2, sort_keys=True)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)  # atomic on the same filesystem
        tmp = None
    finally:
        if tmp is not None and os.path.exists(tmp):
            os.remove(tmp)
    return path


@mcp.tool()
def write_recovery_manifest(map_name: str = "", recovery_root: str = "",
                            source_pak: str = "", source_map: str = "",
                            imported_assets: list = None, source_files: list = None,
                            material_substitutions: dict = None,
                            actor_placement_counts: dict = None,
                            warnings: list = None) -> dict:
    """Write an atomic recovery manifest JSON to
    Saved/LiandriMapForge/Recoveries/<MapSlug>/Manifests/recovery_manifest.json
    (never inside Unreal's Content dir). Records the resolved layout, source PAK
    and map paths, generated/imported asset paths, source filenames + SHA-256
    hashes + sizes, material substitutions, actor placement counts,
    warnings/failures, a UTC timestamp, and the plugin/engine version. Returns
    {manifest_path, map_slug, wrote}."""
    layout = bridge_recovery_layout(map_name=map_name, recovery_root=recovery_root)
    files = []
    for p in (source_files or []):
        entry = {"path": p}
        try:
            entry["sha256"] = _sha256_file(p)
            entry["size"] = os.path.getsize(p)
        except OSError as e:
            entry["error"] = str(e)
        files.append(entry)
    engine = None
    try:
        engine = bridge_status().get("engine")
    except BridgeError:
        pass
    manifest = {
        "plugin": "LiandriMapForge",
        "engine": engine,
        "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "map_slug": layout["map_slug"],
        "layout": layout,
        "source_pak": source_pak or None,
        "source_map": source_map or None,
        "imported_assets": imported_assets or [],
        "source_files": files,
        "material_substitutions": material_substitutions or {},
        "actor_placement_counts": actor_placement_counts or {},
        "warnings": warnings or [],
    }
    out_path = os.path.join(layout["manifests"], "recovery_manifest.json")
    _atomic_write_json(out_path, manifest)
    return {"manifest_path": out_path, "map_slug": layout["map_slug"], "wrote": True}


def _transform_close(a, b, tol=0.5):
    """True when two transform dicts agree within 'tol' on every present axis
    (location/scale use x/y/z, rotation uses pitch/yaw/roll)."""
    for comp in ("location", "rotation", "scale"):
        av, bv = a.get(comp, {}) or {}, b.get(comp, {}) or {}
        for k in set(av) | set(bv):
            if abs(float(av.get(k, 0.0)) - float(bv.get(k, 0.0))) > tol:
                return False
    return True


def _diff_actors(inspection, manifest, transform_tol=0.5):
    """Pure comparison of an inspect_static_mesh_actors result against an actors
    manifest ({"actors": [{name/label, mesh, transform?, material_overrides?}]}).
    Mutates nothing; returns categorized differences."""
    def key(a):
        return a.get("name") or a.get("label")
    want = {key(a): a for a in manifest.get("actors", []) if key(a)}
    have = {key(a): a for a in inspection.get("actors", []) if key(a)}
    missing = sorted(k for k in want if k not in have)
    extra = sorted(k for k in have if k not in want)
    null_mesh = sorted(k for k, a in have.items() if a.get("mesh") is None)
    substitutions, transform_mismatches, missing_overrides = [], [], []
    for k in sorted(set(want) & set(have)):
        w, h = want[k], have[k]
        wm, hm = w.get("mesh"), h.get("mesh")
        if wm and hm and wm != hm:
            substitutions.append({"actor": k, "expected": wm, "actual": hm})
        wt, ht = w.get("transform"), h.get("transform")
        if wt and ht and not _transform_close(wt, ht, transform_tol):
            transform_mismatches.append({"actor": k, "expected": wt, "actual": ht})
        want_ov = {int(s["slot"]): s.get("material")
                   for s in w.get("material_overrides", []) if s.get("is_override")}
        have_ov = {int(s["slot"]): s.get("material")
                   for s in h.get("material_overrides", []) if s.get("is_override")}
        for slot, mat in sorted(want_ov.items()):
            if slot not in have_ov or have_ov[slot] != mat:
                missing_overrides.append({"actor": k, "slot": slot,
                                          "expected": mat, "actual": have_ov.get(slot)})
    return {
        "missing_actors": missing,
        "extra_actors": extra,
        "null_mesh_actors": null_mesh,
        "mesh_substitutions": substitutions,
        "transform_mismatches": transform_mismatches,
        "missing_material_overrides": missing_overrides,
        "ok": not (missing or null_mesh or substitutions
                   or transform_mismatches or missing_overrides),
    }


@mcp.tool()
def audit_static_mesh_actors(actors_manifest_path: str, name_contains: str = "",
                             folder_contains: str = "", transform_tol: float = 0.5) -> dict:
    """Compare the live StaticMeshActors against an actors_manifest.json and
    report missing_actors, extra_actors, null_mesh_actors, mesh_substitutions,
    transform_mismatches, and missing_material_overrides (plus the live summary).
    Read-only; mutates nothing in the editor or on disk."""
    with open(actors_manifest_path, encoding="utf-8") as f:
        manifest = json.load(f)
    inspection = bridge_inspect_static_mesh_actors(
        name_contains=name_contains, folder_contains=folder_contains, limit=1000000)
    diff = _diff_actors(inspection, manifest, transform_tol)
    diff["summary"] = inspection.get("summary")
    return diff


if __name__ == "__main__":
    mcp.run()
