#!/usr/bin/env python3
"""Write the synthetic structural_idw parity inputs (ESRI Shapefiles).

The data are invented (no field data). They exercise every acquisition and
constraint rule the upstream current-layers path applies:

* wells: strict inside, soft inclusion (outside the polygon, inside the 5 %
  bbox margin), outside (rejected), empty value (skipped; OCR_CONF must not be
  borrowed), value 1.2 (clipped to 1.0 by the 0..1 rule), value 3.5 (not a
  ratio, kept), duplicate coordinates, an is_ctrl=1 control point, a well
  inside the hole (rejected);
* boundary: concave polygon with one hole;
* directions: RATIO given, RATIO empty (dialog default 8), INF_RADIUS/CORE_R
  given, a line leaving the domain (clipped into pieces), ACTIVE=0;
* barriers: display_only crossing the value gradient (forces the local detour),
  empty BLK_MODE (full_block), soft (non-blocking, ignored), ACTIVE=0.

Usage: python make_structural_synthetic.py --reference <Drawing> --out <dir>
"""

from __future__ import annotations

import argparse
import math
import sys
import types
from pathlib import Path

DEFAULT_REF = Path("/home/kevin/projects/_refs/haiyou-visualization/Drawing")
PRJ = 'LOCAL_CS["Unspecified_Cartesian",UNIT["metre",1.0,AUTHORITY["EPSG","9001"]]]'


def shapefile_module(root: Path):
    sys.path.insert(0, str(root))
    for name, rel in (("drawing", "drawing"), ("drawing.io", "drawing/io")):
        module = types.ModuleType(name)
        module.__path__ = [str(root / rel)]
        module.__package__ = name
        sys.modules[name] = module
    from drawing.io import pyshp_lib
    return pyshp_lib


def lcg(seed):
    state = seed
    while True:
        state = (1103515245 * state + 12345) % (2 ** 31)
        yield state / float(2 ** 31)


def ring(points):
    pts = [tuple(map(float, p)) for p in points]
    if pts[0] != pts[-1]:
        pts.append(pts[0])
    return pts


def signed_area(pts):
    return 0.5 * sum(x0 * y1 - x1 * y0 for (x0, y0), (x1, y1) in zip(pts, pts[1:]))


def boundary_rings():
    # Concave outline (12 km x 7 km) with a notch on the north side.
    outer = [(0, 0), (4000, -300), (8000, 200), (12000, 0), (12300, 3500), (11800, 7000),
             (8200, 6800), (7400, 4300), (6000, 4100), (5200, 6900), (1500, 7100), (-200, 4200)]
    hole = [(9300, 1200), (10400, 1200), (10400, 2100), (9300, 2100)]
    outer, hole = ring(outer), ring(hole)
    if signed_area(outer) > 0:  # shapefile: exterior clockwise
        outer = outer[::-1]
    if signed_area(hole) < 0:   # holes counter-clockwise
        hole = hole[::-1]
    return outer, hole


def field_value(x, y):
    # Smooth ratio-like trend: high in the west, a ridge along y=3000.
    return 0.15 + 0.6 * math.exp(-((y - 3000.0) / 2200.0) ** 2) * (1.0 - x / 16000.0) + 0.1 * math.sin(x / 1700.0)


def well_rows():
    rnd = lcg(20261003)
    rows = []
    n = 0
    while len(rows) < 20:
        x = 300 + next(rnd) * 11500
        y = 300 + next(rnd) * 6400
        if 5300 < x < 7300 and y > 4300:   # notch
            continue
        if 9200 < x < 10500 and 1100 < y < 2200:  # hole
            continue
        n += 1
        rows.append(dict(well_id=f"SW{n:02d}", x=x, y=y, sand_ratio=round(field_value(x, y), 6),
                         is_ctrl=0, OCR_CONF=0.9))
    rows += [
        dict(well_id="SOFT_EDGE", x=12450, y=3300, sand_ratio=0.41, is_ctrl=0, OCR_CONF=0.8),
        dict(well_id="FAR_OUT", x=16000, y=9500, sand_ratio=0.7, is_ctrl=0, OCR_CONF=0.8),
        dict(well_id="NO_VALUE", x=3000, y=2000, sand_ratio=None, is_ctrl=0, OCR_CONF=0.0),
        dict(well_id="CLIP_HIGH", x=2500, y=3200, sand_ratio=1.2, is_ctrl=0, OCR_CONF=1.0),
        dict(well_id="DUP_A", x=8800, y=3000, sand_ratio=0.33, is_ctrl=0, OCR_CONF=1.0),
        dict(well_id="DUP_B", x=8800, y=3000, sand_ratio=0.39, is_ctrl=0, OCR_CONF=1.0),
        dict(well_id="CTRL_1", x=4500, y=5500, sand_ratio=0.62, is_ctrl=1, OCR_CONF=0.0),
        dict(well_id="IN_HOLE", x=9800, y=1600, sand_ratio=0.9, is_ctrl=0, OCR_CONF=1.0),
    ]
    return rows


DIRECTIONS = [
    dict(ID="D1", ACTIVE=1, RATIO=3.0, INF_RADIUS=0.0, CORE_R=0.0,
         pts=[(800, 2600), (3000, 3100), (5200, 2900), (7600, 3300)]),
    dict(ID="D2", ACTIVE=1, RATIO=None, INF_RADIUS=1500.0, CORE_R=400.0,
         pts=[(8200, 5800), (9600, 4600), (11000, 3600)]),
    dict(ID="D3", ACTIVE=1, RATIO=2.5, INF_RADIUS=0.0, CORE_R=0.0,
         pts=[(4600, 7600), (5600, 5200), (6800, 5300), (7000, 7700)]),
    dict(ID="D4", ACTIVE=0, RATIO=6.0, INF_RADIUS=0.0, CORE_R=0.0,
         pts=[(1000, 800), (3000, 1200)]),
]

BARRIERS = [
    dict(ID="B1", ACTIVE=1, BLK_MODE="display_only", pts=[(2200, 900), (2600, 2600), (3400, 4400), (3900, 6000)]),
    dict(ID="B2", ACTIVE=1, BLK_MODE=None, pts=[(9000, 4200), (10200, 5200), (11200, 6200)]),
    dict(ID="B3", ACTIVE=1, BLK_MODE="soft", pts=[(6500, 600), (7200, 2400)]),
    dict(ID="B4", ACTIVE=0, BLK_MODE="display_only", pts=[(600, 5000), (1800, 6200)]),
]


def write_all(shp, out: Path):
    out.mkdir(parents=True, exist_ok=True)

    def sidecars(stem):
        (out / f"{stem}.prj").write_text(PRJ, encoding="ascii")
        (out / f"{stem}.cpg").write_text("UTF-8", encoding="ascii")

    with shp.Writer(str(out / "wells"), shapeType=shp.POINT, encoding="utf-8") as w:
        w.field("well_id", "C", 40)
        w.field("sand_ratio", "N", 24, 15)
        w.field("is_ctrl", "N", 9, 0)
        w.field("OCR_CONF", "N", 24, 15)
        for row in well_rows():
            w.point(row["x"], row["y"])
            w.record(row["well_id"], row["sand_ratio"], row["is_ctrl"], row["OCR_CONF"])
    sidecars("wells")

    outer, hole = boundary_rings()
    with shp.Writer(str(out / "boundary"), shapeType=shp.POLYGON, encoding="utf-8") as w:
        w.field("NAME", "C", 40)
        w.poly([outer, hole])
        w.record("synthetic mapping boundary")
    sidecars("boundary")

    with shp.Writer(str(out / "directions"), shapeType=shp.POLYLINE, encoding="utf-8") as w:
        w.field("ID", "C", 20)
        w.field("ACTIVE", "N", 9, 0)
        w.field("RATIO", "N", 24, 15)
        w.field("INF_RADIUS", "N", 24, 15)
        w.field("CORE_R", "N", 24, 15)
        for d in DIRECTIONS:
            w.line([d["pts"]])
            w.record(d["ID"], d["ACTIVE"], d["RATIO"], d["INF_RADIUS"], d["CORE_R"])
    sidecars("directions")

    with shp.Writer(str(out / "barriers"), shapeType=shp.POLYLINE, encoding="utf-8") as w:
        w.field("ID", "C", 20)
        w.field("ACTIVE", "N", 9, 0)
        w.field("BLK_MODE", "C", 40)
        for b in BARRIERS:
            w.line([b["pts"]])
            w.record(b["ID"], b["ACTIVE"], b["BLK_MODE"] or "")  # pyshp writes None as "None" in C fields
    sidecars("barriers")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--reference", type=Path, default=DEFAULT_REF)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    write_all(shapefile_module(args.reference.resolve()), args.out)
    print(f"wrote synthetic structural inputs to {args.out}")


if __name__ == "__main__":
    main()
