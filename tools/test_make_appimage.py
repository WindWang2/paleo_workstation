#!/usr/bin/env python3
"""Selftest for tools/make_appimage.sh (#956).

Verifies without network access or full squashfs packaging:
1. Synthesized AppDir with an ELF missing a shared library fails non-zero.
2. Missing required Qt plugin, QGIS provider, GDAL data, or PROJ db fails non-zero.
3. Missing vendored prefix fails unless --allow-system-fallback is explicitly passed.
4. Full --stage-only smoke run against a synthesized vendored prefix succeeds and
   passes --verify-appdir.
"""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parent.parent
SCRIPT = REPO_ROOT / "tools" / "make_appimage.sh"


def compile_elf(compiler: str, source: Path, output: Path, extra_args=()):
    cmd = [compiler, str(source), "-o", str(output), *extra_args]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError(f"compile failed: {' '.join(cmd)}\n{res.stderr}")


def build_minimal_elf(cc: str, scratch: Path, name: str, shared: bool = False, extra_args=()) -> Path:
    src = scratch / f"{name}.c"
    if shared:
        src.write_text("int paleo_fixture_fn(void) { return 42; }\n", encoding="utf-8")
        out = scratch / f"{name}.so"
        compile_elf(cc, src, out, ("-shared", "-fPIC", *extra_args))
        return out
    src.write_text("int main(void) { return 0; }\n", encoding="utf-8")
    out = scratch / name
    compile_elf(cc, src, out, extra_args)
    return out


def populate_valid_appdir(appdir: Path, exe: Path, plugin_so: Path):
    (appdir / "usr/bin").mkdir(parents=True, exist_ok=True)
    (appdir / "usr/lib").mkdir(parents=True, exist_ok=True)
    (appdir / "usr/lib/qt6/plugins/platforms").mkdir(parents=True, exist_ok=True)
    (appdir / "usr/lib/qt6/plugins/sqldrivers").mkdir(parents=True, exist_ok=True)
    (appdir / "usr/lib/qgis/plugins").mkdir(parents=True, exist_ok=True)
    (appdir / "usr/share/qgis/resources").mkdir(parents=True, exist_ok=True)
    (appdir / "usr/share/gdal").mkdir(parents=True, exist_ok=True)
    (appdir / "usr/share/proj").mkdir(parents=True, exist_ok=True)

    shutil.copy2(exe, appdir / "usr/bin/paleo")
    (appdir / "usr/bin/paleo").chmod(0o755)
    shutil.copy2(exe, appdir / "usr/lib/qt6/QtWebEngineProcess.bin")
    (appdir / "usr/lib/qt6/QtWebEngineProcess").write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    (appdir / "usr/lib/qt6/QtWebEngineProcess").chmod(0o755)

    for rel in (
        "usr/lib/qt6/plugins/platforms/libqoffscreen.so",
        "usr/lib/qt6/plugins/sqldrivers/libqsqlite.so",
        "usr/lib/qgis/plugins/libprovider_spatialite.so",
        "usr/lib/qgis/plugins/libprovider_delimitedtext.so",
    ):
        shutil.copy2(plugin_so, appdir / rel)

    (appdir / "usr/share/qgis/resources/srs.db").write_bytes(b"sqlite-fixture")
    (appdir / "usr/share/gdal/epsg.wkt").write_text("fixture\n", encoding="utf-8")
    (appdir / "usr/share/proj/proj.db").write_bytes(b"proj-fixture")


@unittest.skipIf(os.name == "nt", "make_appimage.sh is Linux-only")
class MakeAppImageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        if not cls.cc:
            raise unittest.SkipTest("C compiler required to synthesize ELF fixtures")
        if not shutil.which("readelf") or not shutil.which("ldd"):
            raise unittest.SkipTest("readelf/ldd required for AppImage verification")

    def run_script(self, *args, env=None):
        run_env = os.environ.copy()
        run_env.pop("QGIS_PREFIX_PATH", None)
        run_env.pop("PALEO_APPIMAGE_PREFIX", None)
        run_env.pop("PALEO_QT_PREFIX", None)
        run_env.pop("PALEO_QGIS_PREFIX", None)
        run_env.pop("PALEO_APPIMAGE_ALLOW_SYSTEM", None)
        if env:
            run_env.update(env)
        return subprocess.run(
            ["bash", str(SCRIPT), *args],
            capture_output=True,
            text=True,
            env=run_env,
            timeout=60,
        )

    def test_shell_syntax_valid(self):
        res = subprocess.run(["bash", "-n", str(SCRIPT)], capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stderr)

    def test_unresolved_elf_dependency_blocks_packaging(self):
        with tempfile.TemporaryDirectory(prefix="paleo-appimage-missing-elf-") as tmp:
            root = Path(tmp)
            exe = build_minimal_elf(self.cc, root, "ok_exe")
            plugin = build_minimal_elf(self.cc, root, "ok_plugin", shared=True)

            # Build a temporary shared library and an ELF referencing it, then delete the library.
            ghost_lib = root / "libpaleo_ghost_956.so"
            ghost_src = root / "ghost.c"
            ghost_src.write_text("int ghost_sym(void) { return 7; }\n", encoding="utf-8")
            compile_elf(self.cc, ghost_src, ghost_lib, ("-shared", "-fPIC", "-Wl,-soname,libpaleo_ghost_956.so"))

            broken_src = root / "broken.c"
            broken_src.write_text("int ghost_sym(void); int main(void) { return ghost_sym(); }\n", encoding="utf-8")
            broken_exe = root / "broken_exe"
            compile_elf(self.cc, broken_src, broken_exe, (f"-L{root}", "-lpaleo_ghost_956", "-Wl,--no-as-needed"))
            ghost_lib.unlink()

            appdir = root / "Paleo.AppDir"
            populate_valid_appdir(appdir, exe, plugin)
            # First confirm the clean AppDir passes.
            ok = self.run_script("--verify-appdir", str(appdir))
            self.assertEqual(ok.returncode, 0, ok.stdout + ok.stderr)

            # Now swap in the ELF with a missing shared dependency.
            shutil.copy2(broken_exe, appdir / "usr/bin/paleo")
            (appdir / "usr/bin/paleo").chmod(0o755)
            bad = self.run_script("--verify-appdir", str(appdir))
            self.assertNotEqual(bad.returncode, 0, "unresolved dependency must fail non-zero")
            self.assertIn("MISSING", bad.stderr)
            self.assertIn("libpaleo_ghost_956.so", bad.stderr)

    def test_missing_key_provider_or_qt_plugin_fails(self):
        with tempfile.TemporaryDirectory(prefix="paleo-appimage-probes-") as tmp:
            root = Path(tmp)
            exe = build_minimal_elf(self.cc, root, "ok_exe")
            plugin = build_minimal_elf(self.cc, root, "ok_plugin", shared=True)

            for victim in (
                "usr/lib/qt6/plugins/platforms/libqoffscreen.so",
                "usr/lib/qt6/plugins/sqldrivers/libqsqlite.so",
                "usr/lib/qgis/plugins/libprovider_spatialite.so",
                "usr/lib/qgis/plugins/libprovider_delimitedtext.so",
                "usr/share/qgis/resources/srs.db",
                "usr/share/proj/proj.db",
                "usr/share/gdal/epsg.wkt",
            ):
                appdir = root / f"appdir_{victim.replace('/', '_')}"
                populate_valid_appdir(appdir, exe, plugin)
                (appdir / victim).unlink()
                res = self.run_script("--verify-appdir", str(appdir))
                self.assertNotEqual(res.returncode, 0, f"missing {victim} must fail")
                self.assertIn("PROBE MISSING", res.stderr, f"missing {victim} lacked probe error")

    def test_vendored_prefix_stage_only_smoke_and_missing_prefix_rejected(self):
        if not shutil.which("patchelf"):
            self.skipTest("patchelf required for staging smoke test")
        with tempfile.TemporaryDirectory(prefix="paleo-appimage-smoke-") as tmp:
            root = Path(tmp)
            prefix = root / "vendored_prefix"
            build = root / "build"
            build.mkdir()

            # Build a tiny helper library inside the vendored prefix and link paleo against it.
            (prefix / "lib").mkdir(parents=True)
            helper_src = root / "helper.c"
            helper_src.write_text("int paleo_helper_956(void) { return 0; }\n", encoding="utf-8")
            helper_so = prefix / "lib/libpaleo_helper_956.so"
            compile_elf(self.cc, helper_src, helper_so, ("-shared", "-fPIC", "-Wl,-soname,libpaleo_helper_956.so"))

            main_src = root / "main.c"
            main_src.write_text("int paleo_helper_956(void); int main(void) { return paleo_helper_956(); }\n", encoding="utf-8")
            compile_elf(self.cc, main_src, build / "paleo", (f"-L{prefix / 'lib'}", "-lpaleo_helper_956", "-Wl,--no-as-needed"))
            (build / "paleo").chmod(0o755)

            web_exe = build_minimal_elf(self.cc, root, "web_exe")
            plugin_so = build_minimal_elf(self.cc, root, "plugin_so", shared=True)

            # Populate minimal vendored Qt + QGIS + GDAL + PROJ layout under prefix.
            (prefix / "lib/qt6/plugins/platforms").mkdir(parents=True)
            (prefix / "lib/qt6/plugins/sqldrivers").mkdir(parents=True)
            (prefix / "lib/qt6/libexec").mkdir(parents=True)
            (prefix / "lib/qgis/plugins").mkdir(parents=True)
            (prefix / "share/qgis/resources").mkdir(parents=True)
            (prefix / "share/gdal").mkdir(parents=True)
            (prefix / "share/proj").mkdir(parents=True)

            shutil.copy2(plugin_so, prefix / "lib/qt6/plugins/platforms/libqoffscreen.so")
            shutil.copy2(plugin_so, prefix / "lib/qt6/plugins/sqldrivers/libqsqlite.so")
            shutil.copy2(web_exe, prefix / "lib/qt6/libexec/QtWebEngineProcess")
            shutil.copy2(plugin_so, prefix / "lib/qgis/plugins/libprovider_spatialite.so")
            shutil.copy2(plugin_so, prefix / "lib/qgis/plugins/libprovider_delimitedtext.so")
            (prefix / "share/qgis/resources/srs.db").write_bytes(b"srs-fixture")
            (prefix / "share/gdal/epsg.wkt").write_text("gdal-fixture\n", encoding="utf-8")
            (prefix / "share/proj/proj.db").write_bytes(b"proj-fixture")

            # Missing explicit prefix must fail cleanly.
            missing = self.run_script("--build-dir", str(build), "--prefix", str(root / "no-such-prefix"), "--stage-only")
            self.assertNotEqual(missing.returncode, 0)

            # Valid vendored prefix stages, bundles libpaleo_helper_956.so, patches RPATH, and passes verify_appdir.
            res = self.run_script(
                "--build-dir", str(build),
                "--prefix", str(prefix),
                "--stage-only",
                env={"PALEO_MAMCL_ROOT": str(root / "no-venv")},
            )
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            appdir = build / "appimage/PaleoWorkstation.AppDir"
            self.assertTrue((appdir / "usr/lib/libpaleo_helper_956.so").is_file())
            verify = self.run_script("--verify-appdir", str(appdir))
            self.assertEqual(verify.returncode, 0, verify.stdout + verify.stderr)


if __name__ == "__main__":
    unittest.main()
