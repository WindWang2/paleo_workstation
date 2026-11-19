#!/usr/bin/env python3
"""Prepare and verify a repository-pinned local OSGeo4W installation source."""

import argparse
import hashlib
import json
import re
import shutil
import sys
import tempfile
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from pin_osgeo4w import installed_db_map, parse_setup_ini, resolve_closure

REPO = Path(__file__).resolve().parents[1]
LOCK = REPO / "vendor/osgeo4w-offline.lock.json"
PIN = REPO / "vendor/manifest.json"
SHA256 = re.compile(r"[a-f0-9]{64}\Z")
ARCHIVE = re.compile(r"x86_64/release/(?:[A-Za-z0-9_.+-]+/)+[A-Za-z0-9_.+-]+\.tar\.(?:bz2|xz|gz|zst)\Z")
SETUP = "osgeo4w-setup.exe"
INDEX = "x86_64/setup.ini"
CONTENT = "archives"


def digest(path, algorithm="sha256"):
    result = hashlib.new(algorithm)
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            result.update(chunk)
    return result.hexdigest()


def validate_file(path, row):
    if not path.is_file():
        raise ValueError(f"missing offline archive: {path}")
    if path.stat().st_size != row["size"] or digest(path) != row["sha256"]:
        raise ValueError(f"size or SHA-256 mismatch: {path}")


def entry(relative, path):
    return {"path": relative, "size": path.stat().st_size, "sha256": digest(path)}


def manifest():
    return json.loads(PIN.read_text(encoding="utf-8"))["deps"]["osgeo4w"]


def archive_path(path):
    if not ARCHIVE.fullmatch(path) or any(x in (".", "..") for x in path.split("/")):
        raise ValueError(f"invalid OSGeo4W archive path: {path!r}")
    return path


def locked_index(text, pin):
    parsed = parse_setup_ini(text)
    closure = resolve_closure(parsed, pin["packages"])
    if closure != pin["closure"] or {name: closure[name] for name in pin["packages"]} != pin["packages"]:
        raise ValueError("OSGeo4W index versions do not match vendor/manifest.json")
    selected = []
    for name, version in sorted(closure.items()):
        fields = parsed[name]["install"].split()
        if len(fields) != 3 or not re.fullmatch(r"[0-9a-f]{32,128}", fields[2]):
            raise ValueError(f"invalid OSGeo4W install record: {name}")
        path, size, upstream_digest = fields
        selected.append((name, version, archive_path(path), int(size), upstream_digest))
    return selected


def build_lock(cache, installer, filtered_index, pin):
    """Maintainer-only generation using a reviewed OSGeo4W index and cache."""
    original = (cache / INDEX).read_text(encoding="utf-8")
    selected = locked_index(original, pin)
    blocks = re.split(r"(?m)(?=^@ )", original)
    selected_names = set(pin["closure"])
    filtered = blocks[0] + "".join(re.split(r"(?m)^\[", block, maxsplit=1)[0].rstrip() + "\n\n"
                                   for block in blocks[1:]
                                   if block.splitlines()[0][2:] in selected_names)
    filtered = filtered.rstrip() + "\n"
    if locked_index(filtered, pin) != selected:
        raise ValueError("filtered OSGeo4W index changed the locked closure")
    with filtered_index.open("x", encoding="utf-8", newline="\n") as target:
        target.write(filtered)
    result = {"schema": 1, "site": pin["site"], "setup": entry(SETUP, installer),
              "index": entry(INDEX, filtered_index), "packages": {}}
    if result["setup"]["sha256"] != pin["installer_sha256"]:
        raise ValueError("installer SHA-256 differs from vendor/manifest.json")
    for name, version, path, size, upstream_digest in selected:
        file = cache / path
        legacy = {32: "md5", 64: "sha256", 128: "sha512"}.get(len(upstream_digest))
        if not legacy or file.stat().st_size != size or digest(file, legacy) != upstream_digest:
            raise ValueError(f"cache does not match upstream setup index: {path}")
        result["packages"][name] = dict(entry(path, file), version=version)
    return result


def verify_lock(lock, pin, index):
    if lock["schema"] != 1 or lock["site"] != pin["site"] or not lock["site"].startswith("https://"):
        raise ValueError("wrong OSGeo4W lock schema or site")
    if lock["setup"]["path"] != SETUP or lock["setup"]["sha256"] != pin["installer_sha256"]:
        raise ValueError("wrong installer pin")
    if lock["index"]["path"] != INDEX:
        raise ValueError("wrong OSGeo4W index path")
    validate_file(index, lock["index"])
    index_text = index.read_text(encoding="utf-8")
    records = re.findall(r"(?m)^@ (.+)$", index_text)
    if sorted(records) != sorted(pin["closure"]) or re.search(r"(?m)^\[", index_text):
        raise ValueError("offline index must contain exactly one current record per locked package")
    selected = locked_index(index_text, pin)
    expected = {name: (version, path, size) for name, version, path, size, _ in selected}
    actual = {name: (row["version"], archive_path(row["path"]), row["size"])
              for name, row in lock["packages"].items()}
    if actual != expected or len({row[1] for row in actual.values()}) != len(actual):
        raise ValueError("OSGeo4W package lock/index mismatch")
    for row in [lock["setup"], *lock["packages"].values()]:
        if not isinstance(row["size"], int) or row["size"] <= 0 or not SHA256.fullmatch(row["sha256"]):
            raise ValueError(f"invalid integrity metadata: {row['path']}")


def native_profile(lock, pin, index, profile_index):
    # Derive the small mirror from the verified full snapshot, sharing every
    # archive pin. Qt SQL and compression use the same family as the desktop.
    verify_lock(lock, pin, index)
    text = index.read_text(encoding="utf-8")
    parsed = parse_setup_ini(text)
    roots = pin["native_packages"]
    expected = resolve_closure(parsed, roots)
    if "qgis" in expected or "gdal" in expected:
        raise ValueError("native OSGeo4W packages unexpectedly require GIS")
    profile_pin = dict(pin, packages={name: expected[name] for name in roots}, closure=expected)
    blocks = re.split(r"(?m)(?=^@ )", text)
    selected = blocks[0] + "".join(block for block in blocks[1:]
                                    if block.splitlines()[0][2:] in expected)
    profile_index.write_text(selected.rstrip() + "\n", encoding="utf-8", newline="\n")
    profile_lock = dict(lock, index=entry(INDEX, profile_index),
                        packages={name: lock["packages"][name] for name in expected})
    return profile_lock, profile_pin


def download(url, path, row):
    with tempfile.TemporaryDirectory(prefix=".osgeo4w-download-", dir=path.parent) as tmp:
        scratch = Path(tmp) / "download"
        with urllib.request.urlopen(url, timeout=60) as source, scratch.open("xb") as output:
            remaining = row["size"]
            while data := source.read(min(1024 * 1024, remaining + 1)):
                remaining -= len(data)
                if remaining < 0:
                    raise ValueError(f"download exceeds pin: {row['path']}")
                output.write(data)
        if remaining:
            raise urllib.error.ContentTooShortError(
                f"download ended {remaining} bytes early: {row['path']}", None)
        validate_file(scratch, row)
        scratch.replace(path)


def fetch(url, path, row):
    path.parent.mkdir(parents=True, exist_ok=True)
    for attempt in range(1, 4):
        try:
            download(url, path, row)
            return
        except urllib.error.HTTPError as error:
            if error.code not in (408, 429, 500, 502, 503, 504) or attempt == 3:
                raise
        except (urllib.error.URLError, TimeoutError, ConnectionError):
            if attempt == 3:
                raise
        print(f"retry {row['path']} after attempt {attempt}", flush=True)
        time.sleep(attempt)


def materialize(lock, pin, index, root, offline):
    # setup scans the whole local source for indexes. Give it an isolated,
    # freshly assembled mirror containing only the locked package records.
    verify_lock(lock, pin, index)
    root.mkdir(parents=True, exist_ok=True)
    setup = root / SETUP
    mirror = root / CONTENT
    mirror.mkdir(exist_ok=True)
    items = [(setup, lock["setup"], pin["installer_url"])]
    items += [(mirror / row["path"], row, lock["site"] + "/" + row["path"])
              for row in lock["packages"].values()]
    permitted = {mirror / INDEX, *(path for path, _, _ in items[1:])}
    unexpected = [p for p in mirror.rglob("*") if p.is_file() and p not in permitted]
    if unexpected:
        raise ValueError(f"unexpected files under offline mirror: {unexpected[0]}")
    missing = []
    for path, row, url in items:
        if path.is_file() or offline:
            validate_file(path, row)
        else:
            missing.append((url, path, row))
    if missing:
        with ThreadPoolExecutor(max_workers=4) as pool:
            pending = [pool.submit(fetch, url, path, row) for url, path, row in missing]
            for download_task in pending:
                download_task.result()
    dest = mirror / INDEX
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists():
        validate_file(dest, lock["index"])
    else:
        shutil.copyfile(index, dest)
        validate_file(dest, lock["index"])
    print(f"verified {len(lock['packages'])} locked OSGeo4W archives", flush=True)


def check_installed(database, expected):
    actual = installed_db_map(database.read_text(encoding="utf-8"))
    differences = sorted(set(actual) ^ set(expected) |
                         {p for p in actual.keys() & expected.keys() if actual[p] != expected[p]})
    if differences:
        raise ValueError(f"installed OSGeo4W differs from pin: {differences}")
    print(f"installed OSGeo4W matches all {len(expected)} package versions")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    generate = commands.add_parser("generate")
    for option in ("cache", "installer", "index", "lock"):
        generate.add_argument("--" + option, required=True, type=Path)
    prepare = commands.add_parser("prepare")
    prepare.add_argument("--cache", required=True, type=Path)
    prepare.add_argument("--offline", action="store_true")
    prepare.add_argument("--native", action="store_true")
    check = commands.add_parser("check-installed")
    check.add_argument("database", type=Path)
    check.add_argument("--native", action="store_true")
    args = parser.parse_args()
    pin = manifest()
    try:
        if args.action == "generate":
            rows = build_lock(args.cache, args.installer, args.index, pin)
            with args.lock.open("x", encoding="utf-8", newline="\n") as out:
                out.write(json.dumps(rows, indent=2, sort_keys=True) + "\n")
        elif args.action == "prepare":
            lock = json.loads(LOCK.read_text(encoding="utf-8"))
            index = REPO / "vendor/osgeo4w-setup.ini"
            if args.native:
                with tempfile.TemporaryDirectory(prefix="paleo-osgeo4w-native-") as directory:
                    profile_index = Path(directory) / "setup.ini"
                    profile_lock, profile_pin = native_profile(lock, pin, index, profile_index)
                    materialize(profile_lock, profile_pin, profile_index, args.cache, args.offline)
            else:
                materialize(lock, pin, index, args.cache, args.offline)
        else:
            expected = pin["closure"]
            if args.native:
                lock = json.loads(LOCK.read_text(encoding="utf-8"))
                index = REPO / "vendor/osgeo4w-setup.ini"
                verify_lock(lock, pin, index)
                expected = resolve_closure(parse_setup_ini(index.read_text(encoding="utf-8")),
                                           pin["native_packages"])
            check_installed(args.database, expected)
    except (OSError, KeyError, TypeError, ValueError) as error:
        parser.exit(1, f"offline OSGeo4W: {error}\n")


if __name__ == "__main__":
    main()
