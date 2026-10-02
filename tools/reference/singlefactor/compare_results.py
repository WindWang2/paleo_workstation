#!/usr/bin/env python3
"""Compare a candidate kernel JSON against a frozen golden. Does not rewrite the golden."""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path


def _scale(values):
    finite = [abs(value) for value in values if value is not None]
    return max(1.0, max(finite) if finite else 1.0)


def compare(golden, candidate):
    failures = []
    by_name = {case["name"]: case for case in candidate["cases"]}
    for expected in golden["cases"]:
        actual = by_name.get(expected["name"])
        if actual is None:
            failures.append(f"{expected['name']}: missing")
            continue
        if expected["finite"] != actual["finite"]:
            failures.append(f"{expected['name']}: finite mask differs")
            continue
        scale = _scale(expected["values"])
        errors = []
        for left, right, ok in zip(expected["values"], actual["values"], expected["finite"]):
            if not ok:
                continue
            errors.append(abs(left - right))
        if not errors:
            continue
        max_abs = max(errors)
        rmse = math.sqrt(sum(error * error for error in errors) / len(errors))
        if max_abs > 1e-9 * scale or rmse > 1e-10 * scale:
            failures.append(
                f"{expected['name']}: maxAbs={max_abs:.3e} rmse={rmse:.3e} scale={scale:.6g}"
            )
    return failures


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("golden", type=Path)
    parser.add_argument("candidate", type=Path)
    args = parser.parse_args()
    golden = json.loads(args.golden.read_text(encoding="utf-8"))
    candidate = json.loads(args.candidate.read_text(encoding="utf-8"))
    failures = compare(golden, candidate)
    if failures:
        print("FAIL")
        for line in failures:
            print(line)
        return 1
    print(f"PASS {len(golden['cases'])} cases")
    return 0


if __name__ == "__main__":
    sys.exit(main())
