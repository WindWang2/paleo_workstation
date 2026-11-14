# Optional host-locked template; inputs are bounded data, never code or paths.
import hashlib
import json
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
    if meta.get("contract") != "paleo.python-analysis-input.v1" or meta.get("analysis") != "attribute_correlation":
        raise ValueError("analysis_contract")
    if meta.get("inputProtocol") != "paleo.python-analysis-frame.v1" or meta.get("sourceKind") != "well_curve_numeric_attributes":
        raise ValueError("analysis_source_kind")
    shape = meta.get("shape")
    if type(shape) is not list or len(shape) != 2 or any(type(v) is not int for v in shape):
        raise ValueError("analysis_shape")
    rows, channels = shape
    if not 0 <= rows <= 8192 or not 2 <= channels <= 16 or meta.get("dtype") != "<f8":
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
    if convention.get("kind") != "MD" or convention.get("unit") not in ("m", "ft", "M", "FT") or not convention.get("reference") or convention.get("alignment") != "exact_shared_axis_v1":
        raise ValueError("analysis_axis_semantics")
    if meta.get("provenance", {}).get("axis") != convention:
        raise ValueError("analysis_axis_provenance")
    sampling = meta.get("provenance", {}).get("sampling", {})
    if sampling != {"method": "all_selected_window_v1", "seed": 0, "count": rows, "scope": "selected_window_only", "random": False, "extrapolation": False}:
        raise ValueError("analysis_sampling")
    return meta, values, quality


meta, values, quality = load_input()
rows, channels = values.shape
valid = np.all(np.isfinite(values) & ((quality & 3) == 0), axis=1)
selected = values[valid]
count = int(selected.shape[0])
matrix = [[None for _ in range(channels)] for _ in range(channels)]
constant = []
redundant = []
if count >= 2:
    scales = np.max(np.abs(selected), axis=0)
    normalized = selected / np.where(scales == 0, 1.0, scales)
    centered = normalized - np.mean(normalized, axis=0)
    norms = np.sqrt(np.sum(centered * centered, axis=0))
    constant = [index for index in range(channels) if norms[index] == 0]
    for left in range(channels):
        for right in range(channels):
            if norms[left] and norms[right]:
                coefficient = float(np.dot(centered[:, left], centered[:, right]) / (norms[left] * norms[right]))
                matrix[left][right] = max(-1.0, min(1.0, coefficient))
                if left < right and abs(matrix[left][right]) >= 0.95:
                    redundant.append({"left": left, "right": right, "pearson": matrix[left][right], "sampleCount": count})
report = {"contract": "paleo.attribute-correlation-report.v1", "sourceKind": "well_curve_numeric_attributes",
          "scope": "selected_window_only", "method": "pearson_joint_complete_rows_v1", "columns": meta["columns"],
          "sampleCount": rows, "jointValidCount": count, "rejectedRows": rows - count,
          "coverage": count / rows if rows else None, "matrix": matrix, "constantColumns": constant,
          "redundancyThreshold": 0.95, "redundantPairs": redundant,
          "status": "empty_window" if not rows else "insufficient_samples" if count < 2 else "ok",
          "provenance": meta["provenance"],
          "software": {"python": sys.version.split()[0], "numpy": np.__version__, "determinism": "single_thread_fixed_input_order_tolerance"},
          "recommendations": ["Review strongly correlated pairs with geological context before selecting attributes."],
          "limitations": ["These are aligned well-curve numeric attributes, not a seismic volume or survey-wide population.",
                          "Constant or insufficient columns have undefined coefficients represented by null.",
                          "Correlation does not establish causality or replace formal attribute selection or fusion."]}
json.dump({"contract": "paleo.python-output.v1", "ports": {"REPORT": report}}, sys.stdout,
          allow_nan=False, separators=(",", ":"))
