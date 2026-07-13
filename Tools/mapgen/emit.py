#!/usr/bin/env python3
"""
MapForge emitter: MapSpec (JSON) -> UnrealEd T3D text for UT4 (UE4 4.15 fork).

Grammar matched against this fork's engine source AND a live ground-truth capture
(capture/ground-truth.t3d):
  - BSP polylist / brush     : EditorExporters.cpp:295, EditorObject.cpp:293/641,
                               Brush.h:82 (BrushType), Brush.cpp:38 (BrushComponent0)
  - UTPlayerStart            : root component "CollisionCapsule"        [captured]
  - PointLight               : root "LightComponent0" + per-light LightGuid [captured]
  - Pickups                  : Blueprint classes (Health_Medium_C etc), root "Capsule"
                               [Health_Medium verified in capture; rest share AUTPickup base]

Usage:  python emit.py examples/DM-Box01.mapspec.json --out out/
"""
import json, sys, argparse, os, hashlib

# ---- pickup registry (friendly name -> BP class, /Game asset path, root component)
# Paths verified on disk under Content/RestrictedAssets/Pickups/.
PICKUPS = {
    "health_small":    ("Health_Small_C",    "/Game/RestrictedAssets/Pickups/Health/Health_Small",    "Capsule"),
    "health_medium":   ("Health_Medium_C",   "/Game/RestrictedAssets/Pickups/Health/Health_Medium",   "Capsule"),
    "health_large":    ("Health_Large_C",    "/Game/RestrictedAssets/Pickups/Health/Health_Large",    "Capsule"),
    "armor_small":     ("Armor_Small_C",     "/Game/RestrictedAssets/Pickups/Armor/Armor_Small",      "Capsule"),
    "armor_thighpads": ("Armor_ThighPads_C", "/Game/RestrictedAssets/Pickups/Armor/Armor_ThighPads",  "Capsule"),
    "armor_chest":     ("Armor_Chest_C",     "/Game/RestrictedAssets/Pickups/Armor/Armor_Chest",      "Capsule"),
    "armor_helmet":    ("Armor_Helmet_C",    "/Game/RestrictedAssets/Pickups/Armor/Armor_Helmet",     "Capsule"),
    "shieldbelt":      ("Armor_ShieldBelt_C","/Game/RestrictedAssets/Pickups/Armor/Armor_ShieldBelt", "Capsule"),
}

# ---- formatting -------------------------------------------------------------

def v(x, y, z):
    """T3D poly vector (SetFVECTOR format: sign-forced, zero-padded, 6dp)."""
    return "%+013.6f,%+013.6f,%+013.6f" % (x, y, z)

def p(x, y, z):
    """T3D property vector (X=..,Y=..,Z=..)."""
    return "(X=%.6f,Y=%.6f,Z=%.6f)" % (x, y, z)

def guid(seed):
    """Deterministic 32-hex GUID so emitter output is reproducible for clean diffs."""
    return hashlib.md5(seed.encode("utf-8")).hexdigest().upper()

# ---- BSP box geometry (verified) --------------------------------------------

def _box_faces(mn, mx):
    x0, y0, z0 = mn
    x1, y1, z1 = mx
    return [
        ((0,0,1),  (1,0,0), (0,1,0),  [(x0,y0,z1),(x1,y0,z1),(x1,y1,z1),(x0,y1,z1)]),
        ((0,0,-1), (1,0,0), (0,1,0),  [(x0,y0,z0),(x0,y1,z0),(x1,y1,z0),(x1,y0,z0)]),
        ((1,0,0),  (0,1,0), (0,0,-1), [(x1,y0,z0),(x1,y1,z0),(x1,y1,z1),(x1,y0,z1)]),
        ((-1,0,0), (0,1,0), (0,0,-1), [(x0,y0,z0),(x0,y0,z1),(x0,y1,z1),(x0,y1,z0)]),
        ((0,1,0),  (1,0,0), (0,0,-1), [(x0,y1,z0),(x0,y1,z1),(x1,y1,z1),(x1,y1,z0)]),
        ((0,-1,0), (1,0,0), (0,0,-1), [(x0,y0,z0),(x1,y0,z0),(x1,y0,z1),(x0,y0,z1)]),
    ]

def _poly(face, texture=None):
    normal, tu, tv, verts = face
    head = "               Begin Polygon"
    if texture:
        head += " Texture=%s" % texture
    lines = [head,
             "                  Origin   " + v(*verts[0]),
             "                  Normal   " + v(*normal),
             "                  TextureU " + v(*tu),
             "                  TextureV " + v(*tv)]
    for vert in verts:
        lines.append("                  Vertex   " + v(*vert))
    lines.append("               End Polygon")
    return "\n".join(lines)

def box_brush(name, mn, mx, texture=None):
    polys = "\n".join(_poly(f, texture) for f in _box_faces(mn, mx))
    model = "Model_%s" % name
    return "\n".join([
        "      Begin Actor Class=Brush Name=%s Archetype=Brush'/Script/Engine.Default__Brush'" % name,
        '         Begin Object Class=BrushComponent Name="BrushComponent0" Archetype=BrushComponent\'Default__Brush:BrushComponent0\'',
        "         End Object",
        "         Begin Brush Name=%s" % model,
        "            Begin PolyList",
        polys,
        "            End PolyList",
        "         End Brush",
        '         Begin Object Name="BrushComponent0"',
        "            Brush=Model'%s'" % model,
        "         End Object",
        "         BrushType=Brush_Add",
        "         Brush=Model'%s'" % model,
        "         BrushComponent=BrushComponent0",
        "         RootComponent=BrushComponent0",
        '         ActorLabel="%s"' % name,
        "      End Actor",
    ])

# ---- rooms -> slabs, with doorway punching ----------------------------------

def _wall_boxes(rid, side, x0, y0, x1, y1, z0, z1, t, doors):
    """One wall side as solid slab(s), split around any doorways (leaving gaps +
    lintels/sills). Walls own corners on X; N/S walls span interior X only."""
    if side in ("E", "W"):
        fa = (x1, x1 + t) if side == "E" else (x0 - t, x0)     # fixed X
        sA, sB = y0 - t, y1 + t                                 # span along Y
        center = (y0 + y1) / 2.0
        mk = lambda lo, hi, zl, zh, tag: ("%s_w%s_%s" % (rid, side, tag), (fa[0], lo, zl), (fa[1], hi, zh))
    else:
        fa = (y1, y1 + t) if side == "N" else (y0 - t, y0)     # fixed Y
        sA, sB = x0, x1                                          # span along X
        center = (x0 + x1) / 2.0
        mk = lambda lo, hi, zl, zh, tag: ("%s_w%s_%s" % (rid, side, tag), (lo, fa[0], zl), (hi, fa[1], zh))

    if not doors:
        return [mk(sA, sB, z0, z1, "0")]

    ops = []
    for i, dw in enumerate(doors):
        w = dw.get("width", 192); dh = dw.get("height", 288); sill = dw.get("sill", 0)
        c = center + dw.get("center", 0)
        ops.append((max(c - w/2.0, sA), min(c + w/2.0, sB), z0 + sill, z0 + sill + dh, i))
    ops.sort()

    out, cursor = [], sA
    for (oA, oB, zS, zT, i) in ops:
        if oA > cursor:
            out.append(mk(cursor, oA, z0, z1, "s%d" % i))
        if zS > z0:
            out.append(mk(oA, oB, z0, zS, "sill%d" % i))
        if zT < z1:
            out.append(mk(oA, oB, zT, z1, "lin%d" % i))
        cursor = max(cursor, oB)
    if cursor < sB:
        out.append(mk(cursor, sB, z0, z1, "sE"))
    return out

def room_boxes(room, d, doorways):
    x0, y0, z0 = room["min"]
    x1, y1, z1 = room["max"]
    t, ft = d["wallThickness"], d["floorThickness"]
    rid = room["id"]
    sides = set(room.get("walls", ["N", "E", "S", "W"]))
    out = []
    if room.get("floor", True):
        out.append(("%s_floor" % rid, (x0-t, y0-t, z0-ft), (x1+t, y1+t, z0)))
    if room.get("ceiling", True):
        out.append(("%s_ceil" % rid, (x0-t, y0-t, z1), (x1+t, y1+t, z1+ft)))
    for side in ("N", "E", "S", "W"):
        if side in sides:
            doors = [dw for dw in doorways if dw.get("room") == rid and dw.get("side") == side]
            out.extend(_wall_boxes(rid, side, x0, y0, x1, y1, z0, z1, t, doors))
    return out

# ---- actors (templates locked to the ground-truth capture) ------------------

def player_start(name, loc, yaw):
    x, y, z = loc
    return "\n".join([
        "      Begin Actor Class=UTPlayerStart Name=%s Archetype=UTPlayerStart'/Script/UnrealTournament.Default__UTPlayerStart'" % name,
        '         Begin Object Class=CapsuleComponent Name="CollisionCapsule" Archetype=CapsuleComponent\'Default__UTPlayerStart:CollisionCapsule\'',
        "         End Object",
        '         Begin Object Name="CollisionCapsule"',
        "            RelativeLocation=%s" % p(x, y, z),
        "            RelativeRotation=(Pitch=0.000000,Yaw=%.6f,Roll=0.000000)" % yaw,
        "         End Object",
        "         CapsuleComponent=CollisionCapsule",
        "         RootComponent=CollisionCapsule",
        '         ActorLabel="%s"' % name,
        "      End Actor",
    ])

def point_light(name, loc, intensity, radius):
    x, y, z = loc
    return "\n".join([
        "      Begin Actor Class=PointLight Name=%s Archetype=PointLight'/Script/Engine.Default__PointLight'" % name,
        '         Begin Object Class=PointLightComponent Name="LightComponent0" Archetype=PointLightComponent\'Default__PointLight:LightComponent0\'',
        "         End Object",
        '         Begin Object Name="LightComponent0"',
        "            Mobility=Movable",  # dump-verified token; lights in realtime, no bake
        "            LightGuid=%s" % guid(name),
        "            RelativeLocation=%s" % p(x, y, z),
        "            Intensity=%.6f" % intensity,
        "            AttenuationRadius=%.6f" % radius,
        "         End Object",
        "         PointLightComponent=LightComponent0",
        "         LightComponent=LightComponent0",
        "         RootComponent=LightComponent0",
        '         ActorLabel="%s"' % name,
        "      End Actor",
    ])

def directional_light(name, pitch, yaw, intensity):
    # The "sun". Movable => realtime, no bake. Reconstructed (not yet capture-verified).
    return "\n".join([
        "      Begin Actor Class=DirectionalLight Name=%s Archetype=DirectionalLight'/Script/Engine.Default__DirectionalLight'" % name,
        '         Begin Object Class=DirectionalLightComponent Name="LightComponent0" Archetype=DirectionalLightComponent\'Default__DirectionalLight:LightComponent0\'',
        "         End Object",
        '         Begin Object Name="LightComponent0"',
        "            Mobility=Movable",
        "            LightGuid=%s" % guid(name),
        "            Intensity=%.6f" % intensity,
        "            RelativeRotation=(Pitch=%.6f,Yaw=%.6f,Roll=0.000000)" % (pitch, yaw),
        "         End Object",
        "         LightComponent=LightComponent0",
        "         RootComponent=LightComponent0",
        '         ActorLabel="%s"' % name,
        "      End Actor",
    ])

def sky_light(name, intensity):
    # Ambient fill from the open sky. Reconstructed (not yet capture-verified).
    return "\n".join([
        "      Begin Actor Class=SkyLight Name=%s Archetype=SkyLight'/Script/Engine.Default__SkyLight'" % name,
        '         Begin Object Class=SkyLightComponent Name="SkyLightComponent0" Archetype=SkyLightComponent\'Default__SkyLight:SkyLightComponent0\'',
        "         End Object",
        '         Begin Object Name="SkyLightComponent0"',
        "            Mobility=Movable",
        "            Intensity=%.6f" % intensity,
        "            LowerHemisphereIsBlack=False",
        "         End Object",
        "         LightComponent=SkyLightComponent0",
        "         RootComponent=SkyLightComponent0",
        '         ActorLabel="%s"' % name,
        "      End Actor",
    ])

def pickup(name, ptype, loc):
    if ptype not in PICKUPS:
        raise SystemExit("unknown pickup type '%s' (known: %s)" % (ptype, ", ".join(sorted(PICKUPS))))
    cls, path, root = PICKUPS[ptype]
    x, y, z = loc
    return "\n".join([
        "      Begin Actor Class=%s Name=%s Archetype=%s'%s.Default__%s'" % (cls, name, cls, path, cls),
        '         Begin Object Name="%s"' % root,
        "            RelativeLocation=%s" % p(x, y, z),
        "         End Object",
        "         RootComponent=%s" % root,
        '         ActorLabel="%s"' % ptype,
        "      End Actor",
    ])

# ---- assembly ---------------------------------------------------------------

def _defaults(spec):
    d = {"wallHeight": 512, "wallThickness": 32, "floorThickness": 32}
    d.update(spec.get("defaults", {}))
    return d

def emit(spec, actors=True):
    d = _defaults(spec)
    tex = spec.get("defaults", {}).get("wallMaterial") or None
    body = []
    for room in spec.get("rooms", []):
        for (name, mn, mx) in room_boxes(room, d, spec.get("doorways", [])):
            body.append(box_brush(name, mn, mx, tex))
    # raw additive boxes -- freeform tracing of an arbitrary floorplan
    for b in spec.get("brushes", []):
        body.append(box_brush(b["name"], b["min"], b["max"], b.get("material") or tex))
    if actors:
        for i, ps in enumerate(spec.get("playerStarts", [])):
            body.append(player_start("UTPlayerStart_%d" % i, ps["location"], ps.get("yaw", 0)))
        for i, lt in enumerate(spec.get("lights", [])):
            kind = lt.get("type", "point")
            if kind == "directional":
                body.append(directional_light("DirectionalLight_%d" % i,
                            lt.get("pitch", -50), lt.get("yaw", -35), lt.get("intensity", 6)))
            elif kind == "sky":
                body.append(sky_light("SkyLight_%d" % i, lt.get("intensity", 1.5)))
            else:
                body.append(point_light("PointLight_%d" % i, lt["location"],
                            lt.get("intensity", 5000), lt.get("radius", 1024)))
        for i, pk in enumerate(spec.get("pickups", [])):
            body.append(pickup("Pickup_%d" % i, pk["type"], pk["location"]))
    return "Begin Map\n   Begin Level\n" + "\n".join(body) + "\n   End Level\nEnd Map\n"

def emit_smoke_cube():
    b = box_brush("SmokeCube", (-256, -256, 0), (256, 256, 512))
    return "Begin Map\n   Begin Level\n" + b + "\n   End Level\nEnd Map\n"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("spec")
    ap.add_argument("--out", default="out")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    with open(args.spec, encoding="utf-8") as f:
        spec = json.load(f)
    name = spec.get("name", "map")

    d = _defaults(spec)
    nbrush = sum(len(room_boxes(r, d, spec.get("doorways", []))) for r in spec.get("rooms", [])) + len(spec.get("brushes", []))
    nact = len(spec.get("playerStarts", [])) + len(spec.get("lights", [])) + len(spec.get("pickups", []))

    outputs = {
        "%s.t3d" % name: emit(spec, actors=True),
        "%s.geometry.t3d" % name: emit(spec, actors=False),
        "smoke_cube.t3d": emit_smoke_cube(),
    }
    for fn, text in outputs.items():
        with open(os.path.join(args.out, fn), "w", encoding="utf-8", newline="\r\n") as f:
            f.write(text)
    print("wrote %s.t3d  (%d brushes + %d actors)" % (name, nbrush, nact))
    print("wrote %s.geometry.t3d, smoke_cube.t3d" % name)

if __name__ == "__main__":
    main()
