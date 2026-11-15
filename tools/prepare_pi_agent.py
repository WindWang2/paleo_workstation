#!/usr/bin/env python3
# 层：测试壳
"""Explicit locked Pi supply build; no global npm, package scripts or app-startup installation."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import uuid
import zipfile
from prepare_agent_python import StoreLock
from pi_supply_common import ROOT, VENDOR, MANIFEST, PLATFORMS, archive_member, canonical, check_archive, digest, extract_sources, fetch, linked, load_lock, verify_runtime


def host_platform():
    if platform.machine().lower() not in ("amd64", "x86_64"):
        raise ValueError("unsupported supply build CPU")
    if os.name == "nt":
        return "windows-x86_64"
    if sys.platform.startswith("linux"):
        return "linux-x86_64"
    raise ValueError("unsupported supply build platform")


def payload_files(root):
    root = Path(root)
    # WindowsPath ordering folds case; manifests and ZIPs use the same portable ordinal order.
    return sorted((p for p in root.rglob("*") if p.is_file()), key=lambda p: p.relative_to(root).as_posix())


def discard_owned(path, parent):
    path, parent = Path(path), Path(parent).resolve()
    if not path.exists():
        return
    if linked(path) or path.resolve() == parent or not path.resolve().is_relative_to(parent):
        raise ValueError("refusing cleanup outside owned staging root")
    entries = list(path.rglob("*"))
    if any(linked(p) for p in entries):
        raise ValueError("refusing linked staging cleanup")
    path.chmod(0o700)
    for p in entries:
        p.chmod(0o700 if p.is_dir() else 0o600)
    shutil.rmtree(path)


def build_bundle(cache, work, lock):
    native = lock["platforms"][host_platform()]
    (work / "package.json").write_bytes(canonical({"type": "commonjs", "private": True}))
    node = work / ("node.exe" if os.name == "nt" else "node")
    archive_member(cache / native["nodeArchive"], native["programMember"], node, native["programSha256"])
    node.chmod(0o700)
    wrapper = work / "esbuild/lib/main.js"
    archive_member(cache / lock["build"]["esbuildArchive"], lock["build"]["esbuildMain"], wrapper, maximum=1024 * 1024)
    compiler = work / ("esbuild.exe" if os.name == "nt" else "esbuild-bin")
    archive_member(cache / native["esbuildArchive"], native["esbuildMember"], compiler, maximum=32 * 1024 * 1024)
    compiler.chmod(0o700)
    source = work / "source"; source.mkdir()
    extract_sources(cache, source, lock)
    environment = {"LANG": "C.UTF-8", "HOME": str(work), "USERPROFILE": str(work), "TEMP": str(work), "TMP": str(work),
                   "ESBUILD_BINARY_PATH": str(compiler)}
    if os.name == "nt":
        environment["SystemRoot"] = os.environ["SystemRoot"]
    output = work / "bundle"
    command = [str(node), str(ROOT / lock["build"]["recipe"]), str(wrapper), str(source), str(ROOT / lock["build"]["entry"]), str(output), str(VENDOR / "source.lock.json")]
    result = subprocess.run(command, cwd=work, env=environment, capture_output=True, timeout=120,
                            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    if result.returncode:
        raise ValueError("locked Pi bundle build failed: " + result.stderr[:4096].decode("utf-8", "replace"))
    if (output / "sidecar.mjs").stat().st_size != lock["build"]["bundleSize"] or digest(output / "sidecar.mjs") != lock["build"]["bundleSha256"] or digest(output / "bundle-graph.json") != lock["build"]["graphSha256"]:
        raise ValueError("offline build differs from reviewed bundle/entry graph")
    return output


def materialize(cache, work, stage, target, lock, source_sha):
    bundle = build_bundle(cache, work, lock)
    spec = lock["platforms"][target]
    archive_member(cache / spec["nodeArchive"], spec["programMember"], stage / spec["program"], spec["programSha256"])
    archive_member(cache / spec["nodeArchive"], spec["licenseMember"], stage / "licenses/node/LICENSE.txt", maximum=4 * 1024 * 1024)
    (stage / "agent").mkdir()
    (stage / "agent/package.json").write_bytes(canonical({"type": "module", "private": True}))
    shutil.copyfile(bundle / "sidecar.mjs", stage / "agent/sidecar.mjs")
    shutil.copyfile(bundle / "bundle-graph.json", stage / "agent/bundle-graph.json")
    (stage / "licenses/pi").mkdir(parents=True)
    shutil.copyfile(VENDOR / "LICENSE.upstream", stage / "licenses/pi/LICENSE.txt")
    for source in sorted((VENDOR / "THIRD_PARTY_NOTICES").rglob("*")):
        if source.is_file():
            destination = stage / "licenses/third-party" / source.relative_to(VENDOR / "THIRD_PARTY_NOTICES")
            destination.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(source, destination)
    provenance = {"schema": "paleo.pi-build-provenance.v1", "sourceLockSha256": source_sha,
                  "upstreamCommit": lock["upstream"]["commit"], "sourceArchiveSha256": lock["archives"][lock["upstream"]["archive"]]["sha256"],
                  "upstreamPackageLockSha256": lock["upstream"]["packageLockSha256"], "recipeSha256": lock["build"]["recipeSha256"],
                  "entrySha256": lock["build"]["entrySha256"], "esbuildVersion": lock["build"]["esbuildVersion"],
                  "runtimeExternalImports": [], "patches": lock["upstream"]["patches"]}
    (stage / "build-provenance.json").write_bytes(canonical(provenance))
    rows = []
    for path in payload_files(stage):
        name = path.relative_to(stage).as_posix()
        rows.append({"path": name, "size": path.stat().st_size, "sha256": digest(path), "executable": name == spec["program"]})
    manifest = {"schema": "paleo.pi-runtime.v1", "sourceLockSha256": source_sha, "runtimeVersion": lock["runtimeVersion"],
                "upstreamCommit": lock["upstream"]["commit"], "coreVersion": lock["coreVersion"], "nodeVersion": lock["nodeVersion"],
                "platform": target, "node": spec["program"], "entry": "agent/sidecar.mjs", "treeSize": sum(row["size"] for row in rows), "files": rows}
    raw = canonical(manifest); (stage / MANIFEST).write_bytes(raw)
    anchor = hashlib.sha256(raw).hexdigest(); verify_runtime(stage, anchor, lock)
    for row in rows:
        (stage / row["path"]).chmod(0o555 if row["executable"] else 0o444)
    (stage / MANIFEST).chmod(0o444)
    for path in sorted(stage.rglob("*"), reverse=True):
        if path.is_dir():
            path.chmod(0o555)
    stage.chmod(0o555)
    return anchor


def package_generation(root, archive):
    archive = Path(archive)
    if linked(archive):
        raise ValueError("linked output package")
    archive = archive.resolve(); archive.parent.mkdir(parents=True, exist_ok=True)
    if archive.exists() or linked(archive):
        raise ValueError("output package already exists")
    temporary = archive.with_name(archive.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        with zipfile.ZipFile(temporary, "x", compression=zipfile.ZIP_STORED, allowZip64=True) as output:
            for path in payload_files(root):
                name = path.relative_to(root).as_posix(); info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
                info.create_system = 3; info.external_attr = (0o100555 if name in ("bin/node", "bin/node.exe") else 0o100444) << 16
                with path.open("rb") as source, output.open(info, "w", force_zip64=True) as target:
                    shutil.copyfileobj(source, target, 1024 * 1024)
        os.link(temporary, archive)  # Atomic create without replacing another package.
    finally:
        temporary.unlink(missing_ok=True)


def prepare(cache, store, target, lock, source_sha, archive=None):
    if linked(cache) or linked(store):
        raise ValueError("linked supply root")
    cache, store = Path(cache).resolve(), Path(store).resolve()
    if cache == store or cache.is_relative_to(store) or store.is_relative_to(cache):
        raise ValueError("cache and runtime store must be separate roots")
    cache.mkdir(parents=True, exist_ok=True); store.mkdir(parents=True, exist_ok=True)
    marker = store / ".pi-store.json"
    if marker.exists():
        if read_store_marker(marker) != {"schema": "paleo.pi-store.v1"}:
            raise ValueError("foreign runtime store")
    elif any(store.iterdir()):
        raise ValueError("nonempty unregistered runtime store")
    else:
        marker.write_bytes(canonical({"schema": "paleo.pi-store.v1"}))
    reserve = 2 * max(p["programSize"] for p in lock["platforms"].values()) + 128 * 1024 * 1024
    if min(shutil.disk_usage(cache).free, shutil.disk_usage(store).free) < reserve:
        raise ValueError("Pi supply staging disk budget unavailable")
    generations = store / "generations"
    if linked(generations):
        raise ValueError("linked generation root")
    generations.mkdir(exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=".pi-build-", dir=cache))
    stage = Path(tempfile.mkdtemp(prefix=".pi-stage-", dir=generations))
    try:
        if linked(cache / ".prepare.lock") or linked(store / ".prepare.lock"):
            raise ValueError("linked supply lock")
        with StoreLock(cache), StoreLock(store):
            for name, pin in lock["archives"].items():
                check_archive(cache / name, pin)
            anchor = materialize(cache, work, stage, target, lock, source_sha)
            # All fallible build/duplicate-stage cleanup precedes active-pointer publication.
            discard_owned(work, cache)
            destination = generations / anchor
            if destination.exists():
                verify_runtime(destination, anchor, lock)
            else:
                stage.rename(destination)
            if archive:
                package_generation(destination, archive)
            discard_owned(stage, generations)
            temporary = store / (".current-" + uuid.uuid4().hex)
            try:
                with temporary.open("xb") as output:
                    output.write(canonical({"schema": "paleo.pi-active.v1", "generation": "generations/" + anchor, "manifestSha256": anchor}))
                    output.flush(); os.fsync(output.fileno())
                temporary.replace(store / "current.json")
            finally:
                temporary.unlink(missing_ok=True)
            return destination, anchor
    finally:
        discard_owned(work, cache); discard_owned(stage, generations)


def read_store_marker(path):
    from pi_supply_common import read_json
    if linked(path):
        raise ValueError("linked store marker")
    return read_json(path, 1024)[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", required=True, type=Path)
    network = parser.add_mutually_exclusive_group()
    network.add_argument("--fetch", action="store_true", help="explicitly download the locked offline closure")
    network.add_argument("--offline", action="store_true", help="use only already verified cache objects (also the default)")
    parser.add_argument("--store", type=Path)
    parser.add_argument("--platform", choices=sorted(PLATFORMS))
    parser.add_argument("--archive", type=Path)
    args = parser.parse_args(); lock, source_sha = load_lock()
    if args.fetch:
        if linked(args.cache) or linked(args.cache / ".prepare.lock"):
            raise ValueError("linked supply cache/lock")
        args.cache.mkdir(parents=True, exist_ok=True)
        with StoreLock(args.cache):
            fetch(args.cache, lock)
    if args.store:
        root, anchor = prepare(args.cache, args.store, args.platform or host_platform(), lock, source_sha, args.archive)
        print(json.dumps({"root": str(root), "manifestSha256": anchor, "enabled": False}, sort_keys=True))
    elif not args.fetch or args.archive or args.platform:
        parser.error("--store required for preparation/package output; network only occurs with --fetch")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.SubprocessError, zipfile.BadZipFile) as error:
        print("Pi supply failed: " + str(error), file=sys.stderr)
        raise SystemExit(1)
