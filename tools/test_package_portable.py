#!/usr/bin/env python3
"""Verify portable-package preflight and manifest handling without Windows SDKs."""

import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import package_portable as package
from portable_manifest import PackageFiles, verify


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
            marker = root / "existing-file"
            marker.write_text("keep", encoding="utf-8")
            argv = ["package_portable.py", "--build-dir", str(build), "--out", str(root),
                    "--office-editor", str(editor)]
            with (patch.object(sys, "argv", argv),
                  patch("sys.stderr"), self.assertRaises(SystemExit) as error):
                package.main()
            self.assertEqual(error.exception.code, 1)
            self.assertEqual(marker.read_text(encoding="utf-8"), "keep")
            self.assertTrue((build / "paleo.exe").is_file())

    def test_missing_office_runtime_assets_are_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "index.html").write_bytes(b"index")
            with self.assertRaisesRegex(ValueError, "Office 编辑组件缺失或不完整"):
                package.office_editor_dir(root, str(root))

    def test_unicode_manifest_names_round_trip(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name in ("启动-paleo.bat", "使用说明.txt"):
                (root / name).write_bytes(b"fixture")
            PackageFiles(root).write([])
            manifest = (root / "RUNTIME_SOURCES.json").read_text(encoding="utf-8")
            self.assertIn("启动-paleo.bat", manifest)
            self.assertIn("使用说明.txt", manifest)
            verify(root)
            (root / "使用说明.txt").write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "使用说明.txt"):
                verify(root)

    @unittest.skipIf(os.name == "nt", "ELF installation uses SONAME symlinks")
    def test_manifest_hashes_alias_after_install_and_relocation(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp)
            original = source / "libexample.so"
            original.write_bytes(b"original binary")
            output = source / "out"
            files = PackageFiles(output)
            alias = files.copy(original, "libexample.so.1", "fixture", source)
            target = files.copy(original, "libexample.so.1.0", "fixture", source)
            alias.unlink()
            alias.symlink_to(target.name)
            target.write_bytes(b"relocated binary")
            files.write([], rewritten=[target])
            verify(output)


if __name__ == "__main__":
    unittest.main()
