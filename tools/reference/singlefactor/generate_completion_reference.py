#!/usr/bin/env python3
"""Generate completion goldens (corridor/partition/fault-path) from the frozen reference.

Extends the kernel_parity fixture family for direction 23: point projections,
partition labels and fault-path detour distances come straight from the frozen
haiyou-visualization modules at 27fdb99. Formulas are not modified.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import platform
import sys
import types
from pathlib import Path

REFERENCE_SHA = "27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f"
DEFAULT_REF = Path("/home/kevin/projects/_refs/haiyou-visualization/Drawing")
SOURCE_FILES = (
    "drawing/single_factor/direction_corridor.py",
    "drawing/single_factor/partition.py",
    "drawing/single_factor/continuous_metric.py",
    "drawing/single_factor/surfer_idw.py",
)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    digest.update(path.read_bytes())
    return digest.hexdigest()


def load_reference(drawing_root: Path):
    root = drawing_root.resolve()
    if not (root / "drawing" / "single_factor" / "partition.py").is_file():
        raise SystemExit(f"reference numeric sources not found under {root}")
    if str(root) not in sys.path:
        sys.path.insert(0, str(root))
    for name, relative in (
        ("drawing", "drawing"),
        ("drawing.single_factor", "drawing/single_factor"),
    ):
        module = types.ModuleType(name)
        module.__path__ = [str(root / relative)]
        module.__package__ = name
        sys.modules[name] = module
    from drawing.single_factor import continuous_metric, direction_corridor, partition

    return direction_corridor, partition, continuous_metric


def corridor_cases(direction_corridor):
    spec = direction_corridor.DirectionLineSpec(
        line_id="bend",
        points=((0.0, 1.0), (6.0, 1.0), (6.0, 3.0)),
        active=True,
        ratio=8.0,
        influence_radius=3.0,
        core_radius=1.0,
        zone_id="",
        extend_mode="none",
        transition=0.0,
    )
    geom = direction_corridor.build_polyline_geometry(spec, index=0, extend_distance=0.0)
    queries = [
        [3.0, 1.0],
        [6.0, 2.0],
        [3.0, 3.0],
        [4.0, 0.0],
        [0.0, 1.5],
        [6.5, 3.5],
        [3.0, 4.05],
        [3.0, 1.9],
    ]
    expected = []
    for x, y in queries:
        s, n, tx, ty, dist = direction_corridor.project_point_to_polyline((x, y), geom)
        g = direction_corridor.combined_influence(dist, s, geom)
        expected.append(
            {"s": s, "n": n, "tx": tx, "ty": ty, "distance": dist, "g": g}
        )
    return {
        "kind": "corridor_projection",
        "name": "corridor_bend_projection",
        "line": {
            "points": [[p[0], p[1]] for p in spec.points],
            "ratio": spec.ratio,
            "influence_radius": spec.influence_radius,
            "core_radius": spec.core_radius,
            "extend_mode": spec.extend_mode,
        },
        "queries": queries,
        "expected": expected,
    }


def faultpath_cases(continuous_metric):
    class Barrier:
        def __init__(self, points):
            self.points = points

    wells = [[2.0, 5.0], [8.0, 5.0], [5.0, 9.0]]
    wall = Barrier([(5.0, 0.0), (5.0, 10.0)])
    queries = [[4.0, 5.0], [6.0, 5.0], [4.5, 6.0], [5.0, 5.0], [0.0, 0.0]]
    metric = continuous_metric.FaultPathMetric(
        __import__("numpy").array(wells, dtype=float), [wall]
    )
    matrix = metric.distances(__import__("numpy").array(queries, dtype=float))
    return {
        "kind": "faultpath_distance",
        "name": "faultpath_straight_wall",
        "wells": wells,
        "barrier": [[5.0, 0.0], [5.0, 10.0]],
        "queries": queries,
        "distances": [[float(v) for v in row] for row in matrix],
    }


def partition_cases(partition):
    import numpy as np

    class BarrierLine:
        def __init__(self, line_id, points):
            self.line_id = line_id
            self.points = points
            self.active = True
            self.block_mode = "full_block"
            self.priority = 1

    class Boundary:
        def __init__(self, exterior):
            self.exterior = exterior
            self.holes = ()

    # 上游生产路径（surfer_idw.build_global_surface）以升序 ys 喂分区；
    # assign_well_regions 的越界守卫也只认升序 y。夹具按真实用法生成。
    cols, rows = 12, 10
    xs = np.linspace(0.5, cols - 0.5, cols)
    ys = np.linspace(0.5, rows - 0.5, rows)  # 升序：行 0 最南
    domain = np.ones((rows, cols), dtype=bool)
    wells = [[2.0, 5.0], [10.0, 5.0], [2.0, 8.0], [10.0, 2.0]]
    barriers = [BarrierLine("w", ((6.0, 3.0), (6.0, 7.0)))]
    boundary = Boundary(((0.0, 0.0), (12.0, 0.0), (12.0, 10.0), (0.0, 10.0), (0.0, 0.0)))
    # 标签号按种子发现顺序编号，南北扫描起点不同会置换 id——对拍用
    # 置换不变量：region_count + 探针格/井的「同区」关系矩阵。
    probes = [(2, 4), (9, 4), (5, 8), (5, 1), (0, 0), (11, 9), (2, 8), (10, 2)]

    def same_region_matrix(labels, coords):
        n = len(coords)
        matrix = []
        for i in range(n):
            row = []
            for j in range(n):
                li = labels[coords[i][1], coords[i][0]]
                lj = labels[coords[j][1], coords[j][0]]
                row.append(bool(li >= 0 and li == lj))
            matrix.append(row)
        return matrix

    result = {}
    for interpretation in (False, True):
        part = partition.build_partition(
            xs, ys, domain, barriers, np.array(wells, dtype=float),
            boundaries=(boundary,), interpretation=interpretation,
        )
        # 井的「同区」矩阵按 sampleComponent 语义（pending=-3 视为不与任何井同区）。
        well_labels = [int(v) for v in part.well_region_ids.tolist()]
        n = len(wells)
        well_same = [[bool(well_labels[i] >= 0 and well_labels[i] == well_labels[j])
                      for j in range(n)] for i in range(n)]
        result["interpretation" if interpretation else "local"] = {
            "region_count": int(part.region_count),
            "probe_same_region": same_region_matrix(part.region_ids, probes),
            "well_same_region": well_same,
            "extension_count": len(part.extensions),
        }
    return {
        "kind": "partition_labels",
        "name": "partition_free_end_wall",
        "cols": cols,
        "rows": rows,
        "wells": wells,
        "barrier": [[6.0, 3.0], [6.0, 7.0]],
        "boundary": [[0.0, 0.0], [12.0, 0.0], [12.0, 10.0], [0.0, 10.0], [0.0, 0.0]],
        "probes": [[int(c), int(r)] for c, r in probes],
        "result": result,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--reference", type=Path, default=DEFAULT_REF)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()
    direction_corridor, partition, continuous_metric = load_reference(args.reference)
    import numpy
    import scipy
    import shapely

    cases = [
        corridor_cases(direction_corridor),
        faultpath_cases(continuous_metric),
        partition_cases(partition),
    ]
    payload = {
        "schema": "paleo_singlefactor_kernel_golden_completion_v1",
        "referenceRevision": REFERENCE_SHA,
        "note": "Direction-23 completion goldens: corridor projection, fault-path distances, partition labels.",
        "cases": cases,
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
        "adapter": "generate_completion_reference.py",
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
    print(f"wrote {args.out} ({len(cases)} cases)")
    print(f"wrote {args.manifest}")


if __name__ == "__main__":
    main()
