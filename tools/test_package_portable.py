#!/usr/bin/env python3
"""Verify portable-package preflight and manifest handling without Windows SDKs."""

import os
import shutil
import subprocess
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import package_portable as package
from portable_manifest import PackageFiles, verify


class PortablePackageTests(unittest.TestCase):
    def test_source_prefix_requires_completed_matching_build_and_recipe(self):
        with tempfile.TemporaryDirectory() as temp:
            work = Path(temp)
            recipe = Path(__file__).resolve().parents[1] / "vendor/superbuild"
            copied = work / "vendor/superbuild"
            copied.mkdir(parents=True)
            for name in ("source_receipt.py", "CMakeLists.txt", "prepare-prefix.sh",
                         "debian13-base.lock", "debian-sources.lock", "closed-dependencies.cmake",
                         "prepare-debian-source.cmake"):
                shutil.copyfile(recipe / name, copied / name)
            shutil.copytree(recipe / "patches", copied / "patches")
            prefix = work / "prefix"
            prefix.mkdir()
            build = work / "build"
            build.mkdir()
            def invoke(*args):
                return subprocess.run([sys.executable, str(copied / "source_receipt.py"), *args],
                                      capture_output=True, text=True)
            digest = invoke("digest")
            self.assertEqual(digest.returncode, 0, digest.stderr)
            identity = digest.stdout + "profile\ninstallation\n"
            (prefix / ".paleo-superbuild-instance").write_text(identity)
            self.assertNotEqual(invoke("verify", prefix).returncode, 0)
            self.assertNotEqual(invoke("finish", prefix, "--build", build).returncode, 0)
            (build / ".paleo-superbuild-instance").write_text(identity)
            self.assertEqual(invoke("finish", prefix, "--build", build).returncode, 0)
            self.assertEqual(invoke("verify", prefix).returncode, 0)
            (copied / "patches/new.patch").write_text("new input")
            self.assertNotEqual(invoke("package-inputs", prefix).returncode, 0)

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

    def test_unresolved_pe_dependency_fails_by_default_and_can_be_allowed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            out = root / "out"
            out.mkdir()
            src = root / "build"
            src.mkdir()
            exe = src / "paleo.exe"
            exe.write_bytes(b"MZ-fixture")
            files = PackageFiles(out)
            copied = files.copy(exe, "paleo.exe", "build", src)
            system = root / "System32"
            system.mkdir()
            fake_dumpbin_output = (
                "Dump of file paleo.exe\n\n"
                "  Image has the following dependencies:\n\n"
                "    missing_dep_958.dll\n"
            )
            with patch.object(package, "checked", return_value=fake_dumpbin_output):
                with self.assertRaisesRegex(ValueError, "missing_dep_958.dll"):
                    package.collect_pe_imports(files, [copied], [], Path("dumpbin.exe"), system)
                with patch("sys.stderr"):
                    missing = package.collect_pe_imports(
                        files, [copied], [], Path("dumpbin.exe"), system, allow_missing=True
                    )
            self.assertIn("missing_dep_958.dll", missing)

    def test_verify_propagates_selfcheck_failure_as_nonzero(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            pkg = root / "portable"
            pkg.mkdir()
            for name in ("paleo_selfcheck.exe", "paleo.exe"):
                (pkg / name).write_bytes(b"fixture")
            PackageFiles(pkg).write([])

            def fake_run(cmd, **_kwargs):
                if str(cmd[0]).endswith("paleo_selfcheck.exe"):
                    raise subprocess.CalledProcessError(1, cmd, output="", stderr="selfcheck failed")
                return "ok"

            argv = ["package_portable.py", "--verify-only", str(pkg)]
            with (
                patch.dict(os.environ, {"SystemRoot": str(root)}, clear=False),
                patch.object(package, "checked", side_effect=fake_run),
                patch.object(sys, "argv", argv),
                patch("sys.stderr"),
                self.assertRaises(SystemExit) as error,
            ):
                package.main()
            self.assertEqual(error.exception.code, 1)

    def test_verify_propagates_main_exe_early_exit_and_launch_error(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            pkg = root / "portable"
            pkg.mkdir()
            for name in ("paleo_selfcheck.exe", "paleo.exe"):
                (pkg / name).write_bytes(b"fixture")
            PackageFiles(pkg).write([])

            for failure in (
                subprocess.CalledProcessError(2, ["paleo.exe", "--check-runtime"], stderr="early exit"),
                OSError("launch failed"),
            ):
                def fake_run(cmd, _err=failure, **_kwargs):
                    if str(cmd[0]).endswith("paleo.exe"):
                        raise _err
                    return "selfcheck ok"

                argv = ["package_portable.py", "--verify-only", str(pkg)]
                with (
                    patch.dict(os.environ, {"SystemRoot": str(root)}, clear=False),
                    patch.object(package, "checked", side_effect=fake_run),
                    patch.object(sys, "argv", argv),
                    patch("sys.stderr"),
                    self.assertRaises(SystemExit) as error,
                ):
                    package.main()
                self.assertEqual(error.exception.code, 1)

    def test_cli_path_overrides_deps_qgis_prefix_and_qt_pdf(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            cache_prefix = root / "cache-prefix"
            override_prefix = root / "override-prefix"
            deps_dir = root / "extra-deps"
            qt_pdf_dir = root / "qt-pdf"
            for d in (
                cache_prefix / "apps/qt6/bin",
                cache_prefix / "apps/qgis/bin",
                cache_prefix / "bin",
                override_prefix / "apps/qt6/bin",
                override_prefix / "apps/qgis/bin",
                override_prefix / "bin",
                deps_dir / "bin",
                qt_pdf_dir / "bin",
            ):
                d.mkdir(parents=True)

            (override_prefix / "bin/from_qgis_override.dll").write_bytes(b"qgis-override")
            (deps_dir / "bin/from_deps_override.dll").write_bytes(b"deps-override")
            (qt_pdf_dir / "bin/qt6pdf.dll").write_bytes(b"qtpdf-override")

            cache = {"QGIS_PREFIX": str(cache_prefix)}
            prefix, roots = package.resolve_prefix_and_roots(
                cache,
                qgis_prefix=override_prefix,
                deps=[deps_dir],
                qt_pdf=qt_pdf_dir,
            )
            self.assertEqual(prefix, override_prefix.resolve())

            out = root / "out"
            out.mkdir()
            src = root / "build"
            src.mkdir()
            exe = src / "paleo.exe"
            exe.write_bytes(b"MZ-fixture")
            files = PackageFiles(out)
            copied = files.copy(exe, "paleo.exe", "build", src)
            system = root / "System32"
            system.mkdir()

            def fake_dumpbin(cmd, **_kwargs):
                if str(cmd[-1]).endswith("paleo.exe"):
                    return (
                        "    from_qgis_override.dll\n"
                        "    from_deps_override.dll\n"
                        "    qt6pdf.dll\n"
                    )
                return ""

            with patch.object(package, "checked", side_effect=fake_dumpbin):
                missing = package.collect_pe_imports(files, [copied], roots, Path("dumpbin.exe"), system)
            self.assertEqual(missing, {})
            self.assertTrue((out / "from_qgis_override.dll").is_file())
            self.assertTrue((out / "from_deps_override.dll").is_file())
            self.assertTrue((out / "qt6pdf.dll").is_file())
            self.assertEqual(files.origins["from_deps_override.dll"]["origin"], "deps")
            self.assertEqual(files.origins["qt6pdf.dll"]["origin"], "qt-pdf")
            self.assertEqual(files.origins["from_qgis_override.dll"]["origin"], "osgeo4w")


if __name__ == "__main__":
    unittest.main()
