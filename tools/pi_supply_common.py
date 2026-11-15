#!/usr/bin/env python3
# 层：测试壳
"""Small shared helpers for explicit Pi supply tooling, never application startup."""
from __future__ import annotations
import base64
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import tarfile
from urllib.parse import urlsplit
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / "vendor/pi"
MANIFEST = "pi-manifest.json"
PLATFORMS = {"windows-x86_64", "linux-x86_64"}


def canonical(value):
    return (json.dumps(value, sort_keys=True, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")


def unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError("duplicate JSON key")
        value[key] = item
    return value


def read_json(path, maximum=1024 * 1024):
    with Path(path).open("rb") as stream:
        raw = stream.read(maximum + 1)
    if len(raw) > maximum:
        raise ValueError("JSON byte budget exceeded")
    return json.loads(raw.decode("utf-8"), object_pairs_hook=unique_object), raw


def digest(path, algorithm="sha256"):
    h = hashlib.new(algorithm)
    with Path(path).open("rb") as stream:
        for part in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(part)
    return h.hexdigest()


def hex_digest(value):
    return isinstance(value, str) and len(value) == 64 and value != "0" * 64 and all(c in "0123456789abcdef" for c in value)


def relative(value):
    if not isinstance(value, str) or not value or len(value) > 512 or "\\" in value or ":" in value or PurePosixPath(value).is_absolute():
        raise ValueError("invalid relative path")
    reserved = {"CON", "PRN", "AUX", "NUL", *("COM" + str(n) for n in range(1, 10)), *("LPT" + str(n) for n in range(1, 10))}
    for part in value.split("/"):
        if part in ("", ".", "..") or part.endswith((" ", ".")) or part.split(".")[0].upper() in reserved or any(ord(c) < 32 or c in '<>"|?*' for c in part):
            raise ValueError("nonportable relative path")
    return value


def linked(path):
    path = Path(path)
    return path.is_symlink() or getattr(path, "is_junction", lambda: False)()


def load_lock():
    lock, raw = read_json(VENDOR / "source.lock.json", 128 * 1024)
    if set(lock) != {"schema", "runtimeVersion", "coreVersion", "nodeVersion", "upstream", "nodeEvidence", "archives", "platforms", "build", "limits", "notices"} or lock.get("schema") != "paleo.pi-agent.supply.v1" or set(lock.get("platforms", {})) != PLATFORMS:
        raise ValueError("unsupported Pi source lock")
    if lock["upstream"]["commit"] != "c3cc91632d0a1950fb122605b190233f2aa0006e" or lock["coreVersion"] != "1.5.2" or lock["nodeVersion"] != "22.23.5":
        raise ValueError("unreviewed Pi or Node version")
    caps = {"maxFiles": 512, "maxTreeBytes": 256 * 1024 * 1024, "maxSourceFiles": 4096, "maxSourceBytes": 64 * 1024 * 1024}
    if set(lock["limits"]) != set(caps) or any(type(lock["limits"][key]) is not int or not 0 < lock["limits"][key] <= cap for key, cap in caps.items()):
        raise ValueError("invalid source/runtime budgets")
    for name, item in lock["archives"].items():
        if "/" in relative(name) or not hex_digest(item["sha256"]) or type(item["size"]) is not int or not 0 < item["size"] <= 128 * 1024 * 1024:
            raise ValueError("invalid archive pin")
        url = urlsplit(item["url"])
        if url.scheme != "https" or url.hostname not in {"nodejs.org", "codeload.github.com", "registry.npmjs.org"} or url.username or url.password or url.query or url.fragment:
            raise ValueError("unregistered archive supplier")
        if "integrity" in item:
            kind, value = item["integrity"].split("-", 1)
            if kind != "sha512" or len(base64.b64decode(value, validate=True)) != 64:
                raise ValueError("invalid package SRI")
    for p in lock["platforms"].values():
        if not hex_digest(p["programSha256"]) or p["nodeArchive"] not in lock["archives"] or p["esbuildArchive"] not in lock["archives"]:
            raise ValueError("invalid platform closure")
        for key in ("program", "programMember", "licenseMember", "esbuildMember"):
            relative(p[key])
    checks = [(VENDOR / lock["upstream"]["sourceFiles"], lock["upstream"]["sourceFilesSha256"]),
              (VENDOR / lock["upstream"]["packageLock"], lock["upstream"]["packageLockSha256"]),
              (VENDOR / lock["nodeEvidence"]["file"], lock["nodeEvidence"]["sha256"]),
              (VENDOR / lock["build"]["graph"], lock["build"]["graphSha256"]),
              (ROOT / lock["build"]["entry"], lock["build"]["entrySha256"]),
              (ROOT / lock["build"]["recipe"], lock["build"]["recipeSha256"]),
              (ROOT / "tools/prepare_pi_agent.py", lock["build"]["preparerSha256"]),
              (ROOT / "tools/pi_supply_common.py", lock["build"]["commonSha256"]),
              (ROOT / "tools/prepare_agent_python.py", lock["build"]["storeLockHelperSha256"])]
    modules = lock["build"].get("localModules", {})
    if not isinstance(modules, dict) or set(modules) - {"conversation.mjs"}:
        raise ValueError("unreviewed local runtime module")
    for name, expected in modules.items():
        path = VENDOR / relative(name)
        if path.stat().st_size > 64 * 1024:
            raise ValueError("local runtime module byte budget")
        checks.append((path, expected))
    for notice in lock["notices"]:
        path = VENDOR / relative(notice["path"])
        if path.stat().st_size != notice["size"]:
            raise ValueError("notice byte mismatch")
        checks.append((path, notice["sha256"]))
    for path, expected in checks:
        if not hex_digest(expected) or digest(path) != expected:
            raise ValueError("source recipe/evidence hash mismatch: " + path.name)
    sums = dict(line.split()[::-1] for line in (VENDOR / lock["nodeEvidence"]["file"]).read_text("utf-8").splitlines() if line.strip())
    for target in lock["platforms"].values():
        name = target["nodeArchive"]
        if sums.get(name) != lock["archives"][name]["sha256"]:
            raise ValueError("Node pin disagrees with official release evidence")
    packages, _ = read_json(VENDOR / lock["upstream"]["packageLock"])
    for name in ("esbuild", "@esbuild/win32-x64", "@esbuild/linux-x64"):
        pin = packages["packages"]["node_modules/" + name]
        if not any(item.get("integrity") == pin["integrity"] and item["url"] == pin["resolved"] for item in lock["archives"].values()):
            raise ValueError("build package pin disagrees with upstream package lock")
    return lock, hashlib.sha256(raw).hexdigest()


def check_archive(path, pin):
    path = Path(path)
    if linked(path) or not path.is_file() or path.stat().st_size != pin["size"] or digest(path) != pin["sha256"]:
        raise ValueError("archive size/SHA mismatch: " + path.name)
    if "integrity" in pin:
        actual = base64.b64encode(bytes.fromhex(digest(path, "sha512"))).decode("ascii")
        if "sha512-" + actual != pin["integrity"]:
            raise ValueError("package SRI mismatch")


def fetch(cache, lock):
    cache = Path(cache); cache.mkdir(parents=True, exist_ok=True)
    for name, pin in lock["archives"].items():
        target = cache / name
        if not target.exists():
            temporary = target.with_name(target.name + ".part")
            if linked(temporary):
                raise ValueError("linked download target")
            try:
                with urllib.request.urlopen(pin["url"], timeout=60) as response, temporary.open("wb") as output:
                    if urlsplit(response.geturl()).hostname != urlsplit(pin["url"]).hostname:
                        raise ValueError("unregistered download redirect")
                    total = 0
                    while part := response.read(1024 * 1024):
                        total += len(part)
                        if total > pin["size"]:
                            raise ValueError("download byte budget exceeded")
                        output.write(part)
                check_archive(temporary, pin)
                temporary.replace(target)
            finally:
                temporary.unlink(missing_ok=True)
        check_archive(target, pin)


def copy_checked(stream, destination, size, sha):
    destination = Path(destination); destination.parent.mkdir(parents=True, exist_ok=True)
    h = hashlib.sha256(); remaining = size
    with destination.open("xb") as output:
        while remaining:
            part = stream.read(min(1024 * 1024, remaining))
            if not part:
                raise ValueError("truncated archive member")
            output.write(part); h.update(part); remaining -= len(part)
        if stream.read(1) or h.hexdigest() != sha:
            raise ValueError("archive member identity mismatch")


def extract_sources(cache, destination, lock):
    inventory, _ = read_json(VENDOR / lock["upstream"]["sourceFiles"])
    rows = {relative(row["path"]): row for row in inventory["files"]}
    if len(rows) != len(inventory["files"]) or len(rows) > lock["limits"]["maxSourceFiles"] or sum(row["size"] for row in rows.values()) > lock["limits"]["maxSourceBytes"]:
        raise ValueError("source inventory budget")
    path = Path(cache) / lock["upstream"]["archive"]; check_archive(path, lock["archives"][path.name])
    seen = set(); prefix = "pi-" + lock["upstream"]["commit"] + "/"
    with tarfile.open(path, "r|gz") as archive:
        for member in archive:
            if member.isdir() and member.name.rstrip("/") == prefix.rstrip("/"):
                continue
            if not member.name.startswith(prefix):
                raise ValueError("unexpected source archive root")
            name = member.name[len(prefix):]
            if name not in rows:
                continue
            row = rows[name]
            if name in seen or not member.isfile() or member.size != row["size"]:
                raise ValueError("source archive duplicate/link/member mismatch")
            with archive.extractfile(member) as source:
                copy_checked(source, Path(destination) / name, row["size"], row["sha256"])
            seen.add(name)
    if seen != set(rows):
        raise ValueError("source archive missing selected files")


def archive_member(archive_path, name, destination, expected_sha=None, maximum=256 * 1024 * 1024):
    relative(name)
    if str(archive_path).endswith(".zip"):
        with zipfile.ZipFile(archive_path) as archive:
            entries = [row for row in archive.infolist() if row.filename == name]
            if len(entries) != 1 or entries[0].is_dir() or stat.S_ISLNK(entries[0].external_attr >> 16) or not 0 < entries[0].file_size <= maximum:
                raise ValueError("archive member missing/linked/oversized")
            with archive.open(entries[0]) as source:
                raw = source.read(maximum + 1)
    else:
        found = False; raw = None
        mode = "r|xz" if str(archive_path).endswith(".xz") else "r|gz"
        with tarfile.open(archive_path, mode) as archive:
            for row in archive:
                if row.name != name:
                    continue
                if found or not row.isfile() or not 0 < row.size <= maximum:
                    raise ValueError("archive duplicate/link/oversized member")
                found = True
                with archive.extractfile(row) as source:
                    raw = source.read(maximum + 1)
        if not found:
            raise ValueError("missing archive member")
    if len(raw) > maximum or (expected_sha and hashlib.sha256(raw).hexdigest() != expected_sha):
        raise ValueError("archive member budget/SHA mismatch")
    destination = Path(destination); destination.parent.mkdir(parents=True, exist_ok=True); destination.write_bytes(raw)


def verify_runtime(root, expected_sha, lock):
    root = Path(root)
    if linked(root) or not root.is_dir() or not hex_digest(expected_sha):
        raise ValueError("runtime identity required")
    value, raw = read_json(root / MANIFEST)
    if hashlib.sha256(raw).hexdigest() != expected_sha or canonical(value) != raw:
        raise ValueError("runtime manifest anchor/canonical mismatch")
    required = {"schema", "sourceLockSha256", "runtimeVersion", "upstreamCommit", "coreVersion", "nodeVersion", "platform", "node", "entry", "treeSize", "files"}
    target = lock["platforms"].get(value.get("platform"))
    if set(value) != required or not target or value["schema"] != "paleo.pi-runtime.v1" or value["sourceLockSha256"] != digest(VENDOR / "source.lock.json") or value["upstreamCommit"] != lock["upstream"]["commit"] or value["nodeVersion"] != lock["nodeVersion"] or value["coreVersion"] != lock["coreVersion"] or value["runtimeVersion"] != lock["runtimeVersion"] or value["node"] != target["program"] or value["entry"] != "agent/sidecar.mjs":
        raise ValueError("incompatible runtime manifest")
    rows = value["files"]
    if not isinstance(rows, list) or not 1 <= len(rows) <= lock["limits"]["maxFiles"]:
        raise ValueError("runtime file budget")
    expected = {}; total = 0; folded = set()
    for row in rows:
        if set(row) != {"path", "size", "sha256", "executable"} or type(row["size"]) is not int or row["size"] < 1 or type(row["executable"]) is not bool or not hex_digest(row["sha256"]):
            raise ValueError("invalid runtime file declaration")
        name = relative(row["path"])
        if name == MANIFEST or name.casefold() in folded:
            raise ValueError("duplicate runtime file")
        folded.add(name.casefold()); expected[name] = row; total += row["size"]
        if total > lock["limits"]["maxTreeBytes"]:
            raise ValueError("runtime tree byte budget")
    if value["treeSize"] != total or expected.get(target["program"], {}).get("sha256") != target["programSha256"] or expected.get("agent/sidecar.mjs", {}).get("sha256") != lock["build"]["bundleSha256"]:
        raise ValueError("runtime payload incompatible with host source lock")
    actual = set(); count = 0
    for path in root.rglob("*"):
        count += 1
        if count > 2048:
            raise ValueError("runtime entry budget")
        if linked(path):
            raise ValueError("runtime links forbidden")
        if path.is_dir():
            continue
        name = path.relative_to(root).as_posix()
        if name == MANIFEST:
            continue
        row = expected.get(name)
        if not row or not path.is_file() or path.stat().st_size != row["size"] or digest(path) != row["sha256"]:
            raise ValueError("runtime file set/size/SHA mismatch")
        actual.add(name)
    if actual != set(expected):
        raise ValueError("runtime missing file")
    return value
