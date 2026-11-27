"""Exercise both native PDAL plugins and the GDAL Parquet driver in a Windows prefix."""
import argparse
import csv
import ctypes
import json
import os
from pathlib import Path
import subprocess
import tempfile


def check(prefix):
    prefix = prefix.resolve(strict=True)
    binary = prefix / "bin"
    windows = Path(os.environ["SystemRoot"])
    env = dict(os.environ, PATH=os.pathsep.join(map(str, [
        binary, prefix / "apps/qgis/bin", prefix / "apps/Qt6/bin", windows / "System32", windows])),
        GDAL_DATA=str(prefix / "apps/gdal/share/gdal"), PROJ_DATA=str(prefix / "share/proj"),
        PDAL_DRIVER_PATH=str(binary))
    # Propagate loader failures to the test process even on an interactive desktop.
    previous = ctypes.windll.kernel32.SetErrorMode(0x8001)
    try:
        with tempfile.TemporaryDirectory(prefix="paleo-pdal-arrow-") as tmp:
            directory = Path(tmp)
            points, parquet, output = [directory / name for name in ("points.csv", "points.parquet", "read.csv")]
            points.write_text("X,Y,Z\n3,4,5\n5,7,11\n13,17,19\n", encoding="ascii")

            def run(args, stdin=None):
                result = subprocess.run(args, cwd=directory, env=env, text=True, encoding="utf-8",
                                        errors="replace", input=stdin, capture_output=True, timeout=90)
                if result.returncode:
                    raise RuntimeError(f"{args[0]} returned {result.returncode}: {result.stderr.strip()}")
                return result.stdout

            def pipeline(stages):
                run([binary / "pdal.exe", "pipeline", "--stdin"], json.dumps(stages))

            pipeline([{"type": "readers.text", "filename": str(points), "override_srs": "EPSG:32650"},
                      {"type": "writers.arrow", "filename": str(parquet), "batch_size": 2}])
            pipeline([{"type": "readers.arrow", "filename": str(parquet)},
                      {"type": "writers.text", "filename": str(output), "order": "X,Y,Z", "keep_unspecified": False}])
            with output.open(encoding="utf-8", newline="") as source:
                coords = [tuple(float(row[k]) for k in ("X", "Y", "Z")) for row in csv.DictReader(source)]
            if coords != [(3, 4, 5), (5, 7, 11), (13, 17, 19)]:
                raise ValueError(f"PDAL Parquet round trip changed XYZ coordinates: {coords}")
            info = json.loads(run([binary / "ogrinfo.exe", "-json", "-so", "-al", parquet]))
            if info["layers"][0]["featureCount"] != len(coords):
                raise ValueError("GDAL returned a different feature count for the PDAL Parquet output")
    finally:
        ctypes.windll.kernel32.SetErrorMode(previous)
    print("PDAL/Arrow/GDAL passed the three-point Parquet round trip")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", type=Path)
    check(parser.parse_args().prefix)
