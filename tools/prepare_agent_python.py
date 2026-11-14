#!/usr/bin/env python3
# 层：测试壳
"""Explicit developer supply tool; never an Agent tool or a default installation step."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import posixpath
import shutil
import stat
import subprocess
import sys
import tarfile
from urllib.parse import urlsplit
import urllib.request
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_LOCK = ROOT / "vendor/agent-python/runtime.lock.json"
MANIFEST = "runtime-manifest.json"
SCHEMA = "paleo.python-runtime.v1"


def canonical(value):
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


def digest_file(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def relative_path(value):
    if not isinstance(value, str) or not value or len(value) > 512 or "\\" in value or ":" in value:
        raise ValueError("invalid relative path")
    parts = value.split("/")
    if any(p in ("", ".", "..") or any(ord(c) < 32 for c in p) or p.endswith((" ", ".")) for p in parts):
        raise ValueError("invalid relative path: " + value)
    if PurePosixPath(value).is_absolute():
        raise ValueError("absolute path")
    reserved = {"CON", "PRN", "AUX", "NUL", *("COM" + str(n) for n in range(1, 10)), *("LPT" + str(n) for n in range(1, 10))}
    if any(p.split(".")[0].upper() in reserved or any(c in p for c in '<>"|?*') for p in parts):
        raise ValueError("nonportable path: " + value)
    return value


def load_lock(path=DEFAULT_LOCK):
    raw = Path(path).read_bytes()
    lock = json.loads(raw)
    if lock.get("schema") != "paleo.agent-python.supply.v1" or lock.get("profile") != "analysis-core":
        raise ValueError("unsupported source lock")
    if set(lock.get("platforms", {})) != {"windows-x86_64", "linux-x86_64"}:
        raise ValueError("source lock platform mismatch")
    for key in ("runtimeVersion", "pythonVersion", "numpyVersion"):
        if not isinstance(lock.get(key), str) or not lock[key]:
            raise ValueError("missing " + key)
    limits = lock.get("limits", {})
    if set(limits) != {"maxFiles", "maxTreeBytes"} or any(type(n) is not int or n <= 0 for n in limits.values()):
        raise ValueError("invalid source lock budgets")
    if limits["maxFiles"] > 16384 or limits["maxTreeBytes"] > 1024 * 1024 * 1024:
        raise ValueError("source lock exceeds supply budgets")
    for target, spec in lock["platforms"].items():
        relative_path(spec["interpreter"])
        for path in [*spec.get("textFiles", {}), *spec.get("omitPaths", [])]:
            relative_path(path)
        if "numpyDirectory" in spec:
            relative_path(spec["numpyDirectory"])
        if {s.get("role") for s in spec["sources"]} != {"python", "numpy"} or len(spec["sources"]) != 2:
            raise ValueError("exactly one Python archive and one NumPy wheel required")
        for source in spec["sources"]:
            name = relative_path(source["name"])
            if "/" in name or source["format"] not in ("zip", "tar.gz"):
                raise ValueError("invalid archive declaration")
            h = source.get("sha256", "")
            if len(h) != 64 or any(c not in "0123456789abcdef" for c in h) or h == "0" * 64:
                raise ValueError("source must have a nonzero SHA256")
            if not isinstance(source.get("size"), int) or not 0 < source["size"] <= 512 * 1024 * 1024:
                raise ValueError("invalid source size")
            url = urlsplit(source["url"])
            if url.scheme != "https" or url.username or url.password or url.query or url.fragment:
                raise ValueError("invalid source URL")
            allowed = url.hostname in ("www.python.org", "files.pythonhosted.org")
            allowed |= url.hostname == "github.com" and url.path.startswith("/astral-sh/python-build-standalone/releases/download/")
            if not allowed or not source.get("supplier") or not source.get("license") or not source.get("digestEvidence"):
                raise ValueError("unregistered source supplier")
    return lock, hashlib.sha256(raw).hexdigest()


class StoreLock:
    """An OS lock released on process death; the lock file is outside immutable generations."""
    def __init__(self, store):
        self.file = None
        self.path = Path(store) / ".prepare.lock"

    def __enter__(self):
        self.file = self.path.open("a+b")
        if self.file.tell() == 0:
            self.file.write(b"0")
            self.file.flush()
        self.file.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(self.file.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(self.file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            self.file.close()
            raise RuntimeError("runtime preparation already in progress") from None
        return self

    def __exit__(self, *unused):
        self.file.close()


def archive_source(source, cache, fetch=False):
    path = Path(cache) / source["name"]
    if path.exists() and (path.is_symlink() or not path.is_file()):
        raise ValueError("invalid cache entry")
    valid = path.exists() and path.stat().st_size == source["size"] and digest_file(path) == source["sha256"]
    if valid:
        return path
    if not fetch:
        raise ValueError("offline source absent or corrupt: " + source["name"])
    temporary = path.with_name(path.name + ".download-" + uuid.uuid4().hex)
    try:
        request = urllib.request.Request(source["url"], headers={"User-Agent": "Paleo-Agent-Python-Supply/1"})
        with urllib.request.urlopen(request, timeout=60) as response, temporary.open("xb") as out:
            count = 0
            while block := response.read(1024 * 1024):
                count += len(block)
                if count > source["size"]:
                    raise ValueError("source exceeded locked size")
                out.write(block)
        if temporary.stat().st_size != source["size"] or digest_file(temporary) != source["sha256"]:
            raise ValueError("download did not match source lock")
        os.replace(temporary, path)
        return path
    finally:
        temporary.unlink(missing_ok=True)


def _archive_name(name, prefix):
    name = name.rstrip("/")
    if prefix and name == prefix.rstrip("/"):
        return None
    if prefix and not name.startswith(prefix):
        raise ValueError("archive member outside declared prefix")
    name = name[len(prefix):]
    return relative_path(name)


def unpack(source, archive, destination, occupied, limits, target, omit_paths=()):
    """Extract regular bytes only; resolve pinned tar internal file links, never create links."""
    prefix = source.get("prefix", "")
    seen = set()
    total = 0
    modes = {}

    def omitted(name):
        return name is not None and any(name == path or name.startswith(path + "/") for path in omit_paths)

    def write(name, length, executable, stream):
        nonlocal total
        name = _archive_name(name, prefix)
        if name is None or omitted(name):
            return
        key = name.casefold()
        if key in seen or key in occupied or key == MANIFEST.casefold():
            raise ValueError("duplicate archive file: " + name)
        seen.add(key)
        total += length
        if len(occupied) + 1 > limits["maxFiles"] or total > limits["maxTreeBytes"]:
            raise ValueError("archive budget exceeded")
        path = destination / name
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("xb") as output:
            shutil.copyfileobj(stream, output, 1024 * 1024)
        if path.stat().st_size != length:
            raise ValueError("archive length mismatch")
        path.chmod(0o755 if executable else 0o644)
        occupied.add(key)
        modes[name] = executable

    if source["format"] == "zip":
        with zipfile.ZipFile(archive) as z:
            for info in z.infolist():
                name = info.orig_filename
                if info.is_dir():
                    _archive_name(name, prefix)
                    continue
                mode = info.external_attr >> 16
                if stat.S_ISLNK(mode) or (stat.S_IFMT(mode) not in (0, stat.S_IFREG)):
                    raise ValueError("nonregular zip entry")
                with z.open(info) as stream:
                    write(name, info.file_size, bool(mode & 0o111) or name.lower().endswith(".exe"), stream)
    else:
        with tarfile.open(archive, "r:gz") as tar:
            members = tar.getmembers()
            mapping = {m.name.rstrip("/"): m for m in members}
            if len(mapping) != len(members):
                raise ValueError("duplicate tar member")
            for original in members:
                relative = _archive_name(original.name, prefix)
                if omitted(relative):
                    continue
                if original.isdir():
                    continue
                current, chain = original, set()
                while current.issym() or current.islnk():
                    if current.name in chain or len(chain) > 32:
                        raise ValueError("cyclic archive link")
                    chain.add(current.name)
                    link = current.linkname
                    if PurePosixPath(link).is_absolute() or "\\" in link:
                        raise ValueError("absolute archive link")
                    link = posixpath.normpath(posixpath.join(posixpath.dirname(current.name), link)) if current.issym() else posixpath.normpath(link)
                    _archive_name(link, prefix)
                    if link not in mapping:
                        raise ValueError("dangling archive link")
                    current = mapping[link]
                if not current.isfile():
                    raise ValueError("unsupported archive member")
                with tar.extractfile(current) as stream:
                    write(original.name, current.size, bool(current.mode & 0o111), stream)
    return modes


def remove_stage(path, store):
    path, store = Path(path), Path(store)
    if path.parent.resolve() != store.resolve() or not path.name.startswith(".stage-"):
        raise ValueError("cleanup target outside runtime store")
    def retry(function, name, exc):
        os.chmod(name, 0o700)
        function(name)
    if path.exists():
        shutil.rmtree(path, onerror=retry)


def tree_files(root):
    result = []
    for path in sorted(Path(root).rglob("*")):
        info = path.lstat()
        if path.is_symlink() or getattr(info, "st_file_attributes", 0) & 0x400:
            raise ValueError("runtime tree contains link/reparse point")
        if path.is_dir():
            continue
        if not path.is_file():
            raise ValueError("runtime tree contains nonregular file")
        name = relative_path(path.relative_to(root).as_posix())
        if name != MANIFEST:
            result.append({"path": name, "size": info.st_size, "sha256": digest_file(path), "executable": bool(info.st_mode & 0o111)})
    return sorted(result, key=lambda row: row["path"])


def verify_tree(root, expected_manifest_sha):
    root = Path(root)
    for path in (root, root / MANIFEST):
        info = path.lstat()
        if path.is_symlink() or getattr(info, "st_file_attributes", 0) & 0x400:
            raise ValueError("runtime root/manifest is a link or reparse point")
    if not expected_manifest_sha or digest_file(root / MANIFEST) != expected_manifest_sha:
        raise ValueError("runtime manifest anchor mismatch")
    manifest = json.loads((root / MANIFEST).read_bytes())
    actual = tree_files(root)
    # Executability is an installation-mode fact on POSIX; Windows has ACLs rather than mode bits.
    if os.name == "nt":
        expected = [{k: v for k, v in row.items() if k != "executable"} for row in manifest["files"]]
        actual = [{k: v for k, v in row.items() if k != "executable"} for row in actual]
    else:
        expected = manifest["files"]
    if manifest.get("schema") != SCHEMA or actual != expected:
        raise ValueError("runtime tree content mismatch")
    return manifest


def probe_runtime(root, manifest):
    native = "windows-x86_64" if os.name == "nt" else "linux-x86_64"
    if manifest["platform"] != native or platform.machine().lower() not in ("amd64", "x86_64"):
        raise ValueError("cannot probe a foreign platform")
    env = {"PATH": str(Path(root) / ("" if os.name == "nt" else "bin")), "LANG": "C.UTF-8",
           "OPENBLAS_NUM_THREADS": "1", "OMP_NUM_THREADS": "1", "MKL_NUM_THREADS": "1"}
    if os.name == "nt":
        env.update({key: os.environ[key] for key in ("SystemRoot", "WINDIR") if key in os.environ})
    code = "import sys,json,numpy as n;print(json.dumps({'pythonVersion':'.'.join(map(str,sys.version_info[:3])),'numpyVersion':n.__version__,'dot':float(n.arange(5,dtype=n.float64)@n.arange(5,dtype=n.float64))}))"
    result = subprocess.run([str(Path(root) / manifest["interpreter"]), "-I", "-u", "-B", "-X", "utf8", "-c", code],
                            env=env, cwd=root, capture_output=True, text=True, timeout=30, check=True)
    value = json.loads(result.stdout)
    if value != {"pythonVersion": manifest["pythonVersion"], "numpyVersion": manifest["numpyVersion"], "dot": 30.0}:
        raise ValueError("runtime probe identity/numerical mismatch")
    return value


def prepare(lock, source_lock_sha, target, cache, store, fetch=False, probe=False):
    store, cache = Path(store).resolve(), Path(cache).resolve()
    store.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    spec = lock["platforms"][target]
    with StoreLock(store):
        stage = store / (".stage-" + uuid.uuid4().hex)
        stage.mkdir()
        try:
            executable_paths = set()
            for source in spec["sources"]:
                destination = stage / spec.get("numpyDirectory", "") if source["role"] == "numpy" else stage
                destination.mkdir(parents=True, exist_ok=True)
                occupied = {row["path"].casefold() for row in tree_files(destination)}
                omissions = spec.get("omitPaths", ()) if source["role"] == "python" else ()
                modes = unpack(source, archive_source(source, cache, fetch), destination, occupied, lock["limits"], target, omissions)
                executable_paths.update((destination / name).relative_to(stage).as_posix() for name, executable in modes.items() if executable)
            for name in spec.get("omitPaths", []):
                path = stage / name
                if path.is_dir():
                    shutil.rmtree(path)
                elif path.exists():
                    path.unlink()
            for name, value in spec.get("textFiles", {}).items():
                (stage / name).write_text(value, encoding="utf-8", newline="\n")
            files = tree_files(stage)
            for row in files:
                row["executable"] = row["path"].lower().endswith(".exe") if target.startswith("windows") else row["path"] in executable_paths
            total = sum(row["size"] for row in files)
            if len(files) > lock["limits"]["maxFiles"] or total > lock["limits"]["maxTreeBytes"]:
                raise ValueError("combined runtime exceeds budget")
            if not any(row["path"] == spec["interpreter"] for row in files):
                raise ValueError("runtime interpreter missing")
            manifest = {key: lock[key] for key in ("profile", "runtimeVersion", "pythonVersion", "numpyVersion")}
            manifest.update(schema=SCHEMA, platform=target, interpreter=spec["interpreter"], files=files,
                            sourceLockSha256=source_lock_sha, treeSize=total)
            raw = canonical(manifest)
            if len(raw) > 4 * 1024 * 1024:
                raise ValueError("runtime manifest exceeded 4 MiB")
            anchor = hashlib.sha256(raw).hexdigest()
            (stage / MANIFEST).write_bytes(raw)
            verify_tree(stage, anchor)
            generations = store / "generations"
            generations.mkdir(exist_ok=True)
            generation = generations / anchor
            if generation.exists():
                verify_tree(generation, anchor)
                remove_stage(stage, store)
            else:
                for path in stage.rglob("*"):
                    if path.is_file():
                        path.chmod(0o555 if os.name != "nt" and path.stat().st_mode & 0o111 else 0o444)
                os.replace(stage, generation)
            # Probe the stable path: Windows can keep executable/DLL mappings alive briefly
            # after process exit, which would make a subsequent directory rename fail.
            proof = probe_runtime(generation, manifest) if probe else None
            verify_tree(generation, anchor)
            selection = {"schema": "paleo.python-runtime.selection.v1", "platform": target,
                         "runtimeVersion": lock["runtimeVersion"], "manifestSha256": anchor,
                         "generation": generation.relative_to(store).as_posix()}
            pointer = store / ("current-" + target + ".json")
            temporary = store / (".current-" + uuid.uuid4().hex)
            try:
                with temporary.open("xb") as out:
                    out.write(canonical(selection))
                    out.flush()
                    os.fsync(out.fileno())
                os.replace(temporary, pointer)
            finally:
                temporary.unlink(missing_ok=True)
            return {**selection, "directory": str(generation), "probe": proof}
        finally:
            remove_stage(stage, store)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", type=Path, default=DEFAULT_LOCK)
    parser.add_argument("--platform", required=True, choices=("windows-x86_64", "linux-x86_64"))
    parser.add_argument("--cache", required=True, type=Path)
    parser.add_argument("--store", required=True, type=Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--offline", action="store_true", help="default: consume only locked local archives")
    mode.add_argument("--fetch", action="store_true", help="explicit supplier fetch; never used by Agent execution")
    parser.add_argument("--probe", action="store_true", help="run the locked native Python/NumPy import and numerical check")
    args = parser.parse_args(argv)
    try:
        lock, anchor = load_lock(args.lock)
        value = prepare(lock, anchor, args.platform, args.cache, args.store, args.fetch, args.probe)
        print(json.dumps(value, sort_keys=True))
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, zipfile.BadZipFile, tarfile.TarError) as error:
        print("agent-python: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
