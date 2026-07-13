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


def _bridge_call(cmd, args=None, timeout=600.0):
    """One request/response round-trip with the in-editor bridge."""
    request = json.dumps({"id": 1, "cmd": cmd, "args": args or {}})
    try:
        sock = socket.create_connection((BRIDGE_HOST, BRIDGE_PORT), timeout=10.0)
    except OSError as e:
        raise BridgeError(
            "MapForge bridge not reachable on %s:%d -- is UnrealEd running with "
            "the LiandriMapForge plugin enabled? (%s)" % (BRIDGE_HOST, BRIDGE_PORT, e))
    try:
        sock.settimeout(timeout)
        sock.sendall(request.encode("utf-8") + b"\n")
        buf = bytearray()
        while not buf.endswith(b"\n"):
            chunk = sock.recv(1 << 16)
            if not chunk:
                raise BridgeError("bridge closed the connection mid-response")
            buf += chunk
    finally:
        sock.close()
    response = json.loads(buf.decode("utf-8"))
    if not response.get("ok"):
        raise BridgeError(response.get("error", "unknown bridge error"))
    return response.get("result")


_ASSET_REF = re.compile(r"'(/(?:Game|Engine)/[^']+)'")


def _asset_refs(t3d):
    """Every /Game/ and /Engine/ object path a T3D document references.
    Subobject refs (Pkg.Obj:Sub) are trimmed to Pkg.Obj -- loading the outer
    object is what pins the package."""
    return sorted({m.split(":", 1)[0] for m in _ASSET_REF.findall(t3d)})


def _import_t3d(t3d, preload=True):
    if preload:
        refs = _asset_refs(t3d)
        if refs:
            preloaded = _bridge_call("preload_assets", {"paths": refs})
        else:
            preloaded = {"loaded": 0, "failed": []}
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
    actor count, and whether a lighting/nav build is currently running."""
    return _bridge_call("ping")


@mcp.tool()
def bridge_import_t3d(t3d: str, preload: bool = True) -> dict:
    """Import a T3D document into the level open in UnrealEd. First preloads
    every /Game/ and /Engine/ asset the T3D references, so materials, Blueprint
    actors (lifts, pickups), sounds and damage types all resolve on a fresh
    level -- no preloader-cube hack. Follow with bridge_rebuild_geometry()."""
    return _import_t3d(t3d, preload=preload)


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
def bridge_save(filename: str = "") -> dict:
    """Save the current level. A never-saved level needs an explicit
    'filename' (else UnrealEd pops a modal Save-As dialog and the call
    blocks until a human answers it)."""
    args = {"filename": filename} if filename else {}
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
        with open(save_to, "w", encoding="utf-8") as f:
            f.write(result.pop("t3d"))
        result["saved_to"] = save_to
    return result


@mcp.tool()
def bridge_list_actors(name_contains: str = "", class_contains: str = "",
                       limit: int = 200) -> dict:
    """List actors in the open level (name, label, class, location), filtered
    by case-insensitive substrings of name/label and/or class."""
    return _bridge_call("list_actors", {
        "name_contains": name_contains,
        "class_contains": class_contains,
        "limit": limit,
    })


@mcp.tool()
def bridge_delete_actors(names: list) -> dict:
    """Delete actors by name or label (case-insensitive exact match). Builder
    brush and WorldSettings are always protected. Use with bridge_list_actors
    to clear and re-import a region instead of starting a fresh level."""
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


if __name__ == "__main__":
    mcp.run()
