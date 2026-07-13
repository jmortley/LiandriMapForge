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
import json, os, re, socket
from mcp.server.fastmcp import FastMCP
import emit  # same directory; emit.py guards __main__ so import is side-effect-free

HERE = os.path.dirname(os.path.abspath(__file__))
mcp = FastMCP("mapforge")

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
def bridge_set_class_defaults(asset: str, defaults: dict) -> dict:
    """Override default (CDO) property values on a Blueprint class and save --
    the Tier-1 'config variant' path (no graph logic). 'defaults' maps property
    name -> value; bools/numbers are converted, and strings pass through as UE
    property literals (e.g. "(X=1,Y=2,Z=3)" for a struct, an enum name, or a
    class path). Inherited native properties of the parent mutator/gamemode
    (bForceRespawn, TimeLimit, GoalScore, ...) are valid targets. Returns
    {applied, failed, saved}."""
    return _bridge_call("set_class_defaults", {"asset": asset, "defaults": defaults})


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


if __name__ == "__main__":
    mcp.run()
