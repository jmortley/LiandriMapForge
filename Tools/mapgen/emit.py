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

# ---- static-mesh kit (mined from DM-Solo). Alias -> /Game path. ----
# These are the ShellResources blockout primitives real UT4 whiteboxes use.
MESHES = {
    "cube":       "/Game/RestrictedAssets/Environments/ShellResources/Meshes/Generic/Shape_Cube.Shape_Cube",
    "cylinder":   "/Game/RestrictedAssets/Environments/ShellResources/Meshes/Generic/Shape_Cylinder.Shape_Cylinder",
    "sheet":      "/Game/RestrictedAssets/Environments/ShellResources/Meshes/Generic/SM_Sheet_500.SM_Sheet_500",
    "sheet_tess": "/Game/RestrictedAssets/Environments/ShellResources/Meshes/SM_Sheet_500_tesselated.SM_Sheet_500_tesselated",
    "gate":       "/Game/RestrictedAssets/Environments/ShellResources/Meshes/S_Gate.S_Gate",
    "techlift":   "/Game/RestrictedAssets/Blueprints/Lift/Meshes/SM_TechLift_Tall.SM_TechLift_Tall",
}

# ---- material palette (mined from DM-Solo). Real ShellResources materials, so
# generated surfaces read as UT4 instead of default checker. ----
_M  = "/Game/RestrictedAssets/Environments/ShellResources/Materials/"
_ML = "/Game/RestrictedAssets/Environments/Liandri/Materials/"
_MB = "/Game/RestrictedAssets/Environments/Materials/"

# Art profiles: real material palettes mined from shipped maps. Pick per map via
# spec defaults.artProfile. This is the "deck/solo art as defaults" system.
PROFILES = {
    "solo": {  # DM-Solo -- ShellResources tech (grey panels)
        "floor": _M + "Industrial/M_Shell_IND_Floor_A.M_Shell_IND_Floor_A",
        "wall":  _M + "Tech/M_Shell_City_Wall_B.M_Shell_City_Wall_B",
        "ceil":  _M + "Tech/M_Shell_City_Wall_A.M_Shell_City_Wall_A",
    },
    "deck": {  # DM-Deck -- Liandri concrete + orange
        "floor": _MB + "Blank_concrete2.Blank_concrete2",
        "wall":  _ML + "M_ConcreteWall.M_ConcreteWall",
        "ceil":  _ML + "M_ConcreteWall_Dark.M_ConcreteWall_Dark",
    },
}
DEFAULT_PROFILE = "deck"
SLUDGE_MAT = _MB + "SlimePit.SlimePit"   # real DM-Deck sludge material

# brush-name prefixes that denote a walkable (floor) surface across our builders
_FLOOR_PREFIXES = ("floor", "spine", "walk", "bridge", "end_", "up_", "cat", "tower", "pit_floor", "stair")

def brush_material(name, defaults):
    """Pick a material by the brush's role, from the chosen art profile. Spec defaults win."""
    prof = PROFILES.get(defaults.get("artProfile", DEFAULT_PROFILE), PROFILES[DEFAULT_PROFILE])
    if name.startswith("ceil"):
        return defaults.get("ceilingMaterial") or prof["ceil"]
    if name.startswith(_FLOOR_PREFIXES):
        return defaults.get("floorMaterial") or prof["floor"]
    return defaults.get("wallMaterial") or prof["wall"]

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
        "            LightColor=(B=200,G=225,R=249,A=255)",       # warm sun, from DM-Solo
        "            IndirectLightingIntensity=5.000000",
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

def atmospheric_fog(name):
    # Gives the SkyLight a bright sky to capture -> real ambient fill (no bake).
    return "\n".join([
        "      Begin Actor Class=AtmosphericFog Name=%s Archetype=AtmosphericFog'/Script/Engine.Default__AtmosphericFog'" % name,
        '         Begin Object Class=AtmosphericFogComponent Name="AtmosphericFogComponent0" Archetype=AtmosphericFogComponent\'Default__AtmosphericFog:AtmosphericFogComponent0\'',
        "         End Object",
        '         Begin Object Name="AtmosphericFogComponent0"',
        "            RelativeLocation=(X=0.000000,Y=0.000000,Z=0.000000)",
        "         End Object",
        "         AtmosphericFogComponent=AtmosphericFogComponent0",
        "         RootComponent=AtmosphericFogComponent0",
        '         ActorLabel="%s"' % name,
        "      End Actor",
    ])

def jump_pad(name, loc, target):
    # BaseJumpPad_C (root SceneComponent). `target` is the RELATIVE launch destination
    # offset; the BP computes arc velocity from it. Serialization from a real map dump;
    # BP-construction-dependent, so verify on first paste.
    x, y, z = loc
    tx, ty, tz = target
    return "\n".join([
        "      Begin Actor Class=BaseJumpPad_C Name=%s Archetype=BaseJumpPad_C'/Game/RestrictedAssets/Blueprints/JumpPad/BaseJumpPad.Default__BaseJumpPad_C'" % name,
        '         Begin Object Name="SceneComponent"',
        "            RelativeLocation=%s" % p(x, y, z),
        "         End Object",
        "         JumpTarget=(X=%.6f,Y=%.6f,Z=%.6f)" % (tx, ty, tz),
        "         JumpTime=1.200000",
        "         RootComponent=SceneComponent",
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

def static_mesh_actor(name, mesh, loc, rot=(0, 0, 0), scale=(1, 1, 1), material=None):
    # Format verified against DM-Solo's StaticMeshActor blocks. `mesh` = alias or /Game path.
    path = MESHES.get(mesh, mesh)
    x, y, z = loc
    pitch, yaw, roll = rot
    sx, sy, sz = scale
    lines = [
        "      Begin Actor Class=StaticMeshActor Name=%s Archetype=StaticMeshActor'/Script/Engine.Default__StaticMeshActor'" % name,
        '         Begin Object Class=StaticMeshComponent Name="StaticMeshComponent0" Archetype=StaticMeshComponent\'Default__StaticMeshActor:StaticMeshComponent0\'',
        "         End Object",
        '         Begin Object Name="StaticMeshComponent0"',
        "            StaticMesh=StaticMesh'%s'" % path,
    ]
    if material:
        lines.append("            OverrideMaterials(0)=Material'%s'" % material)
    lines += [
        "            RelativeLocation=%s" % p(x, y, z),
        "            RelativeRotation=(Pitch=%.6f,Yaw=%.6f,Roll=%.6f)" % (pitch, yaw, roll),
        "            RelativeScale3D=(X=%.6f,Y=%.6f,Z=%.6f)" % (sx, sy, sz),
        "         End Object",
        "         StaticMeshComponent=StaticMeshComponent0",
        "         RootComponent=StaticMeshComponent0",
        '         ActorLabel="%s"' % name,
        "      End Actor",
    ]
    return "\n".join(lines)

def material_loader(name, mat):
    """Tiny hidden mesh far below the map whose OverrideMaterials force-loads `mat`.
    Pasted BSP only resolves already-loaded materials, so without these the surfaces
    fall back to checker in a fresh level. Emitted BEFORE the brushes."""
    return static_mesh_actor(name, "cube", [0.0, 0.0, -9000.0], scale=(0.02, 0.02, 0.02), material=mat)

# ---- assembly ---------------------------------------------------------------

def _defaults(spec):
    d = {"wallHeight": 512, "wallThickness": 32, "floorThickness": 32}
    d.update(spec.get("defaults", {}))
    return d

def emit(spec, actors=True, loaders=True):
    """Compile a MapSpec to T3D. `loaders=False` drops the material-preloader
    meshes -- correct when importing through the editor bridge, which preloads
    assets for real (preload_assets) instead of tricking the paste resolver."""
    d = _defaults(spec)
    dm = spec.get("defaults", {})
    body = []
    used_mats = set()
    for room in spec.get("rooms", []):
        for (name, mn, mx) in room_boxes(room, d, spec.get("doorways", [])):
            mat = brush_material(name, dm); used_mats.add(mat)
            body.append(box_brush(name, mn, mx, mat))
    # raw additive boxes -- freeform tracing of an arbitrary floorplan
    for b in spec.get("brushes", []):
        mat = b.get("material") or brush_material(b["name"], dm); used_mats.add(mat)
        body.append(box_brush(b["name"], b["min"], b["max"], mat))
    if actors:
        for i, ps in enumerate(spec.get("playerStarts", [])):
            body.append(player_start("UTPlayerStart_%d" % i, ps["location"], ps.get("yaw", 0)))
        # Always-on environment rig (sun + sky + fog), calibrated to DM-Solo, so
        # generated maps are never dark. Overridable/disable via spec["environment"].
        env = spec.get("environment", {})
        if env.get("enabled", True):
            body.append(directional_light("Sun", env.get("sunPitch", -46.0), env.get("sunYaw", -35.0), env.get("sunIntensity", 6.0)))
            body.append(sky_light("Sky", env.get("skyIntensity", 3.0)))
            body.append(atmospheric_fog("Atmos"))
        # spec.lights = optional point-light accents layered on top of the rig
        for i, lt in enumerate(spec.get("lights", [])):
            if lt.get("type", "point") == "point":
                body.append(point_light("PointLight_%d" % i, lt["location"],
                            lt.get("intensity", 5000), lt.get("radius", 1024)))
        for i, pk in enumerate(spec.get("pickups", [])):
            body.append(pickup("Pickup_%d" % i, pk["type"], pk["location"]))
        for i, m in enumerate(spec.get("meshes", [])):
            body.append(static_mesh_actor("SM_%d" % i, m["mesh"], m["location"],
                        tuple(m.get("rotation", [0, 0, 0])), tuple(m.get("scale", [1, 1, 1])),
                        m.get("material")))
        for i, jp in enumerate(spec.get("jumpPads", [])):
            body.append(jump_pad("JumpPad_%d" % i, jp["location"], jp["target"]))
    loaders = [material_loader("MatLoad_%d" % i, m) for i, m in enumerate(sorted(u for u in used_mats if u))]
    return "Begin Map\n   Begin Level\n" + "\n".join(loaders + body) + "\n   End Level\nEnd Map\n"

def emit_smoke_cube():
    b = box_brush("SmokeCube", (-256, -256, 0), (256, 256, 512))
    return "Begin Map\n   Begin Level\n" + b + "\n   End Level\nEnd Map\n"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("spec")
    ap.add_argument("--out", default="out")
    ap.add_argument("--profile", default=None, help="override art profile (solo|deck); suffixes the output name")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    with open(args.spec, encoding="utf-8") as f:
        spec = json.load(f)
    name = spec.get("name", "map")
    if args.profile:
        spec.setdefault("defaults", {})["artProfile"] = args.profile
        name = "%s_%s" % (name, args.profile)

    d = _defaults(spec)
    nbrush = sum(len(room_boxes(r, d, spec.get("doorways", []))) for r in spec.get("rooms", [])) + len(spec.get("brushes", []))
    nact = len(spec.get("playerStarts", [])) + len(spec.get("lights", [])) + len(spec.get("pickups", [])) + len(spec.get("meshes", []))

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
