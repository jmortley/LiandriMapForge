#!/usr/bin/env python3
"""
DM-DeckStyle: an ORIGINAL elongated multi-level arena, structured off UT4 Deck's
real screenshots -- a long symmetric spine flanked by green TOXIC-SLUDGE channels,
outer + upper walkways, cross-bridges, and jump pads to escape the sludge. Built at
real UT4 scale (~Rankin). Deck-*informed original*, not a clone (that's just pasting
deck.t3d). Writes examples/DM-DeckStyle.mapspec.json -> feed to emit.py.
"""
import json, os

brushes, starts, pickups, jumppads, meshes = [], [], [], [], []
TOXIC = "/Game/RestrictedAssets/Environments/Materials/SlimePit.SlimePit"   # real DM-Deck sludge

def box(name, x0, y0, z0, x1, y1, z1, material=None):
    b = {"name": name, "min": [x0, y0, z0], "max": [x1, y1, z1]}
    if material:
        b["material"] = material
    brushes.append(b)

def stairs(prefix, x0, x1, y0, z_base, rise, step_h=46, run=105):
    """Ascending staircase (+Y). Step COUNT derived from rise so steps stay walkable
    at any scale. Returns (top_y, top_z)."""
    steps = max(2, round(rise / step_h))
    sh = rise / steps
    for i in range(steps):
        box("%s_%d" % (prefix, i), x0, y0 + i*run, z_base, x1, y0 + i*run + run + 6, z_base + (i+1)*sh)
    return y0 + steps*run, z_base + rise

# --- dimensions (uu), real UT4 scale ~ Rankin ---
W, L, T = 2600, 4800, 40           # X/Y half-extents, thickness
SLU, MAIN, UP, TOP = -448, 0, 640, 1600
SPX = 350                          # spine half-width
SIN, SOUT = 350, 1500              # sludge X inner / outer
SY = 3600                          # sludge Y half-length
CW = 850                           # upper walkway depth
UPIN_W, UPIN_E = -(W - CW), (W - CW)   # upper-walkway inner edges: -1750 / +1750

# --- perimeter walls ---
box("wall_W", -W-T, -L-T, SLU-T, -W,   L+T, TOP)
box("wall_E",  W,   -L-T, SLU-T,  W+T, L+T, TOP)
box("wall_S", -W-T, -L-T, SLU-T,  W+T,-L,   TOP)
box("wall_N", -W-T,  L,   SLU-T,  W+T, L+T, TOP)

# --- main-floor blocks, thick (z SLU..MAIN) so their faces wall the sludge ---
box("spine",  -SPX, -SY, SLU, SPX,  SY, MAIN)
box("walk_W", -W,   -SY, SLU, -SOUT,SY, MAIN)
box("walk_E",  SOUT,-SY, SLU,  W,   SY, MAIN)
box("end_S",  -W,   -L,  SLU,  W,  -SY, MAIN)
box("end_N",  -W,    SY, SLU,  W,   L,  MAIN)

# --- toxic sludge channels ---
box("sludge_W", -SOUT, -SY, SLU-T, -SIN, SY, SLU, TOXIC)
box("sludge_E",  SIN,  -SY, SLU-T,  SOUT,SY, SLU, TOXIC)

# --- cross-bridges over the sludge (main level) ---
BRIDGE_Y = (-2000, 0, 2000)
for i, yy in enumerate(BRIDGE_Y):
    box("bridge_%d" % i, -SOUT, yy-160, -T, SOUT, yy+160, MAIN)

# --- upper walkway ring ---
box("up_W", -W,    -4200, UP-T, UPIN_W, 4200, UP)
box("up_E",  UPIN_E,-4200, UP-T, W,     4200, UP)
box("up_endS", -W, -4200, UP-T, W,     -3400, UP)
box("up_endN", -W,  3400, UP-T, W,      4200, UP)

# --- stairs: main -> upper, in the open strip on each walkway's INNER edge ---
stairs("stW", UPIN_W, -SOUT, -700, MAIN, UP - MAIN)   # X[-1750,-1500], top meets up_W at X=-1750
stairs("stE",  SOUT,  UPIN_E, -700, MAIN, UP - MAIN)  # X[1500,1750],  top meets up_E at X=+1750

# --- jump pads: escape the sludge, CLEAR of every bridge, land on the walkway ---
for side, jx, jtx in ((-1, -900, -1275), (1, 900, 1275)):
    jumppads.append({"location": [jx, 1200, SLU], "target": [jtx, 0, (UP - SLU) + 200]})

# --- cover crates on the outer walkways ---
meshes.append({"mesh": "cube", "location": [-2050, -2500, 0], "scale": [4, 4, 3]})
meshes.append({"mesh": "cube", "location": [ 2050,  2500, 0], "scale": [4, 4, 3]})

# --- items ---
pickups += [
    {"type": "shieldbelt",      "location": [0, 0, 64]},              # spine, exposed over sludge
    {"type": "armor_chest",     "location": [-2175, 0, UP + 64]},     # upper west
    {"type": "armor_thighpads", "location": [ 2175, 0, UP + 64]},     # upper east
    {"type": "health_medium",   "location": [0, -4200, 64]},          # south end
    {"type": "health_medium",   "location": [0,  4200, 64]},          # north end
    {"type": "health_small",    "location": [-2050, 2200, 64]},
    {"type": "health_small",    "location": [ 2050, -2200, 64]},
]

# --- spawns: ends + main + upper ---
starts += [
    {"location": [0, -4400, 96], "yaw": 90},
    {"location": [0,  4400, 96], "yaw": -90},
    {"location": [-2050, -2800, 96], "yaw": 45},
    {"location": [ 2050,  2800, 96], "yaw": 225},
    {"location": [-2175, -1500, UP + 96], "yaw": 0},
    {"location": [ 2175,  1500, UP + 96], "yaw": 180},
]

spec = {
    "name": "DM-DeckStyle", "gametype": "DM",
    "defaults": {"wallThickness": T, "floorThickness": T, "artProfile": "deck"},
    "brushes": brushes, "jumpPads": jumppads, "meshes": meshes,
    "pickups": pickups, "playerStarts": starts,
    "source": {"layout": "original, structured off UT4 Deck screenshots, Rankin scale",
               "notes": "Elongated symmetric spine + twin sludge channels, outer/upper walkways, cross-bridges, jump pads. Deck-informed original."},
}
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "examples", "DM-DeckStyle.mapspec.json")
json.dump(spec, open(out, "w", encoding="utf-8"), indent=2)
print("wrote", out)
print("%d brushes, %d spawns, %d pickups, %d jumppads, %d meshes" % (
    len(brushes), len(starts), len(pickups), len(jumppads), len(meshes)))
