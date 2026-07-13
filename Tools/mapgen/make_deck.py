#!/usr/bin/env python3
"""
Author a multi-level DM-Deck-inspired arena from knowledge (NOT a silhouette trace).
Demonstrates verticality the flat tracer can't: a lower arena wrapping a central
PIT (shield belt down low = risk), an upper CATWALK RING + a central TOWER (armor
up high = reward), linked by STAIRS and a JUMP PAD. Deck-inspired blockout, not a
faithful clone. Writes examples/DM-Deck.mapspec.json -> feed to emit.py.
"""
import json, os

brushes, starts, pickups, jumppads, meshes = [], [], [], [], []

def box(name, x0, y0, z0, x1, y1, z1):
    brushes.append({"name": name, "min": [x0, y0, z0], "max": [x1, y1, z1]})

def stairs(prefix, x0, x1, y0, run, steps, z_base, rise):
    """Solid ascending staircase, treads advancing in +Y, rising `rise` over `steps`."""
    sh = rise / steps
    for i in range(steps):
        box("%s_%d" % (prefix, i), x0, y0 + i * run, z_base, x1, y0 + i * run + run + 4, z_base + (i + 1) * sh)

# --- dimensions (uu) ---
W, D, T = 1920, 1600, 32          # X/Y half-extents, wall/floor thickness
LOW, PIT, UP, TOP = 0, -256, 448, 960
PX, PY = 448, 384                 # pit half-extents
TX = 192                          # central tower half-extent
CW = 512                          # catwalk depth

# --- perimeter walls (contain both levels) ---
box("wall_S", -W-T, -D-T, PIT-T,  W+T, -D,   TOP)
box("wall_N", -W-T,  D,   PIT-T,  W+T,  D+T, TOP)
box("wall_W", -W-T, -D,   PIT-T, -W,    D,   TOP)
box("wall_E",  W,   -D,   PIT-T,  W+T,  D,   TOP)

# --- lower floor, framed around the central pit hole ---
box("floor_S", -W, -D,  -T,  W, -PY, LOW)
box("floor_N", -W,  PY, -T,  W,  D,  LOW)
box("floor_W", -W, -PY, -T, -PX, PY, LOW)
box("floor_E",  PX,-PY, -T,  W,  PY, LOW)

# --- pit floor + lining walls (a ring around the tower base) ---
box("pit_floor", -PX, -PY, PIT-T, PX, PY, PIT)
box("pit_wW", -PX-T, -PY, PIT, -PX,   PY,   LOW)
box("pit_wE",  PX,   -PY, PIT,  PX+T, PY,   LOW)
box("pit_wS", -PX, -PY-T, PIT,  PX,  -PY,   LOW)
box("pit_wN", -PX,  PY,   PIT,  PX,   PY+T, LOW)

# --- central tower: solid column pit -> upper, walkable top ---
box("tower", -TX, -TX, PIT, TX, TX, UP)

# --- upper catwalk ring ---
box("cat_S", -W, -D,      UP-T,  W,   -D+CW, UP)
box("cat_N", -W,  D-CW,   UP-T,  W,    D,    UP)
box("cat_W", -W, -D+CW,   UP-T, -W+CW, D-CW, UP)
box("cat_E",  W-CW, -D+CW,UP-T,  W,    D-CW, UP)

# --- bridge: north catwalk -> tower top ---
box("bridge_N", -128, TX, UP-T, 128, D-CW, UP)

# --- stairs: lower floor -> catwalks (west & east) ---
stairs("stairW", -W+CW, -W+CW+256, -900, 85, 12, LOW, UP)
stairs("stairE",  W-CW-256, W-CW,  -900, 85, 12, LOW, UP)
# --- pit exit stair: pit floor -> north lower floor ---
stairs("pitexit", -408, -248, -300, 85, 8, PIT, LOW - PIT)

# --- jump pad: pit -> tower top (Deck's signature vertical move) ---
jumppads.append({"location": [300, 0, PIT], "target": [-300, 0, (UP - PIT) + 120]})

# --- cover: cylinder pillars on the lower floor ---
meshes.append({"mesh": "cylinder", "location": [-900, -900, 0], "scale": [1.4, 1.4, 2.4]})
meshes.append({"mesh": "cylinder", "location": [ 900,  900, 0], "scale": [1.4, 1.4, 2.4]})

# --- items across the levels ---
pickups += [
    {"type": "shieldbelt",      "location": [-300, 0, PIT + 64]},      # belt: pit ring (risk)
    {"type": "armor_chest",     "location": [0, 0, UP + 64]},          # vest: tower top (reward)
    {"type": "armor_thighpads", "location": [0, D - CW//2, UP + 64]},  # pads: north catwalk
    {"type": "health_medium",   "location": [-1200, 0, 64]},
    {"type": "health_medium",   "location": [ 1200, 0, 64]},
    {"type": "health_small",    "location": [0, -1200, 64]},
    {"type": "health_small",    "location": [0,  1200, 64]},
    {"type": "health_small",    "location": [-W + CW, 0, UP + 64]},    # west catwalk
]

# --- spawns: both levels ---
starts += [
    {"location": [-1400, -1200, 96], "yaw": 45},
    {"location": [ 1400,  1200, 96], "yaw": 225},
    {"location": [ 1400, -1200, 96], "yaw": 135},
    {"location": [-1400,  1200, 96], "yaw": -45},
    {"location": [-W + CW, -600, UP + 96], "yaw": 0},     # upper west catwalk
    {"location": [ W - CW,  600, UP + 96], "yaw": 180},   # upper east catwalk
]

spec = {
    "name": "DM-Deck",
    "gametype": "DM",
    "defaults": {"wallThickness": T, "floorThickness": T},
    "brushes": brushes,
    "jumpPads": jumppads,
    "meshes": meshes,
    "pickups": pickups,
    "playerStarts": starts,
    "source": {"layout": "hand-authored from Deck knowledge",
               "notes": "Multi-level: lower arena + pit, upper catwalk ring + tower, stairs + jump pad. Deck-inspired blockout, not a faithful clone."},
}

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "examples", "DM-Deck.mapspec.json")
with open(out, "w", encoding="utf-8") as f:
    json.dump(spec, f, indent=2)
print("wrote %s" % out)
print("%d brushes, %d spawns, %d pickups, %d jumppads, %d meshes" % (
    len(brushes), len(starts), len(pickups), len(jumppads), len(meshes)))
