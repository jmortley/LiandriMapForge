"""Offline unit tests for the MapForge recovery MCP helpers.

These need no running editor: they exercise the pure comparison logic
(_diff_actors / _transform_close) and the atomic manifest writer
(_atomic_write_json / _sha256_file). The live editor-driven recovery checks
live in bridge_smoke.py.

    python tests/test_recovery_offline.py     # or: pytest tests/test_recovery_offline.py
"""
import hashlib, json, os, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
MAPGEN = os.path.dirname(HERE)
sys.path.insert(0, MAPGEN)
import mapforge_mcp as mod  # noqa: E402  (imports emit + fastmcp; no server started)


def _slot(i, material, is_override=True):
    return {"slot": i, "material": material, "is_override": is_override}


def test_diff_all_match():
    """Identical live vs manifest -> ok, every bucket empty."""
    actors = [{"name": "SM_A", "mesh": "/Game/M/SM_A.SM_A",
               "transform": {"location": {"x": 0, "y": 0, "z": 0}},
               "material_overrides": [_slot(0, "/Game/M/M_A.M_A")]}]
    diff = mod._diff_actors({"actors": actors}, {"actors": actors})
    assert diff["ok"], diff
    for bucket in ("missing_actors", "extra_actors", "null_mesh_actors",
                   "mesh_substitutions", "transform_mismatches",
                   "missing_material_overrides"):
        assert diff[bucket] == [], (bucket, diff[bucket])


def test_diff_missing_extra_and_null():
    """Missing actor, extra actor, and a null-mesh live actor are all reported."""
    manifest = {"actors": [{"name": "SM_Present", "mesh": "/Game/M/SM.SM"},
                           {"name": "SM_Missing", "mesh": "/Game/M/SM2.SM2"}]}
    live = {"actors": [{"name": "SM_Present", "mesh": None},
                       {"name": "SM_Extra", "mesh": "/Game/M/SM3.SM3"}]}
    diff = mod._diff_actors(live, manifest)
    assert diff["missing_actors"] == ["SM_Missing"], diff
    assert diff["extra_actors"] == ["SM_Extra"], diff
    assert diff["null_mesh_actors"] == ["SM_Present"], diff
    assert not diff["ok"]


def test_diff_mesh_substitution():
    """Same actor, different mesh -> substitution."""
    manifest = {"actors": [{"name": "SM_A", "mesh": "/Game/M/Expected.Expected"}]}
    live = {"actors": [{"name": "SM_A", "mesh": "/Game/M/Actual.Actual"}]}
    diff = mod._diff_actors(live, manifest)
    assert diff["mesh_substitutions"] == [
        {"actor": "SM_A", "expected": "/Game/M/Expected.Expected",
         "actual": "/Game/M/Actual.Actual"}], diff
    assert not diff["ok"]


def test_diff_transform_mismatch_and_tolerance():
    """Location past tolerance mismatches; within tolerance does not."""
    manifest = {"actors": [{"name": "SM_A", "mesh": "/Game/M/SM.SM",
                            "transform": {"location": {"x": 0, "y": 0, "z": 0}}}]}
    far = {"actors": [{"name": "SM_A", "mesh": "/Game/M/SM.SM",
                       "transform": {"location": {"x": 10, "y": 0, "z": 0}}}]}
    near = {"actors": [{"name": "SM_A", "mesh": "/Game/M/SM.SM",
                        "transform": {"location": {"x": 0.1, "y": 0, "z": 0}}}]}
    assert mod._diff_actors(far, manifest)["transform_mismatches"], "10uu should mismatch"
    assert not mod._diff_actors(near, manifest)["transform_mismatches"], "0.1uu within tol"


def test_diff_missing_material_override():
    """An expected override slot that is absent/different live is reported."""
    manifest = {"actors": [{"name": "SM_A", "mesh": "/Game/M/SM.SM",
                            "material_overrides": [_slot(1, "/Game/M/M_Want.M_Want")]}]}
    live = {"actors": [{"name": "SM_A", "mesh": "/Game/M/SM.SM",
                        "material_overrides": [_slot(1, None, is_override=False)]}]}
    diff = mod._diff_actors(live, manifest)
    assert diff["missing_material_overrides"] == [
        {"actor": "SM_A", "slot": 1,
         "expected": "/Game/M/M_Want.M_Want", "actual": None}], diff


def test_transform_close_rotation_keys():
    """Rotation uses pitch/yaw/roll; a yaw delta past tol is not close."""
    a = {"rotation": {"pitch": 0, "yaw": 0, "roll": 0}}
    b = {"rotation": {"pitch": 0, "yaw": 90, "roll": 0}}
    assert not mod._transform_close(a, b)
    assert mod._transform_close(a, {"rotation": {"pitch": 0, "yaw": 0.2, "roll": 0}})


def test_atomic_write_json_and_sha256():
    """Atomic write yields valid JSON, leaves no .tmp, and hashes match."""
    with tempfile.TemporaryDirectory() as d:
        target = os.path.join(d, "sub", "recovery_manifest.json")
        data = {"map_slug": "DM-Andok_Scaled", "warnings": [], "n": 2}
        mod._atomic_write_json(target, data)
        assert os.path.isfile(target)
        with open(target, encoding="utf-8") as f:
            assert json.load(f) == data
        # no leftover temp files beside the target
        leftovers = [n for n in os.listdir(os.path.dirname(target)) if n.endswith(".tmp")]
        assert leftovers == [], leftovers
        # sha256 helper matches hashlib over the same bytes
        with open(target, "rb") as f:
            expected = hashlib.sha256(f.read()).hexdigest()
        assert mod._sha256_file(target) == expected


def main():
    tests = [v for k, v in sorted(globals().items())
             if k.startswith("test_") and callable(v)]
    for t in tests:
        t()
        print("OK:", t.__name__)
    print("OFFLINE RECOVERY TESTS PASSED (%d)" % len(tests))


if __name__ == "__main__":
    main()
