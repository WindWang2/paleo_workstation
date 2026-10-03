#!/usr/bin/env python3
"""Compare two structural_idw JSON documents (golden vs candidate).

Both documents follow the run_structural_reference.py schema; a C++ dump only
needs the keys it produces (missing sections are reported, not failed, unless
--require is given). Nothing is rewritten.

Grid: finite masks must match; maxAbs <= 1e-9*S and RMSE <= 1e-10*S with
S = max(1, max|z|). Contours: per level, the same number of polylines with the
same open/closed state, matched one-to-one by minimum symmetric Hausdorff
distance; the worst match must be <= --contour-tol (map units, default
1e-6 * grid step). Orientation and start vertex are ignored.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path


def _grid_stats(gold, cand):
    gz, cz = gold["grid"]["z"], cand["grid"]["z"]
    if len(gz) != len(cz) or any(len(a) != len(b) for a, b in zip(gz, cz)):
        return dict(ok=False, reason="grid shape differs")
    finite_gold = [v for row in gz for v in row if v is not None]
    scale = max(1.0, max((abs(v) for v in finite_gold), default=1.0))
    mask_diff = 0
    errors = []
    for ra, rb in zip(gz, cz):
        for a, b in zip(ra, rb):
            if (a is None) != (b is None):
                mask_diff += 1
            elif a is not None:
                errors.append(abs(a - b))
    max_abs = max(errors, default=0.0)
    rmse = math.sqrt(sum(e * e for e in errors) / len(errors)) if errors else 0.0
    axes = max([abs(a - b) for a, b in zip(gold["grid"]["x"], cand["grid"]["x"])] +
               [abs(a - b) for a, b in zip(gold["grid"]["y"], cand["grid"]["y"])], default=0.0)
    ok = (mask_diff == 0 and max_abs <= 1e-9 * scale and rmse <= 1e-10 * scale
          and len(gold["grid"]["x"]) == len(cand["grid"]["x"]) and len(gold["grid"]["y"]) == len(cand["grid"]["y"]))
    vm = sum(1 for ra, rb in zip(gold["grid"]["valid_mask"], cand["grid"].get("valid_mask", gold["grid"]["valid_mask"]))
             for a, b in zip(ra, rb) if a != b)
    return dict(ok=ok and vm == 0, mask_diff=mask_diff, valid_mask_diff=vm, max_abs=max_abs, rmse=rmse,
                scale=scale, axes_max_abs=axes)


def _seg_dist(p, a, b):
    ax, ay = a
    bx, by = b
    px, py = p
    vx, vy = bx - ax, by - ay
    ll = vx * vx + vy * vy
    t = 0.0 if ll <= 0 else max(0.0, min(1.0, ((px - ax) * vx + (py - ay) * vy) / ll))
    return math.hypot(px - (ax + t * vx), py - (ay + t * vy))


def _directed(a, b):
    worst = 0.0
    for p in a:
        best = min((_seg_dist(p, b[i], b[i + 1]) for i in range(len(b) - 1)), default=math.inf)
        worst = max(worst, best)
    return worst


def hausdorff(a, b):
    return max(_directed(a, b), _directed(b, a))


def _closed(line):
    return len(line) > 2 and line[0] == line[-1]


def _contour_stats(gold_map, cand_map, tol):
    report = {}
    ok = True
    for level, gold_lines in gold_map.items():
        cand_lines = cand_map.get(level)
        if cand_lines is None:
            # tolerate float formatting differences in keys
            match = [k for k in cand_map if abs(float(k) - float(level)) <= 1e-12]
            cand_lines = cand_map[match[0]] if match else []
        entry = dict(gold=len(gold_lines), cand=len(cand_lines))
        if len(gold_lines) != len(cand_lines):
            entry["ok"] = False
            ok = False
            report[level] = entry
            continue
        remaining = list(range(len(cand_lines)))
        worst = 0.0
        closed_mismatch = 0
        for g in gold_lines:
            best_j, best_d = None, math.inf
            for j in remaining:
                d = hausdorff(g, cand_lines[j])
                if d < best_d:
                    best_j, best_d = j, d
            remaining.remove(best_j)
            worst = max(worst, best_d)
            closed_mismatch += int(_closed(g) != _closed(cand_lines[best_j]))
        entry.update(hausdorff_max=worst, closed_mismatch=closed_mismatch, ok=worst <= tol and closed_mismatch == 0)
        ok &= entry["ok"]
        report[level] = entry
    extra = sorted(set(cand_map) - set(gold_map))
    if extra:
        ok = False
        report["extra_levels"] = extra
    return ok, report


def compare(gold, cand, contour_tol=None, require=()):
    out = dict()
    ok = True
    if "grid" in cand:
        out["grid"] = _grid_stats(gold, cand)
        ok &= out["grid"]["ok"]
    elif "grid" in require:
        ok = False
        out["grid"] = "missing"
    if "wells" in cand:
        key = lambda w: (w["well_id"], w["x"], w["y"], w["value"], bool(w.get("is_control_point")))
        same = [key(w) for w in gold["wells"]] == [key(w) for w in cand["wells"]]
        out["wells"] = dict(ok=same, gold=len(gold["wells"]), cand=len(cand["wells"]))
        ok &= same
    elif "wells" in require:
        ok = False
        out["wells"] = "missing"
    step = max(abs(gold["grid"]["x"][1] - gold["grid"]["x"][0]), abs(gold["grid"]["y"][1] - gold["grid"]["y"][0]))
    tol = contour_tol if contour_tol is not None else 1e-6 * step
    for key in ("initial_contours", "contours"):
        if key in cand and cand[key] is not None and gold.get(key) is not None:
            good, rep = _contour_stats(gold[key], cand[key], tol)
            out[key] = dict(ok=good, tolerance=tol, levels=rep)
            ok &= good
        elif key in require:
            ok = False
            out[key] = "missing"
    out["ok"] = bool(ok)
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("golden", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--contour-tol", type=float)
    parser.add_argument("--require", action="append", default=[])
    args = parser.parse_args()
    gold = json.loads(args.golden.read_text(encoding="utf-8"))
    cand = json.loads(args.candidate.read_text(encoding="utf-8"))
    result = compare(gold, cand, args.contour_tol, tuple(args.require))
    print(json.dumps(result, ensure_ascii=False, indent=1))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
