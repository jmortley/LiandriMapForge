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

    # multi-material regression: the two-usemtl cube must import as TWO slots.
    # That only happens when the .mtl sidecar is staged next to the temp OBJ --
    # without it the FBX SDK sees no materials and collapses to one slot.
    # import_materials stays False on purpose: the slot count comes from the
    # OBJ's usemtl groups, not from creating UMaterial assets.
    obj2 = os.path.join(fixtures, "cube_two_materials.obj").replace("\\", "/")
    imp3 = call("import_static_mesh", {
        "source": obj2, "destination": dest, "name": "SM_MapForgeSmoke_TwoMat",
        "overwrite": True, "save": False,
        "options": {"import_materials": False},
    })
    assert imp3["material_slot_count"] == 2, imp3
    assert imp3["lod0_triangles"] == 12, imp3
    print("multi-material OBJ (mtllib staged): OK (%d slots, %d tris)"
          % (imp3["material_slot_count"], imp3["lod0_triangles"]))

    # MTL traversal safety: an mtllib that escapes the source dir with '..' is
    # rejected (warning, not staged), so the mesh imports as one default slot.
    obj_trav = os.path.join(fixtures, "traversal_mtllib.obj").replace("\\", "/")
    imp4 = call("import_static_mesh", {
        "source": obj_trav, "destination": dest, "name": "SM_MapForgeSmoke_Trav",
        "overwrite": True, "save": False, "options": {"import_materials": False},
    })
    assert imp4["material_slot_count"] == 1, imp4
    assert any("unsafe mtllib" in w.lower() for w in imp4.get("warnings", [])), imp4
    print("MTL traversal rejection: OK (%d slot, warned)" % imp4["material_slot_count"])

    # exec hardening: dangerous families rejected before GEditor->Exec
    expect_error("exec", {"command": 'MAP LOAD FILE="X.umap"'}, "blocked")
    expect_error("exec", {"command": "OBJ IMPORT FILE=x.obj NAME=y"}, "blocked")
    print("exec MAP LOAD / OBJ IMPORT rejection: OK")


def _expected_slug(short_name):
    """Mirror of the C++ inferred-slug derivation: strip one recovery suffix
    (longest first, case-insensitive), then sanitize to [A-Za-z0-9_-]."""
    name = short_name
    for suf in ("-Recovered-Editable", "_Recovered_Editable", "-Recovered", "_Recovered"):
        if name.lower().endswith(suf.lower()):
            name = name[: -len(suf)]
            break
    return re.sub(r"[^A-Za-z0-9_-]", "_", name)


def _current_map_short():
    pkg = call("status")["map"]  # e.g. /Game/Developers/MrJmo/VehicleTest_Suspense
    return pkg.rstrip("/").split("/")[-1]


def test_recovery():
    """recovery_layout derivation (explicit + inferred, stable, suffix rules),
    slug/root rejection, two-map separation, Saved-vs-Content split, default +
    override roots for import, and the read-only actor audit (no dirtying)."""
    # explicit map_name -> canonical layout under the default root
    lay = call("recovery_layout", {"map_name": "DM-Andok_Scaled"})
    assert lay["map_slug"] == "DM-Andok_Scaled", lay
    assert lay["content_root"] == "/Game/RecoveredMaps/DM-Andok_Scaled", lay
    assert lay["static_meshes"] == "/Game/RecoveredMaps/DM-Andok_Scaled/Geometry/StaticMeshes", lay
    assert lay["bsp"].endswith("/Geometry/BSP"), lay
    assert lay["materials"].endswith("/Materials") and lay["blueprints"].endswith("/Blueprints"), lay
    # editable content stays under /Game; raw extraction lives under Saved, never
    # in the Content tree, and never touches RestrictedAssets.
    fsr = lay["filesystem_root"].replace("\\", "/")
    assert "/Saved/LiandriMapForge/Recoveries/DM-Andok_Scaled" in fsr, lay
    assert not fsr.startswith("/Game"), lay
    assert lay["raw_extract"].replace("\\", "/").endswith("/RawExtract"), lay
    assert "RestrictedAssets" not in lay["content_root"], lay
    print("recovery_layout (explicit slug, Saved raw-extract outside Content): OK")

    # stable: identical input -> identical layout
    assert call("recovery_layout", {"map_name": "DM-Andok_Scaled"}) == lay, "derivation not stable"

    # explicit map_name is NOT suffix-stripped (stripping is inference-only)
    kept = call("recovery_layout", {"map_name": "DM-Andok_Scaled-Recovered-Editable"})
    assert kept["map_slug"] == "DM-Andok_Scaled-Recovered-Editable", kept

    # inferred slug mirrors the C++ derivation for whatever map is open
    short = _current_map_short()
    inferred = call("recovery_layout", {})
    assert inferred["map_slug"] == _expected_slug(short), (inferred["map_slug"], short)
    print("recovery_layout (inferred slug '%s'): OK" % inferred["map_slug"])

    # two maps with the same asset names -> separate content roots
    a = call("recovery_layout", {"map_name": "MapAlpha"})
    b = call("recovery_layout", {"map_name": "MapBeta"})
    assert a["static_meshes"] != b["static_meshes"], (a, b)
    assert a["static_meshes"].endswith("/MapAlpha/Geometry/StaticMeshes"), a
    assert b["static_meshes"].endswith("/MapBeta/Geometry/StaticMeshes"), b
    print("two-map separation: OK")

    # invalid names / traversal / non-/Game root are rejected
    expect_error("recovery_layout", {"map_name": "../evil"}, "invalid map_name")
    expect_error("recovery_layout", {"map_name": "a/b"}, "invalid map_name")
    expect_error("recovery_layout", {"map_name": "X", "recovery_root": "/Engine/Foo"}, "/game")
    print("recovery_layout rejects (traversal / separators / /Engine root): OK")

    # default mesh destination via the recovery layout. Override the root into a
    # Developers sandbox so nothing lands under /Game/RecoveredMaps, even in memory.
    fixtures = os.path.join(HERE, "fixtures")
    obj = os.path.join(fixtures, "tiny_mesh.obj").replace("\\", "/")
    root = "/Game/Developers/MapForgeRecovery"
    smesh = call("import_static_mesh", {
        "source": obj, "map_name": "SmokeRecovery", "recovery_root": root,
        "overwrite": True, "save": False, "name": "SM_RecDefault"})
    assert smesh["asset"].startswith(root + "/SmokeRecovery/Geometry/StaticMeshes/SM_RecDefault"), smesh
    assert smesh["recovery_layout"]["content_root"] == root + "/SmokeRecovery", smesh
    bsp = call("import_static_mesh", {
        "source": obj, "map_name": "SmokeRecovery", "recovery_root": root,
        "category": "Geometry/BSP", "overwrite": True, "save": False, "name": "SM_RecBSP"})
    assert bsp["asset"].startswith(root + "/SmokeRecovery/Geometry/BSP/SM_RecBSP"), bsp
    expect_error("import_static_mesh",
                 {"source": obj, "map_name": "SmokeRecovery", "recovery_root": root,
                  "category": "../../etc", "name": "X"}, "traversal")
    print("default StaticMeshes + BSP destinations (category rejects): OK")

    # actor audit is read-only: inspecting must not change map_dirty
    dirty_before = call("status")["map_dirty"]
    insp = call("inspect_static_mesh_actors", {"limit": 5})
    assert "summary" in insp, insp
    for f in ("total_static_mesh_actors", "actors_null_mesh", "unique_mesh_paths",
              "actors_hidden", "actors_unresolved_materials"):
        assert f in insp["summary"], insp["summary"]
    dirty_after = call("status")["map_dirty"]
    assert dirty_after == dirty_before, \
        "inspect dirtied the map (%s -> %s)" % (dirty_before, dirty_after)
    print("inspect_static_mesh_actors read-only (no dirty): OK (%d SMAs)"
          % insp["summary"]["total_static_mesh_actors"])

    # live inspect + audit diff: place a probe, compare against a manifest that
    # expects a different mesh and one absent actor -> substitution + missing.
    probe = call("place_static_mesh", {"asset": smesh["asset"],
                                       "label": "SM_AuditProbe", "location": [1000, 0, 0]})
    inspection = call("inspect_static_mesh_actors", {"name_contains": "SM_AuditProbe"})
    manifest = {"actors": [
        {"name": probe["actor"], "mesh": "/Game/Nope/Wrong.Wrong"},
        {"name": "SM_AuditProbe_Missing", "mesh": "/Game/Nope/Gone.Gone"}]}
    audit = mcp_mod._diff_actors(inspection, manifest)
    assert any(s["actor"] == probe["actor"] for s in audit["mesh_substitutions"]), audit
    assert "SM_AuditProbe_Missing" in audit["missing_actors"], audit
    call("delete_actors", {"names": [probe["actor"]]})
    print("live inspect + audit diff (substitution + missing): OK")


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
    test_recovery()

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
