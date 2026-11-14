# Optional host-locked template; inputs are bounded data, never code or paths.
import hashlib
import json
import math
import struct
import sys

import numpy as np


def read_exact(size):
    if type(size) is not int or not 0 <= size <= 2 * 1024 * 1024:
        raise ValueError("analysis_frame_budget")
    value = sys.stdin.buffer.read(size)
    if len(value) != size:
        raise ValueError("analysis_frame_incomplete")
    return value


def load_input():
    length = struct.unpack("<I", read_exact(4))[0]
    if not 1 <= length <= 65536:
        raise ValueError("analysis_header_budget")
    meta = json.loads(read_exact(length).decode("utf-8"))
    if meta.get("contract") != "paleo.python-analysis-input.v1" or meta.get("analysis") != "well_quality":
        raise ValueError("analysis_contract")
    if meta.get("inputProtocol") != "paleo.python-analysis-frame.v1" or meta.get("sourceKind") != "well_curve_numeric_attributes":
        raise ValueError("analysis_source_kind")
    shape = meta.get("shape")
    if type(shape) is not list or len(shape) != 2 or any(type(v) is not int for v in shape):
        raise ValueError("analysis_shape")
    rows, channels = shape
    if not 0 <= rows <= 8192 or not 1 <= channels <= 16 or meta.get("dtype") != "<f8":
        raise ValueError("analysis_shape_budget")
    expected = rows * (8 + 9 * channels)
    layout = {"axisBytes": rows * 8, "valueBytes": rows * channels * 8, "qualityBytes": rows * channels,
              "order": "axis_then_row_major_values_then_row_major_quality"}
    if meta.get("layout") != layout or meta.get("payload", {}).get("bytes") != expected:
        raise ValueError("analysis_layout")
    payload = read_exact(expected)
    if sys.stdin.buffer.read(1) or hashlib.sha256(payload).hexdigest() != meta["payload"].get("sha256"):
        raise ValueError("analysis_payload_integrity")
    axis = np.frombuffer(payload, dtype="<f8", count=rows)
    values = np.frombuffer(payload, dtype="<f8", count=rows * channels, offset=rows * 8).reshape(rows, channels)
    quality = np.frombuffer(payload, dtype=np.uint8, count=rows * channels, offset=rows * 8 + rows * channels * 8).reshape(rows, channels)
    if not np.all(np.isfinite(axis)) or np.any(np.diff(axis) <= 0) or np.any(quality & 248):
        raise ValueError("analysis_axis_or_quality")
    columns = meta.get("columns")
    convention = meta.get("axis", {})
    if type(columns) is not list or len(columns) != channels or any(not c.get("unit") for c in columns):
        raise ValueError("analysis_column_semantics")
    if convention.get("kind") != "MD" or convention.get("unit") not in ("m", "ft") or not convention.get("reference") or convention.get("alignment") != "exact_shared_axis_v1":
        raise ValueError("analysis_axis_semantics")
    sampling = meta.get("provenance", {}).get("sampling", {})
    if sampling != {"method": "all_selected_window_v1", "seed": 0, "count": rows, "scope": "selected_window_only", "random": False, "extrapolation": False}:
        raise ValueError("analysis_sampling")
    return meta, values, quality


meta, values, quality = load_input()
rows, channels = values.shape
metrics = []
for index in range(channels):
    column, flags = values[:, index], quality[:, index]
    finite = np.isfinite(column)
    valid = finite & ((flags & 3) == 0)
    selected = column[valid]
    count = int(selected.size)
    result = {"curveId": meta["columns"][index]["curveId"], "name": meta["columns"][index]["name"],
              "unit": meta["columns"][index]["unit"], "sampleCount": rows, "validCount": count,
              "missingCount": int(np.count_nonzero(((flags & 1) != 0) | ~finite)),
              "nonFiniteCount": int(np.count_nonzero(~finite)), "sdkAbnormalCount": int(np.count_nonzero(flags & 2)),
              "editedCount": int(np.count_nonzero(flags & 4)), "coverage": count / rows if rows else None,
              "minimum": None, "maximum": None, "mean": None, "populationStdDev": None,
              "zScoreOutlierCount": 0, "zScoreThreshold": 3.0,
              "status": "empty_window" if not rows else "all_missing"}
    if count:
        scale = float(np.max(np.abs(selected)))
        normalized = selected / scale if scale else selected
        mean, deviation = float(np.mean(normalized)), float(np.std(normalized))
        result.update(minimum=float(np.min(selected)), maximum=float(np.max(selected)),
                      mean=mean * scale if scale else 0.0, populationStdDev=deviation * scale,
                      status="constant" if deviation == 0 else "ok")
        if deviation:
            result["zScoreOutlierCount"] = int(np.count_nonzero(np.abs(normalized - mean) > 3.0 * deviation))
    metrics.append(result)
report = {"contract": "paleo.well-quality-report.v1", "sourceKind": "well_curve_numeric_attributes",
          "scope": "selected_window_only", "metrics": metrics, "provenance": meta["provenance"],
          "recommendations": ["Review missing and abnormal samples before interpretation; source curves remain unchanged."],
          "limitations": ["Metrics describe only the declared windows, not the complete well or survey.",
                          "A fixed population z-score threshold is descriptive and does not establish geological validity."]}
json.dump({"contract": "paleo.python-output.v1", "ports": {"REPORT": report}}, sys.stdout,
          allow_nan=False, separators=(",", ":"))
