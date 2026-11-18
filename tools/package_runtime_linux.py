#!/usr/bin/env python3
"""Complete a Linux CMake install with its locked, relocatable runtime."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from urllib.request import urlopen

from portable_manifest import PackageFiles, REPO_ROOT, digest, load_cmake_cache, verify, verify_receipt


ABI_ROOT = Path("/usr/lib/x86_64-linux-gnu")
GLIBC_ABI = frozenset("""
ld-linux-x86-64.so.2 libanl.so.1 libc.so.6 libdl.so.2 libm.so.6 libmvec.so.1
libpthread.so.0 libresolv.so.2 librt.so.1 libutil.so.1
""".split())
QT_GROUPS = {"generic", "iconengines", "imageformats", "networkinformation",
             "platforms", "position", "sqldrivers", "styles", "tls", "vectorimageformats",
             "xcbglintegrations", "wayland-decoration-client", "wayland-shell-integration"}
CERT_LOCK = "vendor/runtime-ca-certificates.lock.json"
LAUNCHER = REPO_ROOT / "tools/paleo-portable-linux.sh"


def run(args, env=None, cwd=None):
    completed = subprocess.run(args, env=env, cwd=cwd, check=True,
                               capture_output=True, text=True, timeout=300)
    return completed.stdout


def is_elf(path: Path):
    if path.is_file():
        with path.open("rb") as stream:
            return stream.read(4) == b"\x7fELF"
    return False


def runtime_elfs(package: Path):
    for subtree in ("bin", "lib"):
        yield from (path for path in sorted((package / subtree).rglob("*"))
                    if not path.is_symlink() and is_elf(path))


def ldd_resolutions(binary: Path, env: dict[str, str]):
    for line in run(["ldd", str(binary)], env=env).splitlines():
        name, sep, target = line.strip().partition(" => ")
        if sep:
            if target.startswith("not found"):
                raise ValueError(f"{binary.name} requires unresolved {name}")
            path = target.rsplit(" (", 1)[0]
        elif name.startswith("/"):
            path = name.rsplit(" (", 1)[0]
            name = Path(path).name
        else:
            continue
        if not Path(path).is_absolute() or Path(name).name != name:
            raise ValueError(f"{binary.name}: unexpected loader entry {line.strip()}")
        yield name, Path(path).resolve(strict=True)


def host_abi(name, path):
    return name in GLIBC_ABI and path == (ABI_ROOT / name).resolve()


def dependency_prefix(cache):
    prefix = Path(cache["QGIS_PREFIX"]).resolve(strict=True)
    if (prefix / ".paleo-superbuild-base.sha256").is_file():
        lock = "vendor/superbuild/debian13-base.lock"
        verify_receipt(prefix, ".paleo-superbuild-base.sha256", lock, normalize=True)
        return prefix, prefix, [lock, "vendor/superbuild/debian-sources.lock",
                                "vendor/superbuild/CMakeLists.txt"]
    lock = "vendor/deb-closure.lock"
    verify_receipt(prefix.parent, ".paleo-deb-closure.sha256", lock, normalize=True)
    return prefix, prefix.parent, [lock]


def installable_sources(files, prefix, multiarch):
    seeds = {}
    plugins = multiarch / "qt6/plugins"
    for group in sorted(QT_GROUPS):
        base = plugins / group
        for path in sorted(base.glob("*.so")):
            seeds[path] = files.copy(path, f"lib/qt6/plugins/{group}/{path.name}", "linux-prefix", prefix)
    qml = multiarch / "qt6/qml"
    if qml.is_dir():
        for path in sorted(qml.rglob("*")):
            relative = path.relative_to(qml)
            if path.is_file() and relative.parts[0] != "QtTest":
                target = files.copy(path, f"lib/qt6/qml/{relative.as_posix()}", "linux-prefix", prefix)
                if is_elf(path):
                    seeds[path] = target

    qgis = prefix / "lib/qgis/plugins"
    for path in sorted(qgis.glob("*.so")):
        if not path.name.startswith("libplugin_"):
            seeds[path] = files.copy(path, f"lib/qgis/plugins/{path.name}", "linux-prefix", prefix)

    gdal = next((path for path in (multiarch / "gdalplugins", prefix / "lib/gdalplugins")
                 if path.is_dir()), None)
    if gdal:
        for path in sorted(gdal.glob("*.so")):
            seeds[path] = files.copy(path, f"lib/gdalplugins/{path.name}", "linux-prefix", prefix)
        drivers = gdal / "drivers.ini"
        if drivers.is_file():
            files.copy(drivers, "lib/gdalplugins/drivers.ini", "linux-prefix", prefix)
    else:
        (files.output / "lib/gdalplugins").mkdir(parents=True, exist_ok=True)

    web = prefix / "lib/qt6/libexec/QtWebEngineProcess"
    seeds[web] = files.copy(web, "lib/qt6/libexec/QtWebEngineProcess", "linux-prefix", prefix)
    for relative in ("dri/swrast_dri.so", "dri/kms_swrast_dri.so", "libEGL_mesa.so.0",
                     "libGLX_mesa.so.0", "libpipewire-0.3.so.0"):
        path = multiarch / relative
        if path.is_file():
            seeds[path] = files.copy(path, f"lib/{relative}", "linux-prefix", prefix)
    return seeds


def stage_shared_libs(files, seeds, approved, env):
    copied = {}
    for source in sorted(seeds):
        for name, path in ldd_resolutions(source, env):
            if host_abi(name, path):
                continue
            role, root = next(((role, root) for role, root in approved if path.is_relative_to(root)),
                              (None, None))
            if not role:
                raise ValueError(f"{source.name} resolves {name} outside the locked prefixes: {path}")
            if name in copied:
                if copied[name] != path:
                    raise ValueError(f"{name} resolves to both {path} and {copied[name]}")
                continue
            copied[name] = path
            if not (files.output / "lib" / name).is_file():
                files.copy(path, f"lib/{name}", role, root)


def stage_linux_resources(files, prefix, data_root):
    for source, destination in (
        ("share/proj", "share/proj"), ("share/gdal", "share/gdal"),
        ("share/qgis/resources", "share/qgis/resources"), ("share/qgis/svg", "share/qgis/svg"),
        ("share/qt6/resources", "share/qt6/resources"),
        ("share/qt6/translations/qtwebengine_locales", "share/qt6/translations/qtwebengine_locales"),
        ("share/fonts/truetype/dejavu", "share/fonts/truetype/dejavu"),
    ):
        files.tree(prefix / source, destination, "linux-prefix", prefix)
    for path in sorted((prefix / "share/doc").glob("*/copyright")):
        if path.is_file():
            destination = f"share/paleo/licenses/deb/{path.parent.name}/copyright"
            files.copy(path, destination, "linux-prefix", prefix)
    for directory in ("share/glvnd/egl_vendor.d", "share/qgis/grass"):
        if (prefix / directory).is_dir():
            files.tree(prefix / directory, directory, "linux-prefix", prefix)
    fonts = data_root / "etc/fonts/fonts.conf"
    target = files.copy(fonts, "etc/fonts/fonts.conf", "linux-prefix", data_root)
    config = target.read_text(encoding="utf-8").replace("../../usr/share/fonts", "../../share/fonts")
    config = re.sub(r"(?m)^[ \t]*<dir(?: prefix=\"xdg\")?>(?:fonts|~/\.fonts)</dir>\s*$", "", config)
    target.write_text(config, encoding="utf-8")
    for helper in ("bin", "lib/qt6/libexec"):
        qt_conf = files.output / helper / "qt.conf"
        relative = os.path.relpath(files.output, qt_conf.parent)
        qt_conf.write_text(f"[Paths]\nPrefix={relative}\nPlugins=lib/qt6/plugins\n"
                           "LibraryExecutables=lib/qt6/libexec\nData=share/qt6\n"
                           "Translations=share/qt6/translations\n", encoding="ascii")
    launcher = files.copy(LAUNCHER, "paleo-portable", "source", REPO_ROOT)
    launcher.chmod(0o755)
    files.copy(REPO_ROOT / "vendor/office-editor-NOTICE.txt",
               "share/paleo/licenses/office-editor/NOTICE.txt", "source", REPO_ROOT)
    return target


def stage_ca_bundle(files, scratch: Path):
    pin = json.loads((REPO_ROOT / CERT_LOCK).read_text(encoding="utf-8"))
    cache = REPO_ROOT / "vendor/cache/runtime-ca"
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / pin["url"].rsplit("/", 1)[1]
    if not archive.is_file():
        if os.environ.get("PALEO_VENDOR_OFFLINE") == "1":
            raise ValueError(f"Missing offline certificate archive: {archive}")
        with urlopen(pin["url"], timeout=60) as response:
            data = response.read(pin["size"] + 1)
        candidate = scratch / archive.name
        candidate.write_bytes(data)
        if len(data) != pin["size"] or digest(candidate) != pin["sha256"]:
            raise ValueError(f"Certificate archive size or SHA-256 mismatch: {archive.name}")
        shutil.copyfile(candidate, archive)
    if archive.stat().st_size != pin["size"] or digest(archive) != pin["sha256"]:
        raise ValueError(f"Certificate archive size or SHA-256 mismatch: {archive}")
    extracted = scratch / "certificates"
    run(["dpkg-deb", "-x", str(archive), str(extracted)])
    certs = sorted((extracted / "usr/share/ca-certificates/mozilla").glob("*.crt"))
    if not certs:
        raise ValueError(f"Certificate archive has no Mozilla PEM data: {archive.name}")
    output = files.output / "share/certs/ca-bundle.pem"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(b"".join(path.read_bytes().strip() + b"\n" for path in certs))
    files.origins[output.relative_to(files.output).as_posix()] = {
        "origin": "debian-ca-certificates", "source": archive.name, "sourceSha256": pin["sha256"],
    }
    files.copy(extracted / "usr/share/doc/ca-certificates/copyright",
               "share/paleo/licenses/ca-certificates/copyright", "debian-ca-certificates", extracted)


def install_application(files: PackageFiles, build: Path, cache):
    print(f"Installing application and Office assets from {build}", flush=True)
    subprocess.run([cache["CMAKE_COMMAND"], "--install", str(build), "--prefix", str(files.output)],
                   check=True, timeout=1800, stdout=subprocess.DEVNULL, text=True,
                   stderr=subprocess.PIPE)
    editor = files.output / "libexec/office-editor"
    required = (REPO_ROOT / "vendor/office-editor-required.txt").read_text(encoding="utf-8").splitlines()
    for name in required:
        path = editor / name
        if not path.is_file() or not path.resolve().is_relative_to(files.output) or not path.stat().st_size:
            raise ValueError(f"Office editor installation is missing a local runtime asset: {name}")
    for path in sorted(editor.rglob("*")):
        if path.is_file():
            name = path.relative_to(editor).as_posix()
            sha = digest(path)
            files.origins[path.relative_to(files.output).as_posix()] = {
                "origin": "office-editor", "source": name, "sourceSha256": sha,
                "size": path.stat().st_size, "sha256": sha,
            }


def stage_linux(files: PackageFiles, build: Path, scratch: Path):
    cache = load_cmake_cache(build)
    prefix, data_root, locks = dependency_prefix(cache)
    multiarch = prefix / "lib/x86_64-linux-gnu"
    source_root = Path(cache["CMAKE_HOME_DIRECTORY"]).resolve(strict=True)
    ort = source_root / "vendor/onnxruntime"
    approved = [("linux-prefix", prefix)]
    lib_search = [prefix / "lib", multiarch]
    grass = next(iter(sorted((prefix / "lib").glob("grass[0-9]*"))), None)
    if grass:
        lib_search.append(grass / "lib")
        files.tree(grass, f"lib/{grass.name}", "linux-prefix", prefix,
                   lambda path: path.suffix not in {".a", ".h"})
    if ort.is_dir() and cache.get("PALEO_ONNXRUNTIME") != "OFF":
        approved.append(("onnxruntime", ort.resolve()))
        lib_search.append(ort / "lib")
        for name in ("LICENSE", "ThirdPartyNotices.txt"):
            files.copy(ort / name, f"share/paleo/licenses/onnxruntime/{name}", "onnxruntime", ort)
    seeds = installable_sources(files, prefix, multiarch)
    if grass:
        for path in sorted(grass.rglob("*")):
            if is_elf(path):
                seeds[path] = files.output / path.relative_to(prefix)
    for name in ("paleo", "paleo_selfcheck", "paleo_data_snapshot_worker"):
        path = build / name
        seeds[path] = files.output / "bin" / name
        files.origins[f"bin/{name}"] = {"origin": "build", "source": name, "sourceSha256": digest(path)}
    if ort.is_dir() and cache.get("PALEO_ONNXRUNTIME") != "OFF":
        for path in (ort / "lib").glob("libonnxruntime_providers*.so"):
            seeds[path] = files.copy(path, f"lib/{path.name}", "onnxruntime", ort)
    stage_shared_libs(files, seeds, approved,
                      {"PATH": "/usr/bin:/bin", "LD_LIBRARY_PATH": os.pathsep.join(map(str, lib_search))})
    install_application(files, build, cache)
    rewritten = [stage_linux_resources(files, prefix, data_root)]
    stage_ca_bundle(files, scratch)
    for path in runtime_elfs(files.output):
        relative = os.path.relpath(files.output / "lib", path.parent)
        run(["patchelf", "--set-rpath", f"$ORIGIN/{relative}", str(path)])
        rewritten.append(path)
    files.write([*locks, "vendor/manifest.json", "vendor/office-editor-required.txt",
                 "vendor/fetch-ranuts-document.sh", CERT_LOCK],
                rewritten)


def verify_linux(output: Path):
    verify(output)
    env = {"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8"}
    for path in runtime_elfs(output):
        for name, resolved in ldd_resolutions(path, env):
            if not resolved.is_relative_to(output) and not host_abi(name, resolved):
                raise ValueError(f"{path.name} loads unbundled library {resolved}")
    with tempfile.TemporaryDirectory(prefix="paleo-runtime-check-") as scratch:
        env.update(HOME=scratch, XDG_CONFIG_HOME=f"{scratch}/config",
                   XDG_DATA_HOME=f"{scratch}/data", XDG_CACHE_HOME=f"{scratch}/cache",
                   QT_QPA_PLATFORM="offscreen", PROJ_NETWORK="OFF")
        launcher = str(output / "paleo-portable")
        print(run([launcher, "selfcheck"], env=env, cwd=scratch))
        print(run([launcher, "--check-runtime"], env=env, cwd=scratch))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--verify-only", type=Path)
    try:
        args = parser.parse_args()
        if args.verify_only:
            verify_linux(args.verify_only.resolve(strict=True))
            return
        if not args.build_dir or not args.out:
            parser.error("--build-dir and --out are required")
        output = args.out.absolute()
        if output.exists() or output.is_symlink():
            raise ValueError(f"Output directory already exists: {output}")
        output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".paleo-package-", dir=output.parent) as scratch:
            stage = Path(scratch) / "runtime"
            stage.mkdir()
            stage_linux(PackageFiles(stage), args.build_dir.resolve(strict=True), Path(scratch))
            verify_linux(stage)
            if output.exists() or output.is_symlink():
                raise ValueError(f"Output directory already exists: {output}")
            stage.rename(output)
        print(f"Linux portable runtime: {output}")
    except (KeyError, OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n{getattr(error, 'stderr', '') or ''}")


if __name__ == "__main__":
    main()
