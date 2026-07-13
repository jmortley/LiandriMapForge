#!/usr/bin/env python3
"""
LiandriMapForge tracer: elimplus layout PNG -> MapSpec -> (emit.py) -> T3D.

No eyeballing. Reads the actual silhouette, thresholds it to a solid/empty grid,
and emits floor plates matching the solid shape + walls at the empty cells that
border it. This reproduces the layout's FOOTPRINT faithfully.

Still flat: a 2D silhouette has no elevation. Footprint = faithful; Z = absent.

Deps:  pip install pillow
Usage:
    python trace.py capture/rankin.png --preview                 # verify the mask first
    python trace.py capture/rankin.png --cells 30 --scale 170 --name DM-Rankin
    python emit.py examples/DM-Rankin.mapspec.json --out out/
"""
import argparse, json, sys


def load_grid(path, cells, invert, thresh):
    try:
        from PIL import Image
    except ImportError:
        sys.exit("Pillow not installed. Run: pip install pillow")
    im = Image.open(path).convert("RGBA")
    W, H = im.size
    px = im.load()
    cw, ch = W / cells, H / cells
    grid = [[False] * cells for _ in range(cells)]
    for gy in range(cells):
        for gx in range(cells):
            x0, x1 = int(gx * cw), int((gx + 1) * cw)
            y0, y1 = int(gy * ch), int((gy + 1) * ch)
            solid = total = 0
            step = max(1, (x1 - x0) // 6)  # subsample for speed
            for y in range(y0, y1, step):
                for x in range(x0, x1, step):
                    r, g, b, a = px[x, y]
                    lum = (r + g + b) / 3.0
                    is_solid = (a > 128) and (lum < thresh)
                    if invert:
                        is_solid = (a > 128) and (lum >= thresh)
                    solid += 1 if is_solid else 0
                    total += 1
            grid[gy][gx] = (total > 0 and solid / total > 0.5)
    return grid


def show(grid):
    for row in grid:
        print("".join("#" if c else "." for c in row))


def greedy_rects(cellset):
    """Cover a set of (x,y) cells with maximal axis-aligned rectangles (greedy)."""
    remaining = set(cellset)
    rects = []
    while remaining:
        x0, y0 = min(remaining, key=lambda c: (c[1], c[0]))
        w = 0
        while (x0 + w, y0) in remaining:
            w += 1
        h = 0
        while all((x0 + i, y0 + h) in remaining for i in range(w)):
            h += 1
        for yy in range(y0, y0 + h):
            for xx in range(x0, x0 + w):
                remaining.discard((xx, yy))
        rects.append((x0, y0, w, h))
    return rects


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--cells", type=int, default=30, help="grid resolution (higher = more detail + more brushes)")
    ap.add_argument("--scale", type=float, default=170.0, help="uu per grid cell (ignored if --world-width given)")
    ap.add_argument("--world-width", type=float, default=None, help="uu the FULL image spans = StatSQL bounds extent (max_x-min_x). Layout PNG is the 1024px UTHUD minimap of the NavMesh bounds, so scale = width/cells.")
    ap.add_argument("--wall-height", type=float, default=512.0)
    ap.add_argument("--floor-thick", type=float, default=32.0)
    ap.add_argument("--thresh", type=float, default=200.0, help="luminance split; solid = darker than this")
    ap.add_argument("--invert", action="store_true", help="flip if walls/floor come out swapped")
    ap.add_argument("--preview", action="store_true", help="print the detected mask and exit")
    ap.add_argument("--name", default="DM-Rankin")
    ap.add_argument("--out", default=None)
    a = ap.parse_args()

    grid = load_grid(a.image, a.cells, a.invert, a.thresh)
    C = a.cells
    solid_n = sum(sum(r) for r in grid)

    if a.preview:
        show(grid)
        print("\n%d/%d solid cells. If '#' traces the FLOOR shape, generate; else add --invert." % (solid_n, C * C))
        return
    if solid_n == 0 or solid_n == C * C:
        sys.exit("Mask is all-or-nothing (%d/%d). Adjust --thresh or --invert; run --preview." % (solid_n, C * C))

    floor = {(x, y) for y in range(C) for x in range(C) if grid[y][x]}
    if a.world_width:
        S = a.world_width / C  # full image == the map's NavMesh bounds (UTHUD minimap = 1024px)
    else:
        S = a.scale
    print("scale: %.1f uu/cell (image spans %.0f uu across %d cells)" % (S, S * C, C))
    cx = C / 2.0
    wx = lambda gx: (gx - cx) * S
    wy = lambda gy: (cx - gy) * S  # flip so image-up = world +Y
    walls = set()
    for (x, y) in floor:
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if 0 <= nx < C and 0 <= ny < C and not grid[ny][nx]:
                walls.add((nx, ny))

    def box(name, rect, zlo, zhi):
        x0, y0, w, h = rect
        xa, xb = wx(x0), wx(x0 + w)
        ya, yb = wy(y0 + h), wy(y0)
        return {"name": name, "min": [min(xa, xb), min(ya, yb), zlo], "max": [max(xa, xb), max(ya, yb), zhi]}

    brushes = [box("floor_%d" % i, r, -a.floor_thick, 0.0) for i, r in enumerate(greedy_rects(floor))]
    brushes += [box("wall_%d" % i, r, 0.0, a.wall_height) for i, r in enumerate(greedy_rects(walls))]

    fc = sorted(floor, key=lambda c: (c[0] + c[1]))
    picks = [fc[0], fc[-1], fc[len(fc) // 2], fc[len(fc) // 4]]
    starts = [{"location": [wx(gx) + S / 2, wy(gy) - S / 2, 96], "yaw": yaw}
              for (gx, gy), yaw in zip(picks, (45, 225, 315, 135))]

    spec = {
        "name": a.name, "gametype": "DM",
        "defaults": {"wallHeight": a.wall_height, "wallThickness": 32, "floorThickness": a.floor_thick},
        "brushes": brushes,
        "playerStarts": starts,
        "lights": [
            {"type": "directional", "pitch": -52, "yaw": -35, "intensity": 8},
            {"type": "sky", "intensity": 2.0},
        ],
        "source": {"layout": a.image,
                   "notes": "Auto-traced silhouette @ %d cells, %.0fuu/cell. Flat: footprint faithful, no elevation." % (C, S)},
    }
    out = a.out or "examples/%s.mapspec.json" % a.name
    with open(out, "w", encoding="utf-8") as f:
        json.dump(spec, f, indent=2)
    print("wrote %s: %d brushes (%d floor, %d wall cells), %d spawns" % (out, len(brushes), len(floor), len(walls), len(starts)))


if __name__ == "__main__":
    main()
