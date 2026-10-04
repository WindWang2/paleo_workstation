#!/usr/bin/env python3
"""Capture real production widget fixtures in isolated Qt test processes."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("revision", choices=("before", "after"))
    parser.add_argument("--scale", choices=("1", "2"), default="1")
    parser.add_argument("--build", default="build")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--supplemental", action="store_true", help="capture web fallback and populated map decorations in a fresh fixture")
    mode.add_argument("--controls", action="store_true", help="capture populated selection/filter/tag/hidden-track chips")
    mode.add_argument("--rows", action="store_true", help="capture populated fixed-height association and residual rows")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = (root / args.build).resolve()
    output = root / "docs" / "visual-polish" / args.revision / (args.scale + "x")
    output.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update(QT_QPA_PLATFORM="offscreen", QT_SCALE_FACTOR=args.scale,
               QGIS_PREFIX_PATH=str(root / "vendor/superbuild/prefix"),
               QTEST_FUNCTION_TIMEOUT="900000",
               PALEO_WORKFLOW_CAPTURE=str(output),
               PALEO_SECONDARY_CAPTURE=str(output), PALEO_VISUAL_CAPTURE=str(output))
    jobs = {
        "tst_ui": ["workflowAuditSnapshots", "secondarySurfaceAuditSnapshots",
                   "visualPolishChartSnapshots", "additionalVisualPolishSnapshots",
                   "dataOpsVisualPolishSnapshots"],
        "tst_datapreview": ["emptyStateBeforeAnyTab", "everyTypeOpensContent",
                            "loadingFailureAndRetryStates", "captureSynchronousLoadingState"],
        "tst_wellsection_ui": ["screenshots"],
        "tst_wellcomposite_visual": ["captureTokenPaperEvidence"],
        "tst_seismic_sectionui": ["captureTokenSectionEvidence"],
    }
    if args.supplemental:
        jobs = {"tst_ui": ["supplementalVisualPolishSnapshots", "mappingWorkbenchCanvasRibbonAndReferences"],
                "tst_seismic_3d": ["captureNativeVolumeEvidence"]}
    if args.controls:
        jobs = {"tst_ui": ["compactControlsVisualPolishSnapshots"]}
    if args.rows:
        jobs = {"tst_ui": ["fixedRowsVisualPolishSnapshots"]}
    suffix = "-rows" if args.rows else "-controls" if args.controls else "-supplemental" if args.supplemental else ""
    for test, functions in jobs.items():
        env["QT_QPA_PLATFORM"] = "xcb" if test == "tst_seismic_3d" else "offscreen"
        if test == "tst_seismic_3d":
            env["LIBGL_ALWAYS_SOFTWARE"] = "1"
            env["LP_NUM_THREADS"] = "8"
        sandbox = build / "visual-capture-sandbox" / args.revision / args.scale / test
        sandbox.mkdir(parents=True, exist_ok=True)
        env.update(XDG_CONFIG_HOME=str(sandbox / "config"),
                   XDG_DATA_HOME=str(sandbox / "data"))
        log_path = output / (test + suffix + ".log")
        with log_path.open("w") as log:
            result = subprocess.run([str(build / test), *functions], cwd=build,
                                    env=env, stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            raise SystemExit(f"{test} failed ({result.returncode}); see {log_path}")
    # Supplemental native captures replace the earlier unfixed-window images.
    if args.supplemental and (output / "capture.json").exists():
        base = json.loads((output / "capture.json").read_text())
        base["images"] = [name for name in base["images"]
                          if not name.startswith(("web-empty", "mapping-decorations", "seismic-3d"))]
        base["binariesSha256"].pop("tst_seismic_3d", None)
        (output / "capture.json").write_text(json.dumps(base, ensure_ascii=False, indent=2) + "\n")
    metadata = {
        "gitCommit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
        "productionDiffSha256": hashlib.sha256(subprocess.check_output(
            ["git", "diff", "HEAD", "--", "src/ui"], cwd=root)).hexdigest(),
        "binariesSha256": {test: hashlib.sha256((build / test).read_bytes()).hexdigest()
                            for test in jobs},
        "scale": args.scale,
        "backend": "Qt offscreen widgets; seismic-3d uses X11 / software OpenGL",
        "images": sorted(p.name for p in output.glob("*.png")
                         if ("-controls" if p.name.startswith("control-") else
                             "-rows" if p.name.startswith("fixed-rows-") else
                             "-supplemental" if p.name.startswith(("web-empty", "mapping-decorations", "seismic-3d")) else "") == suffix),
    }
    (output / ("capture" + suffix + ".json")).write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n")
    print(f"Captured {len(metadata['images'])} images at {output}")


if __name__ == "__main__":
    main()
