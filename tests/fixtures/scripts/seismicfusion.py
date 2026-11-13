"""Process fixture for the optional external fusion protocol; no inference dependencies."""
import argparse
import json
from pathlib import Path
import struct
import sys
import time

parser = argparse.ArgumentParser()
parser.add_argument("--run", required=True)
parser.add_argument("--result", required=True)
args = parser.parse_args()
request = json.loads(Path(args.run).read_text(encoding="utf-8"))
output = Path(request["output_dir"])
mode = request["unit_name"]
# Confirm the client's cwd change did not break local relative input paths.
assert Path(request["seismic_path"]).is_absolute()
assert Path(request["seismic_path"]).is_file()
print("PROGRESS 20 读取层位", flush=True)
if mode == "sleep":
    time.sleep(30)
if mode == "failure":
    Path(args.result).write_text(json.dumps({"state": "failed", "error": "fixture failure"}), encoding="utf-8")
    sys.exit(7)
if mode == "bad-json":
    Path(args.result).write_text("invalid-json", encoding="utf-8")
    sys.exit(0)
if mode == "missing-result":
    sys.exit(0)
if mode == "noise":
    print("x" * 200000, flush=True)
    print("PROGRESS 42 not-a-number-is-tested-separately", flush=True)
    print("PROGRESS invalid ignored", flush=True)
    print("PROGRESS 999 ignored", flush=True)
(output / "grid.bin").write_bytes(struct.pack("<4i", 1, 2, -9999, 3))
(output / "grid.tif").write_bytes(b"fixture file presence only; no GIS decoding")
result = {
    "status": "succeeded", "unit_name": mode, "output_dir": str(output),
    "nodata": -9999, "elapsed_seconds": 0.01,
    "grid": {
        "columns": 2, "rows": 2, "dx": 50, "dy": 50,
        "extent_left": 100, "extent_bottom": 200, "extent_right": 200, "extent_top": 300,
        "width": 100, "height": 100, "gdal_geotransform": [100, 50, 0, 300, 0, -50],
        "binary_row_order": "north_to_south", "axis_order": "east_north_xy", "coordinate_unit": "m",
        "crs_auth": "EPSG:32650", "crs_wkt": "fixture-wkt",
    },
    "classification": {"n_clusters": 3, "valid_cells": 3, "selected_attributes": ["A4", "A1"]},
    "output_files": {"int32_north_up_bin": "grid.bin", "geotiff_north_up": "grid.tif"},
}
Path(args.result).write_text(json.dumps({"state": "succeeded", "result": result}), encoding="utf-8")
# No final newline: the client must drain stdout after process exit.
sys.stdout.write("PROGRESS 100 完成")
sys.stdout.flush()
if mode == "nonzero-success":
    sys.exit(9)
