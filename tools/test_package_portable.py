#!/usr/bin/env python3
"""Verify portable-package preflight and manifest handling without Windows SDKs."""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import package_portable as package


class PortablePackageTests(unittest.TestCase):
    def test_existing_output_is_preserved(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build = root / "build"
            build.mkdir()
            for name in ("paleo.exe", "paleo_data_snapshot_worker.exe"):
                (build / name).touch()
            editor = build / "office-editor"
            required = package.REPO_ROOT / "vendor" / "office-editor-required.txt"
            for relative in required.read_text(encoding="utf-8").splitlines():
                asset = editor / relative
                asset.parent.mkdir(parents=True, exist_ok=True)
                asset.write_bytes(b"fixture")
            qgis = root / "qgis"
            (qgis / "resources").mkdir(parents=True)
            (qgis / "resources" / "symbology-style.xml").touch()
            marker = root / "existing-file"
            marker.write_text("keep", encoding="utf-8")
            argv = ["package_portable.py", "--build-dir", str(build), "--out", str(root),
                    "--office-editor", str(editor)]
            with (patch.object(package, "QGIS_PREFIX", qgis),
                  patch.object(package, "find_dumpbin", return_value=Path("dumpbin")),
                  patch.object(sys, "argv", argv),
                  self.assertRaisesRegex(SystemExit, "输出目录已存在")):
                package.main()
            self.assertEqual(marker.read_text(encoding="utf-8"), "keep")
            self.assertTrue((build / "paleo.exe").is_file())

    def test_missing_office_runtime_assets_are_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "index.html").write_bytes(b"index")
            with self.assertRaisesRegex(SystemExit, "Office 编辑组件缺失或不完整"):
                package.office_editor_dir(root, str(root))

    def test_unicode_manifest_names_round_trip(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name in ("启动-paleo.bat", "使用说明.txt"):
                (root / name).write_bytes(b"fixture")
            package.write_integrity_manifest(root)
            manifest = (root / "SHA256SUMS.txt").read_text(encoding="utf-8")
            self.assertIn("启动-paleo.bat", manifest)
            self.assertIn("使用说明.txt", manifest)
            checked = subprocess.run([sys.executable, str(root / "verify_package.py")],
                                     capture_output=True, text=True, timeout=15)
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)


if __name__ == "__main__":
    unittest.main()
