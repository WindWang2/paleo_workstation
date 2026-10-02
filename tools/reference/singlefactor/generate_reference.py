#!/usr/bin/env python3
"""Generate numeric goldens from the frozen haiyou-visualization reference.

This script imports the numeric modules only. Parent packages are registered
as empty modules so Drawing/drawing/__init__.py (PyQt6) and
drawing/single_factor/__init__.py (workflow UI) are not executed.
numpy, scipy, and shapely are real imports. Formulas are not modified.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import sys
import types
from pathlib import Path

REFERENCE_SHA = "27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f"
DEFAULT_REF = Path("/home/kevin/projects/_refs/haiyou-visualization/Drawing")
SOURCE_FILES = (
    "drawing/constraint_semantics.py",
    "drawing/single_factor/constrained_engine.py",
    "drawing/single_factor/continuous_metric.py",
    "drawing/single_factor/surfer_idw.py",
    "drawing/single_factor/structural_idw.py",
    "drawing/single_factor/interpretive_boundary.py",
    "drawing/single_factor/well_clusters.py",
    "drawing/single_factor/contour_work_field.py",
)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    digest.update(path.read_bytes())
    return digest.hexdigest()


def load_reference(drawing_root: Path):
    root = drawing_root.resolve()
    if not (root / "drawing" / "single_factor" / "structural_idw.py").is_file():
        raise SystemExit(f"reference numeric sources not found under {root}")
    if str(root) not in sys.path:
        sys.path.insert(0, str(root))
    for name, relative in (
        ("drawing", "drawing"),
        ("drawing.single_factor", "drawing/single_factor"),
    ):
        existing = sys.modules.get(name)
        if existing is not None and getattr(existing, "__file__", None):
            raise SystemExit(f"{name} was already imported from {existing.__file__}; refuse to hide that import")
        module = types.ModuleType(name)
        module.__path__ = [str(root / relative)]
        module.__package__ = name
        sys.modules[name] = module
    from drawing.single_factor.constrained_engine import DirectionLine
    from drawing.single_factor.structural_idw import LocalInterpolator

    return LocalInterpolator, DirectionLine


def _finite(values):
    out = []
    mask = []
    for value in values:
        number = float(value)
        if number == number and abs(number) != float("inf"):
            out.append(number)
            mask.append(True)
        else:
            out.append(None)
            mask.append(False)
    return out, mask


def _run(LocalInterpolator, DirectionLine, case):
    directions = []
    for item in case.get("directions", []):
        directions.append(
            DirectionLine(
                line_id=item.get("id", "direction"),
                points=tuple(tuple(point) for point in item["points"]),
                active=bool(item.get("active", True)),
                ratio=float(item["ratio"]),
                influence_radius=float(item["influence_radius"]),
                core_radius=float(item["core_radius"]),
            )
        )
    field = LocalInterpolator(
        case["wells"],
        directions=directions,
        power=float(case["power"]),
        region=None,
        walls=(),
        barrier_shapes=(),
        search_radius=case["search_radius"],
        min_points=int(case["min_points"]),
        max_points=int(case["max_points"]),
        cluster_span=float(case["cluster_span"]),
        interpretive_boundaries=case.get("soft", []),
    )
    values, influence = field.evaluate(case["queries"])
    stored, mask = _finite(values)
    influence_stored, _ = _finite(influence)
    return {
        "name": case["name"],
        "profile": case["profile"],
        "wells": case["wells"],
        "queries": case["queries"],
        "power": case["power"],
        "min_points": case["min_points"],
        "max_points": case["max_points"],
        "search_radius": case["search_radius"],
        "cluster_span": case["cluster_span"],
        "directions": case.get("directions", []),
        "soft": case.get("soft", []),
        "values": stored,
        "finite": mask,
        "influence": influence_stored,
    }


def cases():
    wells = [[0.0, 0.0, 1.0], [4.0, 0.0, 3.0], [0.0, 4.0, 5.0], [4.0, 4.0, 9.0]]
    queries = [[1.0, 2.0], [4.0, 4.0], [2.0, 2.0], [8.0, 1.0], [0.0, 0.0]]
    base = dict(wells=wells, queries=queries, power=2.0, min_points=1, max_points=0,
                search_radius=None, cluster_span=0.0, directions=[], soft=[])
    direction = {
        "id": "d1",
        "points": [[0.0, 1.0], [6.0, 1.0], [6.0, 3.0]],
        "active": True,
        "ratio": 8.0,
        "influence_radius": 3.0,
        "core_radius": 0.9,
    }
    soft = {"points": [[2.0, -1.0], [2.0, 5.0]], "radius": 2.5, "strength": 0.35}
    soft_b = {"points": [[1.0, -1.0], [1.0, 5.0]], "radius": 2.5, "strength": 0.5}
    items = [
        {**base, "name": "plain_idw", "profile": "reference_compatible"},
        {**base, "name": "duplicate_rows", "profile": "reference_compatible",
         "wells": [[0.0, 0.0, 1.0], [0.0, 0.0, 3.0], [4.0, 0.0, 9.0]],
         "queries": [[0.0, 0.0], [1.0, 0.0], [2.0, 0.0]]},
        {**base, "name": "max_points_truncation", "profile": "reference_compatible",
         "max_points": 1, "queries": [[1.0, 0.0], [2.0, 2.0]]},
        {**base, "name": "search_taper", "profile": "product_explicit",
         "search_radius": 3.0, "queries": [[1.0, 1.0], [6.0, 0.0]]},
        {**base, "name": "direction_local", "profile": "reference_compatible",
         "directions": [direction]},
        {**base, "name": "direction_ratio_one", "profile": "reference_compatible",
         "directions": [{**direction, "ratio": 1.0}]},
        {**base, "name": "direction_reversed", "profile": "reference_compatible",
         "directions": [{**direction, "points": list(reversed(direction["points"]))}]},
        {**base, "name": "direction_far", "profile": "reference_compatible",
         "directions": [{**direction, "points": [[100.0, 100.0], [130.0, 100.0]]}]},
        {**base, "name": "soft_boundary", "profile": "reference_compatible", "soft": [soft]},
        {**base, "name": "soft_overlap", "profile": "reference_compatible", "soft": [soft, soft_b]},
        {**base, "name": "soft_strength_zero", "profile": "reference_compatible",
         "soft": [{**soft, "strength": 0.0}]},
        {**base, "name": "direction_and_soft", "profile": "reference_compatible",
         "directions": [direction], "soft": [soft]},
        {**base, "name": "well_clusters", "profile": "reference_compatible",
         "wells": [[0.0, 0.0, 1.0], [1.0, 0.2, 2.0], [0.2, 1.0, 3.0],
                   [20.0, 0.0, 8.0], [21.0, 0.4, 9.0], [20.3, 1.0, 10.0]],
         "queries": [[0.4, 0.4], [10.0, 0.5], [40.0, 0.0]],
         "cluster_span": 30.0},
    ]
    return items


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--reference", type=Path, default=DEFAULT_REF)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()
    LocalInterpolator, DirectionLine = load_reference(args.reference)
    import numpy
    import scipy
    import shapely

    payload = {
        "schema": "paleo_singlefactor_kernel_golden_v1",
        "referenceRevision": REFERENCE_SHA,
        "note": "NaN is null. finite[] is the nodata mask. Hard barriers are not in these cases.",
        "cases": [_run(LocalInterpolator, DirectionLine, case) for case in cases()],
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(payload, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    files = []
    for relative in SOURCE_FILES:
        path = args.reference.resolve() / relative
        files.append({"path": relative, "sha256": _sha256(path)})
    manifest = {
        "referenceRevision": REFERENCE_SHA,
        "referenceRoot": str(args.reference.resolve()),
        "adapter": "generate_reference.py",
        "adapterVersion": "1.0.0",
        "python": sys.version,
        "platform": platform.platform(),
        "packages": {
            "numpy": numpy.__version__,
            "scipy": scipy.__version__,
            "shapely": shapely.__version__,
        },
        "sources": files,
        "golden": str(args.out),
        "goldenSha256": _sha256(args.out),
    }
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {args.out} ({len(payload['cases'])} cases)")
    print(f"wrote {args.manifest}")


if __name__ == "__main__":
    main()
