#!/usr/bin/env python3
"""Assemble and check a Windows portable runtime from locked or overridden inputs.

All dependency search paths come from CMakeCache.txt, environment variables, or
explicit CLI overrides (--qgis-prefix / --deps / --qt-pdf / --office-editor);
no personal machine paths are hardcoded.

By default, any unresolved PE dependency fails hard (non-zero exit). Pass
--allow-missing only for explicit diagnostic overrides. All --verify /
--verify-only failures, early process exits, timeouts, and launch errors
propagate as non-zero exit codes.
"""

import argparse
from collections import deque
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import zipfile

from portable_manifest import (PackageFiles, REPO_ROOT, load_cmake_cache,
                               verify, verify_receipt)


REQUIRED_EXES = ("paleo.exe", "paleo_selfcheck.exe", "paleo_data_snapshot_worker.exe")
OPTIONAL_EXES = ("paleo_mkproject.exe", "paleo_seismic_demo.exe")
OS_DLLS = frozenset("""
advapi32 authz avrt bcrypt bcryptprimitives bluetoothapis cfgmgr32 combase comctl32
comdlg32 crypt32 cryptbase cryptui d2d1 d3d9 d3d11 d3d12 dbghelp dcomp devobj
dhcpcsvc dhcpcsvc6 dnsapi dpapi dsparse dwmapi dwrite dxcore dxgi dxva2
fontsub gdi32 gdi32full hid icu icuuc iertutil imm32 iphlpapi kernel.appcore kernel32
kernelbase mf mfplat mfreadwrite mfuuid mmdevapi mpr msimg32 msvcp_win
msvcrt mswsock ncrypt netapi32 netutils normaliz nsi ntasn1 ntdll odbc32 odbccp32
ole32 oleacc oleaut32 opengl32 pathcch pdh powrprof profapi propsys psapi
rpcrt4 runtimeobject secur32 sechost setupapi shcore shell32 shlwapi srvcli
sspicli strmiids taskschd ucrtbase uiautomationcore umpdc urlmon user32 userenv usp10
uxtheme version win32u windows.storage winhttp wininet winmm winusb
wintrust wldap32 ws2_32 wsock32 wtsapi32 xmllite
""".split())
OTHER_OS_IMPORTS = {"winspool.drv", "bthprops.cpl"}
QT_PLUGIN_GROUPS = (
    "crypto", "generic", "iconengines", "imageformats", "networkinformation", "platforms",
    "position", "sqldrivers", "styles", "tls", "vectorimageformats",
)
LAUNCHER = (
    '@echo off\r\nsetlocal\r\n'
    'start "" "%~dp0paleo.exe" %*\r\n'
)


def checked(command, **kwargs):
    return subprocess.run(command, check=True, text=True, encoding="utf-8",
                          errors="replace", capture_output=True, timeout=300, **kwargs).stdout


def find_dumpbin() -> Path:
    installer = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
    location = checked([str(installer), "-latest", "-products", "*", "-requires",
                        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                        "-property", "installationPath"]).strip()
    tools = Path(location) / "VC/Tools/MSVC"
    for version in sorted(tools.iterdir(), reverse=True):
        path = version / "bin/Hostx64/x64/dumpbin.exe"
        if path.is_file():
            return path
    raise ValueError("MSVC x64 dumpbin was not found through vswhere")


def office_editor_dir(build: Path, override: str | None) -> Path:
    lock = REPO_ROOT / "vendor/office-editor-required.txt"
    candidates = ([Path(override)] if override else
                  [build / "office-editor", REPO_ROOT / "vendor/ranuts-document"])
    for directory in candidates:
        if all((directory / name).is_file() and (directory / name).stat().st_size
               for name in lock.read_text(encoding="utf-8").splitlines()):
            return directory.resolve(strict=True)
    raise ValueError("Office 编辑组件缺失或不完整。请准备 vendor/ranuts-document 或指定 --office-editor")


def is_os_import(name: str, system: Path) -> bool:
    return (name.startswith(("api-ms-win-", "ext-ms-win-"))
            or ((name.removesuffix(".dll") in OS_DLLS or name in OTHER_OS_IMPORTS)
                and (system / name).is_file()))


def _append_override_root(roots: list[tuple[Path, str, Path]], raw_path: Path, role: str):
    resolved = Path(raw_path).resolve(strict=True)
    if not resolved.is_dir():
        raise ValueError(f"Dependency override directory does not exist: {raw_path}")
    bin_dir = resolved / "bin"
    if bin_dir.is_dir():
        roots.append((bin_dir, role, resolved))
    roots.append((resolved, role, resolved))


def resolve_prefix_and_roots(
    cache: dict[str, str],
    qgis_prefix: Path | None = None,
    deps: list[Path] | None = None,
    qt_pdf: Path | None = None,
) -> tuple[Path, list[tuple[Path, str, Path]]]:
    """Resolve QGIS dependency prefix and DLL search roots from CLI/env/cache."""
    raw_prefix = qgis_prefix or os.environ.get("PALEO_QGIS_PREFIX") or cache.get("QGIS_PREFIX")
    if not raw_prefix:
        raise ValueError("QGIS_PREFIX is not set in CMakeCache.txt; pass --qgis-prefix or set PALEO_QGIS_PREFIX")
    prefix = Path(raw_prefix).resolve(strict=True)
    qt = prefix / "apps/qt6"
    gis = prefix / "apps/qgis"
    roots: list[tuple[Path, str, Path]] = []

    # Explicit overrides take search precedence when provided (#958).
    dep_paths = list(deps or [])
    env_deps = os.environ.get("PALEO_DEPS_DIR")
    if not dep_paths and env_deps:
        dep_paths = [Path(p) for p in env_deps.split(os.pathsep) if p]
    for dep_path in dep_paths:
        _append_override_root(roots, dep_path, "deps")

    qt_pdf_raw = qt_pdf or os.environ.get("PALEO_QT_PDF_DIR")
    if qt_pdf_raw:
        _append_override_root(roots, Path(qt_pdf_raw), "qt-pdf")

    roots.extend((root, "osgeo4w", prefix) for root in (qt / "bin", gis / "bin", prefix / "bin"))
    return prefix, roots


def collect_pe_imports(
    files: PackageFiles,
    entries,
    roots,
    dumpbin: Path,
    system: Path,
    *,
    allow_missing: bool = False,
):
    pending = deque(entries)
    found = {entry.name.lower() for entry in entries if entry.parent == files.output}
    missing: dict[str, set[str]] = {}
    while pending:
        binary = pending.popleft()
        listing = checked([str(dumpbin), "/nologo", "/dependents", str(binary)])
        for name in sorted(set(re.findall(r"^ {4}([\w.+-]+\.(?:dll|drv|cpl))\s*$",
                                          listing.lower(), re.M))):
            if name in found or is_os_import(name, system):
                continue
            match = next(((directory / name, role, root) for directory, role, root in roots
                          if (directory / name).is_file()), None)
            if match:
                pending.append(files.copy(match[0], name, match[1], match[2]))
                found.add(name)
            else:
                missing.setdefault(name, set()).add(binary.name)
    if missing:
        details = "\n".join(f"{name} required by {', '.join(sorted(users))}"
                            for name, users in sorted(missing.items()))
        if not allow_missing:
            raise ValueError(f"Missing locked runtime dependencies:\n{details}")
        print(f"WARNING: allowing missing runtime dependencies (--allow-missing):\n{details}",
              file=sys.stderr)
    return missing


def stage_windows(
    files: PackageFiles,
    build: Path,
    office: Path,
    cache,
    dumpbin: Path,
    *,
    qgis_prefix: Path | None = None,
    deps: list[Path] | None = None,
    qt_pdf: Path | None = None,
    allow_missing: bool = False,
):
    prefix, roots = resolve_prefix_and_roots(cache, qgis_prefix=qgis_prefix, deps=deps, qt_pdf=qt_pdf)
    verify_receipt(prefix, ".paleo-osgeo4w-lock.sha256", "vendor/osgeo4w-offline.lock.json")
    verify_receipt(prefix, ".paleo-opencl-source.sha256", "vendor/opencl-source.lock.json")
    qt = prefix / "apps/qt6"
    gis = prefix / "apps/qgis"
    gdal = prefix / "apps/gdal"
    products = (*REQUIRED_EXES, *(name for name in OPTIONAL_EXES if (build / name).is_file()))
    entries = [files.copy(build / name, name, "build", build) for name in products]
    entries.append(files.copy(qt / "bin/QtWebEngineProcess.exe", "QtWebEngineProcess.exe",
                              "osgeo4w", prefix))
    # Runtime loaders reach these plugins without an import from paleo.exe.
    for group in QT_PLUGIN_GROUPS:
        source = qt / "plugins" / group
        if source.is_dir():
            files.tree(source, f"plugins/{group}", "osgeo4w", prefix, is_release_dll)
    qml = qt / "qml"
    files.tree(qml, "qml", "osgeo4w", prefix,
               lambda path: path.relative_to(qml).parts[0] != "QtTest"
               and path.suffix.lower() != ".pdb"
               and (path.suffix.lower() != ".dll" or is_release_dll(path)))
    files.tree(gis / "plugins", "qgis/plugins", "osgeo4w", prefix,
               lambda path: is_release_dll(path) and not path.name.startswith("plugin_"))
    gdal_plugins = gdal / "lib/gdalplugins"
    files.copy(gdal_plugins / "drivers.ini", "gdalplugins/drivers.ini", "osgeo4w", prefix)
    files.tree(gdal_plugins, "gdalplugins", "osgeo4w", prefix, is_release_dll)
    entries.extend(files.output.rglob("*.dll"))
    freexl = build / "paleo_freexl.dll"
    entries.append(files.copy(freexl, freexl.name, "build", build))
    freexl_source = REPO_ROOT / "vendor/Data_service/vendor/freexl"
    for name in ("NOTICE.txt", "LGPL-2.1.txt", "spreadsheet_compat.patch", "freexl-2.0.0/COPYING"):
        files.copy(freexl_source / name, f"share/paleo/licenses/freexl/{name}", "source", REPO_ROOT)
    ort = Path(cache["CMAKE_HOME_DIRECTORY"]) / "vendor/onnxruntime"
    if ort.is_dir() and cache.get("PALEO_ONNXRUNTIME") != "OFF" and cache.get("ORT_LIB") != "OFF":
        for name in ("onnxruntime.dll", "onnxruntime_providers_shared.dll"):
            entries.append(files.copy(ort / "lib" / name, name, "onnxruntime", ort))
        for name in ("LICENSE", "ThirdPartyNotices.txt"):
            files.copy(ort / name, f"share/paleo/licenses/onnxruntime/{name}", "onnxruntime", ort)
    collect_pe_imports(
        files, entries, roots, dumpbin,
        Path(os.environ["SystemRoot"]) / "System32",
        allow_missing=allow_missing,
    )
    add_windows_resources(files, prefix, qt, gis, gdal, office)


def is_release_dll(path: Path) -> bool:
    return (path.suffix.lower() == ".dll"
            and not (path.stem.endswith("d") and path.with_name(path.stem[:-1] + ".dll").exists()))


def add_windows_resources(files: PackageFiles, prefix: Path, qt: Path, gis: Path, gdal: Path,
                          office: Path):
    for source, destination in ((qt / "resources", "resources"),
                                (qt / "translations", "translations"),
                                (gis / "resources", "qgis/resources"),
                                (gis / "svg", "qgis/svg"),
                                (gdal / "share/gdal", "share/gdal"),
                                (prefix / "share/proj", "share/proj")):
        files.tree(source, destination, "osgeo4w", prefix)
    files.copy(qt / "certs/rootcerts.pem", "share/certs/ca-bundle.pem", "osgeo4w", prefix)
    files.tree(gis / "doc", "share/paleo/licenses/qgis", "osgeo4w", prefix,
               lambda path: path.name.upper() in {"LICENSE", "COPYING"})
    files.tree(prefix / "share/doc", "share/paleo/licenses/osgeo4w", "osgeo4w", prefix)
    if (qt / "sbom").is_dir():
        files.tree(qt / "sbom", "share/paleo/licenses/qt/sbom", "osgeo4w", prefix)
    files.tree(REPO_ROOT / "resources/fonts", "share/paleo/licenses/fonts", "source", REPO_ROOT,
               lambda path: path.name.startswith("OFL") or path.name.endswith(".lock.json"))
    files.tree(office, "office-editor", "office-editor", office)
    files.copy(REPO_ROOT / "vendor/office-editor-NOTICE.txt",
               "share/paleo/licenses/office-editor/NOTICE.txt", "source", REPO_ROOT)
    (files.output / "qt.conf").write_text("[Paths]\nPrefix=.\n", encoding="ascii")
    (files.output / "启动-paleo.bat").write_bytes(LAUNCHER.encode("ascii"))


def verify_windows(package: Path):
    verify(package)
    # Build SDK, Qt/GIS prefixes and the caller's Python are absent from PATH.
    with tempfile.TemporaryDirectory(prefix="paleo-portable-check-") as scratch:
        system = Path(os.environ["SystemRoot"])
        env = {"SystemRoot": str(system), "PATH": str(system / "System32"),
               "TEMP": scratch, "TMP": scratch,
               "QT_QPA_PLATFORM": "offscreen", "PROJ_NETWORK": "OFF",
               "QGIS_CUSTOM_CONFIG_PATH": scratch}
        print(checked([str(package / "paleo_selfcheck.exe")], cwd=scratch, env=env))
        print(checked([str(package / "paleo.exe"), "--check-runtime"], cwd=scratch, env=env))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--office-editor")
    parser.add_argument("--qgis-prefix", type=Path,
                        help="Override QGIS/OSGeo4W dependency prefix (or set PALEO_QGIS_PREFIX)")
    parser.add_argument("--deps", type=Path, action="append",
                        help="Additional DLL dependency search root (repeatable; or set PALEO_DEPS_DIR)")
    parser.add_argument("--qt-pdf", type=Path,
                        help="Additional Qt PDF DLL search root (or set PALEO_QT_PDF_DIR)")
    parser.add_argument("--allow-missing", action="store_true",
                        help="Warn instead of failing when PE DLL closure has unresolved imports")
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--verify-only", type=Path)
    parser.add_argument("--zip", action="store_true")
    parser.add_argument("--skip-python", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        if args.verify_only:
            verify_windows(args.verify_only.resolve(strict=True))
            return
        if not args.build_dir:
            parser.error("--build-dir is required to create a package")
        build = args.build_dir.resolve(strict=True)
        output = (args.out or build / "portable").absolute()
        if output.exists() or output.is_symlink():
            raise ValueError(f"输出目录已存在，请另选 --out: {output}")
        output.parent.mkdir(parents=True, exist_ok=True)
        archive = build / "paleo-portable.zip"
        if args.zip and (archive.exists() or archive.is_symlink() or output == archive):
            raise ValueError(f"压缩包路径已占用: {archive}")
        cache = load_cmake_cache(build)
        office = office_editor_dir(build, args.office_editor or os.environ.get("PALEO_OFFICE_EDITOR"))
        with tempfile.TemporaryDirectory(prefix=".paleo-package-", dir=output.parent) as staging:
            package = Path(staging) / "runtime"
            package.mkdir()
            files = PackageFiles(package)
            stage_windows(
                files, build, office, cache, find_dumpbin(),
                qgis_prefix=args.qgis_prefix,
                deps=args.deps,
                qt_pdf=args.qt_pdf,
                allow_missing=args.allow_missing,
            )
            files.write(["vendor/osgeo4w-offline.lock.json", "vendor/manifest.json",
                         "vendor/opencl-source.lock.json",
                         "vendor/office-editor-required.txt", "vendor/fetch-ranuts-document.sh"])
            if args.verify:
                verify_windows(package)
            package.rename(output)
        if args.zip:
            with zipfile.ZipFile(archive, "x", zipfile.ZIP_DEFLATED, compresslevel=6) as zipped:
                for path in sorted(output.rglob("*")):
                    if path.is_file():
                        zipped.write(path, path.relative_to(output.parent))
        print(f"Windows portable runtime: {output}")
    except (KeyError, OSError, ValueError, subprocess.SubprocessError) as error:
        outputs = (getattr(error, "stdout", None), getattr(error, "stderr", None))
        diagnostics = "\n".join(str(output) for output in outputs if output)
        parser.exit(1, f"{error}\n{diagnostics}")


if __name__ == "__main__":
    main()
