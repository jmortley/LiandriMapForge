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
import json, os
from mcp.server.fastmcp import FastMCP
import emit  # same directory; emit.py guards __main__ so import is side-effect-free

HERE = os.path.dirname(os.path.abspath(__file__))
mcp = FastMCP("mapforge")


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


if __name__ == "__main__":
    mcp.run()
