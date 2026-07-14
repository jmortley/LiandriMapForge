"""End-to-end smoke + regression test for the MapForgeBridge editor module.

Run with UnrealEd open (any map). Non-destructive: everything it imports it
deletes, and it never saves. Exercises every verb plus the audit-fix
regressions: fragmented requests, persistent connections, strict error
propagation, and the extended status fields.

    python tests/bridge_smoke.py
"""
import json, os, re, socket, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
MAPGEN = os.path.dirname(HERE)
sys.path.insert(0, MAPGEN)
import emit
import mapforge_mcp as mcp_mod

HOST, PORT = "127.0.0.1", int(os.environ.get("MAPFORGE_BRIDGE_PORT", "8765"))
FIXTURES = os.path.join(MAPGEN, "capture", "fixtures")


def call(cmd, args=None, timeout=600.0):
    return mcp_mod._bridge_call(cmd, args, timeout=timeout)


def expect_error(cmd, args, needle=""):
    try:
        call(cmd, args)
    except mcp_mod.BridgeError as e:
        assert needle.lower() in str(e).lower(), "wrong error for %s: %s" % (cmd, e)
        return str(e)
    raise AssertionError("%s unexpectedly succeeded" % cmd)


def wait_for_bridge(deadline_s=900):
    deadline = time.time() + deadline_s
    while True:
        try:
            return call("ping")
        except mcp_mod.BridgeError as e:
            if time.time() > deadline:
                raise SystemExit("bridge never came up: %s" % e)
            time.sleep(5)


def test_fragmented_request():
    """Finding 1 regression: a request trickled in tiny bursts spans many
    editor ticks; the old (reversed) close detection dropped it mid-line."""
    payload = (json.dumps({"id": 7, "cmd": "ping", "args": {}}) + "\n").encode()
    s = socket.create_connection((HOST, PORT), timeout=10.0)
    try:
        s.settimeout(60.0)
        for i in range(0, len(payload), 5):
            s.sendall(payload[i:i + 5])
            time.sleep(0.03)  # ~ several ticks per fragment
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(65536)
            assert chunk, "connection dropped on fragmented request (finding 1 regressed)"
            buf += chunk
    finally:
        s.close()
    r = json.loads(buf)
    assert r["ok"] and r["id"] == 7, r
    print("fragmented request: OK")


def test_persistent_connection():
    """Two requests over one socket -- the connection must survive idling
    between them (old peek closed healthy idle connections)."""
    s = socket.create_connection((HOST, PORT), timeout=10.0)
    try:
        s.settimeout(60.0)
        for reqid in (1, 2):
            s.sendall((json.dumps({"id": reqid, "cmd": "ping", "args": {}}) + "\n").encode())
            buf = b""
            while not buf.endswith(b"\n"):
                chunk = s.recv(65536)
                assert chunk, "connection dropped between requests"
                buf += chunk
            assert json.loads(buf)["id"] == reqid
            time.sleep(0.2)
    finally:
        s.close()
    print("persistent connection: OK")


def test_static_mesh():
    """Import the tiny OBJ fixture, check stats + complex-as-simple collision,
    place it at identity, and exercise the validation + exec-hardening rejects.
    Non-destructive: the asset is imported in-memory (save=False) to a dedicated
    /Game/Developers path and the placed actor is deleted; the level is never
    saved. The big Andok OBJ is intentionally NOT used here."""
    fixtures = os.path.join(HERE, "fixtures")
    obj = os.path.join(fixtures, "tiny_mesh.obj").replace("\\", "/")
    mtl = os.path.join(fixtures, "tiny_mesh.mtl").replace("\\", "/")
    dest = "/Game/Developers/MapForgeSmoke"
    name = "SM_MapForgeSmoke"

    # validation rejects (before any editor mutation)
    expect_error("import_static_mesh",
                 {"source": "C:/nope/does_not_exist.obj", "destination": dest, "name": name},
                 "not found")
    expect_error("import_static_mesh",
                 {"source": mtl, "destination": dest, "name": name},
                 "unsupported extension")
    expect_error("import_static_mesh",
                 {"source": obj, "destination": "/Engine/Foo", "name": name},
                 "/game")
    print("import validation rejects: OK")

    # successful import (in-memory; save=False keeps the disk clean)
    imp = call("import_static_mesh", {
        "source": obj, "destination": dest, "name": name,
        "overwrite": True, "save": False,
        "options": {"combine_meshes": True, "import_materials": False,
                    "import_textures": False, "generate_lightmap_uvs": True,
                    "compute_normals": True, "use_mikk_tspace": False,
                    "collision": "complex_as_simple", "lightmap_coordinate_index": 1},
    })
    assert imp["material_slot_count"] >= 1, imp
    assert imp["lod0_triangles"] == 2, imp
    assert imp["lod0_vertices"] > 0, imp
    assert imp["uv_channels"] >= 1, imp
    assert imp["collision_trace_mode"] == "complex_as_simple", imp
    assert imp["simple_collision_primitives"] == 0, imp
    asset = imp["asset"]
    print("import + stats + complex_as_simple: OK (%d tris, %d slot(s), %d uv ch)"
          % (imp["lod0_triangles"], imp["material_slot_count"], imp["uv_channels"]))

    # overwrite gate (the asset now exists in memory)
    expect_error("import_static_mesh",
                 {"source": obj, "destination": dest, "name": name,
                  "overwrite": False, "save": False},
                 "already exists")
    imp2 = call("import_static_mesh",
                {"source": obj, "destination": dest, "name": name,
                 "overwrite": True, "save": False})
    assert imp2["overwritten"] is True, imp2
    print("overwrite reject / allow: OK")

    # placement at identity, then clean up the actor (never save the level)
    placed = call("place_static_mesh", {"asset": asset, "label": "SM_MapForgeSmoke_Placed"})
    loc, sc = placed["transform"]["location"], placed["transform"]["scale"]
    assert abs(loc["x"]) < 1e-3 and abs(loc["y"]) < 1e-3 and abs(loc["z"]) < 1e-3, placed
    assert abs(sc["x"] - 1.0) < 1e-3 and abs(sc["z"] - 1.0) < 1e-3, placed
    print("place at identity: OK (%s)" % placed["label"])
    call("delete_actors", {"names": [placed["actor"]]})

    # exec hardening: dangerous families rejected before GEditor->Exec
    expect_error("exec", {"command": 'MAP LOAD FILE="X.umap"'}, "blocked")
    expect_error("exec", {"command": "OBJ IMPORT FILE=x.obj NAME=y"}, "blocked")
    print("exec MAP LOAD / OBJ IMPORT rejection: OK")


def main():
    status = wait_for_bridge()
    print("PING:", json.dumps(status))
    for field in ("lighting_building", "editor_building", "building",
                  "pie_active", "level_locked", "map_dirty"):
        assert field in status, "status missing '%s'" % field

    test_fragmented_request()
    test_persistent_connection()

    # Strict error propagation (finding 8).
    expect_error("import_t3d", {"t3d": "this is not T3D at all"}, "no actors")
    expect_error("nonsense_verb", {}, "unknown cmd")
    bogus = call("preload_assets", {"paths": ["/Game/Does/Not/Exist.Exist"]})
    assert bogus["failed"], "bogus asset should be reported as failed"
    print("error propagation: OK")

    # Classic flow: cube -> rebuild -> list -> retexture -> clean up.
    imp = call("import_t3d", {"t3d": emit.emit_smoke_cube()})
    assert imp["count"] == 1, imp
    call("rebuild_geometry")
    listed = call("list_actors", {"name_contains": "SmokeCube"})
    assert listed["matched"] >= 1
    cube_names = [a["name"] for a in listed["actors"]]

    mat = emit.PROFILES["solo"]["floor"]
    pre = call("preload_assets", {"paths": [mat]})
    assert not pre["failed"], pre
    setmat = call("set_surface_material", {"material": mat, "brush": "SmokeCube", "normal": [0, 0, 1]})
    assert setmat["changed"] >= 1, setmat
    print("cube + rebuild + setmat: OK (changed %d of %d surfaces)"
          % (setmat["changed"], setmat["total_surfaces"]))

    # The headline capability: real goo pool + functional lift fixtures with
    # every referenced asset preloaded (what the paste workflow couldn't do).
    snippet = ""
    for name in ("utpainvolume.t3d", "generic_lift.t3d"):
        with open(os.path.join(FIXTURES, name), encoding="utf-8") as f:
            snippet += f.read()
    refs = mcp_mod._asset_refs(snippet)
    pre2 = call("preload_assets", {"paths": refs})
    assert not pre2["failed"], pre2
    imp2 = call("import_t3d", {"t3d": snippet})
    classes = {a["class"] for a in imp2["actors"]}
    assert "UTPainVolume" in classes and "Generic_Lift_C" in classes, classes
    print("pain volume + lift import: OK (%d refs preloaded)" % pre2["loaded"])

    exported = call("export_t3d", {"selected_only": True})
    assert "UTPainVolume" in exported["t3d"], "export missing pain volume"
    print("selected export: OK (%d chars)" % exported["length"])

    # loaders=False pipeline (findings 2+3): no MatLoad cubes, bare refs harvested.
    spec = json.load(open(os.path.join(MAPGEN, "examples", "DM-Box01.mapspec.json")))
    t3d = emit.emit(spec, actors=False, loaders=False)
    assert "MatLoad_" not in t3d
    box_refs = mcp_mod._asset_refs(t3d)
    assert any("/Game/" in r for r in box_refs), "bare Texture= refs not harvested"
    print("loaders=False + bare-ref harvest: OK (%d refs)" % len(box_refs))

    test_static_mesh()

    # Clean up everything this test created.
    to_delete = cube_names + [a["name"] for a in imp2["actors"]]
    deleted = call("delete_actors", {"names": to_delete})
    assert deleted["deleted"] and deleted["matched"] == len(to_delete), deleted
    call("rebuild_geometry")
    left = call("list_actors", {"name_contains": "SmokeCube"})
    assert left["matched"] == 0, "cube survived delete"
    print("cleanup: OK (%d actors removed)" % deleted["matched"])

    print("FINAL:", json.dumps(call("status")))
    print("SMOKE TEST PASSED")


if __name__ == "__main__":
    main()
