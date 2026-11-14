"""Independent scalar references for the two optional host-locked templates."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import struct
import subprocess
import sys
import unittest


def frame(kind, values, quality=None, axis=None, channels=None, change=None):
    rows = len(values)
    channels = channels or (len(values[0]) if rows else 2)
    axis = axis if axis is not None else [100 + index * 0.5 for index in range(rows)]
    quality = quality if quality is not None else [[0] * channels for _ in range(rows)]
    payload = b"".join(struct.pack("<d", value) for value in axis)
    payload += b"".join(struct.pack("<d", value) for row in values for value in row)
    payload += bytes(flag for row in quality for flag in row)
    sampling = {"method": "all_selected_window_v1", "seed": 0, "count": rows,
                "scope": "selected_window_only", "random": False, "extrapolation": False}
    meta = {"contract": "paleo.python-analysis-input.v1", "analysis": kind,
            "inputProtocol": "paleo.python-analysis-frame.v1", "sourceKind": "well_curve_numeric_attributes",
            "dtype": "<f8", "shape": [rows, channels],
            "axis": {"kind": "MD", "unit": "m", "reference": "KB", "alignment": "exact_shared_axis_v1"},
            "columns": [{"curveId": str(index + 1), "name": "curve-" + str(index), "unit": "API"}
                        for index in range(channels)],
            "layout": {"axisBytes": rows * 8, "valueBytes": rows * channels * 8,
                       "qualityBytes": rows * channels, "order": "axis_then_row_major_values_then_row_major_quality"},
            "payload": {"bytes": len(payload), "sha256": hashlib.sha256(payload).hexdigest()},
            "provenance": {"documentId": "fixture", "callerScope": "agent:test", "sampling": sampling}}
    if change:
        change(meta)
    header = json.dumps(meta, separators=(",", ":"), allow_nan=False).encode("utf-8")
    return struct.pack("<I", len(header)) + header + payload


class TemplateTests(unittest.TestCase):
    python = ""
    template_root = Path(__file__).resolve().parents[1] / "vendor" / "agent-python" / "templates"

    def run_template(self, kind, payload, success=True):
        environment = {"PATH": str(Path(self.python).parent), "OPENBLAS_NUM_THREADS": "1",
                       "OMP_NUM_THREADS": "1", "MKL_NUM_THREADS": "1", "NUMEXPR_NUM_THREADS": "1"}
        if "SystemRoot" in os.environ:
            environment["SystemRoot"] = os.environ["SystemRoot"]
        result = subprocess.run([self.python, "-I", "-u", "-B", "-X", "utf8",
                                 str(self.template_root / (kind + ".py"))], input=payload,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment,
                                timeout=15, check=False)
        if not success:
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b"")
            return None
        self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", "replace"))
        self.assertLess(len(result.stdout), 64 * 1024)
        return json.loads(result.stdout)["ports"]["REPORT"]

    def test_quality_matches_scalar_reference(self):
        values = [[1.0, 5.0], [2.0, 5.0], [100.0, 5.0], [4.0, 5.0], [float("nan"), 5.0],
                  [float("inf"), 5.0], [7.0, 5.0]]
        quality = [[0, 0], [4, 0], [2, 0], [1, 0], [0, 0], [0, 0], [0, 0]]
        report = self.run_template("well_quality", frame("well_quality", values, quality))
        first, second = report["metrics"]
        reference = [1.0, 2.0, 7.0]
        self.assertEqual(first["validCount"], len(reference))
        self.assertAlmostEqual(first["mean"], statistics.mean(reference), places=13)
        self.assertAlmostEqual(first["populationStdDev"], statistics.pstdev(reference), places=13)
        self.assertEqual(first["missingCount"], 3)
        self.assertEqual(first["sdkAbnormalCount"], 1)
        self.assertEqual(first["editedCount"], 1)
        self.assertEqual(first["nonFiniteCount"], 2)
        self.assertEqual(second["status"], "constant")
        self.assertEqual(second["populationStdDev"], 0.0)

    def test_pearson_matches_independent_reference(self):
        values = [[1.0, 5.0, 7.0], [2.0, 4.0, 7.0], [3.0, 2.0, 7.0], [4.0, 1.0, 7.0]]
        report = self.run_template("attribute_correlation", frame("attribute_correlation", values))
        x, y = [row[0] for row in values], [row[1] for row in values]
        mean_x, mean_y = statistics.mean(x), statistics.mean(y)
        reference = sum((a - mean_x) * (b - mean_y) for a, b in zip(x, y))
        reference /= math.sqrt(sum((a - mean_x) ** 2 for a in x) * sum((b - mean_y) ** 2 for b in y))
        self.assertAlmostEqual(report["matrix"][0][1], reference, places=13)
        self.assertEqual(report["constantColumns"], [2])
        self.assertIsNone(report["matrix"][2][2])
        self.assertEqual(len(report["redundantPairs"]), 1)
        self.assertEqual(report["jointValidCount"], 4)

    def test_joint_complete_rows_exclude_null_abnormal_and_nonfinite(self):
        values = [[1.0, 2.0], [2.0, 4.0], [float("nan"), 3.0], [3.0, 6.0], [4.0, float("inf")]]
        quality = [[0, 0], [0, 0], [0, 0], [2, 0], [0, 0]]
        report = self.run_template("attribute_correlation", frame("attribute_correlation", values, quality))
        self.assertEqual(report["jointValidCount"], 2)
        self.assertEqual(report["rejectedRows"], 3)
        self.assertAlmostEqual(report["matrix"][0][1], 1.0, places=14)

    def test_empty_and_all_missing_are_explicit(self):
        for kind in ("well_quality", "attribute_correlation"):
            with self.subTest(kind=kind):
                report = self.run_template(kind, frame(kind, [], channels=2))
                self.assertEqual(report.get("status", report.get("metrics", [{}])[0].get("status")), "empty_window")
        report = self.run_template("well_quality", frame("well_quality", [[float("nan"), float("inf")]]))
        self.assertTrue(all(value["status"] == "all_missing" and value["mean"] is None for value in report["metrics"]))
        report = self.run_template("attribute_correlation", frame("attribute_correlation", [[1.0, 2.0]]))
        self.assertEqual(report["status"], "insufficient_samples")
        self.assertTrue(all(value is None for row in report["matrix"] for value in row))

    def test_large_magnitudes_remain_finite(self):
        values = [[1e308, -1e308], [-1e308, 1e308], [0.0, 0.0]]
        quality = self.run_template("well_quality", frame("well_quality", values))
        self.assertTrue(all(math.isfinite(item["populationStdDev"]) for item in quality["metrics"]))
        correlation = self.run_template("attribute_correlation", frame("attribute_correlation", values))
        self.assertAlmostEqual(correlation["matrix"][0][1], -1.0, places=13)

    def test_bad_frames_semantics_and_budget_fail_before_output(self):
        changes = [lambda m: m.update(dtype="object"), lambda m: m.update(shape=[8193, 2]),
                   lambda m: m["columns"][0].update(unit=""), lambda m: m["axis"].update(reference=""),
                   lambda m: m["provenance"]["sampling"].update(method="implicit_latest"),
                   lambda m: m["payload"].update(sha256="0" * 64)]
        for kind in ("well_quality", "attribute_correlation"):
            for index, change in enumerate(changes):
                with self.subTest(kind=kind, change=index):
                    self.run_template(kind, frame(kind, [[1.0, 2.0]], change=change), False)
            self.run_template(kind, frame(kind, [[1.0, 2.0]])[:-1], False)
            self.run_template(kind, frame(kind, [[1.0, 2.0]]) + b"extra", False)
            self.run_template(kind, frame(kind, [[1.0, 2.0], [3.0, 4.0]], axis=[1.0, 1.0]), False)

    def test_maximum_window_is_bounded_and_preserves_scope(self):
        values = [[float(row + column) for column in range(16)] for row in range(8192)]
        report = self.run_template("attribute_correlation", frame("attribute_correlation", values))
        self.assertEqual(report["sampleCount"], 8192)
        self.assertEqual(report["sourceKind"], "well_curve_numeric_attributes")
        self.assertEqual(report["scope"], "selected_window_only")
        self.assertEqual(len(report["matrix"]), 16)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--python", required=True, help="Absolute prepared interpreter; no PATH discovery")
    arguments, remaining = parser.parse_known_args()
    if not Path(arguments.python).is_absolute():
        parser.error("--python must be absolute")
    TemplateTests.python = arguments.python
    unittest.main(argv=[sys.argv[0]] + remaining)
