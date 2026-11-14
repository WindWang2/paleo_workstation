# Explicit optional trusted template. Host input is data, never source interpolation.
import json
import math
import sys

import numpy as np

request = json.load(sys.stdin)
if type(request) is not dict or set(request) != {"values"}:
    raise ValueError("invalid_summary_request")
values = request["values"]
if type(values) is not list or not 1 <= len(values) <= 8192:
    raise ValueError("invalid_summary_size")
if any(type(value) not in (int, float) or not math.isfinite(value) for value in values):
    raise ValueError("invalid_summary_value")
array = np.asarray(values, dtype=np.float64)
report = {"contract": "paleo.python-summary.v1", "count": int(array.size),
          "sum": float(array.sum()), "minimum": float(array.min()), "maximum": float(array.max())}
if not all(math.isfinite(report[key]) for key in ("sum", "minimum", "maximum")):
    raise ValueError("summary_overflow")
json.dump({"contract": "paleo.python-output.v1", "ports": {"REPORT": report}},
          sys.stdout, allow_nan=False, separators=(",", ":"))
