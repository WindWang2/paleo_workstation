# 层：测试壳
"""Bundle a verified optional Python generation for an execution-free C++ installer."""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import stat
import struct
import sys
import uuid

sys.dont_write_bytecode = True
import prepare_agent_python as runtime
import prepare_agent_python_scripts as scripts

ROOT = Path(__file__).resolve().parents[1]
MAGIC = b"PALEOPY1\n"
SCHEMA = "paleo.python-delivery.v1"
MAX_HEADER_BYTES = 4 * 1024 * 1024
MAX_ENTRIES = 16384
MAX_FILE_BYTES = 1024 * 1024 * 1024
MAX_TREE_BYTES = MAX_FILE_BYTES + MAX_HEADER_BYTES
MAX_SCRIPTS_BYTES = 4 * 1024 * 1024
BLOCK_BYTES = 64 * 1024


def canonical(value):
    return scripts.canonical(value)


def regular_open(path):
    """Reject links and compare the opened regular file with its checked identity."""
    path = Path(path)
    scripts.directory(path.parent)
    before = path.lstat()
    if scripts._linked(before) or not stat.S_ISREG(before.st_mode) or before.st_nlink != 1:
        raise ValueError("linked/nonregular source file")
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    stream = os.fdopen(os.open(path, flags), "rb")
    opened = os.fstat(stream.fileno())
    if (opened.st_dev, opened.st_ino) != (before.st_dev, before.st_ino) or opened.st_nlink != 1:
        stream.close()
        raise ValueError("source identity changed while opening")
    return stream


def _bounded_bytes(path, maximum):
    with regular_open(path) as stream:
        before = os.fstat(stream.fileno())
        if not 0 < before.st_size <= maximum:
            raise ValueError("source size exceeded")
        raw = stream.read(maximum + 1)
        after = os.fstat(stream.fileno())
    if len(raw) != before.st_size or (after.st_size, after.st_mtime_ns, after.st_ctime_ns) != (
            before.st_size, before.st_mtime_ns, before.st_ctime_ns):
        raise ValueError("source changed while reading")
    return raw


def _source_tree(root):
    """Do not follow even a directory link while enumerating the supplied tree."""
    names = set()
    count, file_count, total = 0, 0, 0
    def failed(error):
        raise error
    for directory, subdirs, files in os.walk(root, followlinks=False, onerror=failed):
        for name in subdirs + files:
            path = Path(directory) / name
            info = path.lstat()
            if scripts._linked(info) or not (stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode)):
                raise ValueError("generation contains a link or special entry")
            if stat.S_ISREG(info.st_mode) and info.st_nlink != 1:
                raise ValueError("generation contains a hard link")
            if stat.S_ISREG(info.st_mode):
                file_count += 1
                total += info.st_size
                if info.st_size > MAX_FILE_BYTES or file_count > MAX_ENTRIES or total > MAX_TREE_BYTES:
                    raise ValueError("source generation byte/file budget exceeded")
            relative = path.relative_to(root).as_posix()
            runtime.relative_path(relative)
            key = relative.casefold()
            if key in names:
                raise ValueError("generation contains a case-folded alias")
            names.add(key)
            count += 1
            if count > MAX_ENTRIES * 3:
                raise ValueError("generation directory budget exceeded")


def _compatible_runtime(value, lock, source_sha):
    for key in ("profile", "runtimeVersion", "pythonVersion", "numpyVersion"):
        if value.get(key) != lock[key]:
            raise ValueError("generation is incompatible with this host source profile")
    field = "sourceLockSha256" if value.get("schema") == runtime.SCHEMA else "sourceRuntimeLockSha256"
    if value.get(field) != source_sha:
        raise ValueError("generation source lock is incompatible")


def validate_header(value):
    expected = {"schema", "kind", "platform", "manifestSha256", "licenseStatus", "entries"}
    if not isinstance(value, dict) or set(value) != expected or value["schema"] != SCHEMA:
        raise ValueError("unsupported delivery header")
    if value["kind"] not in ("runtime", "scripts") or not scripts.digest(value["manifestSha256"]):
        raise ValueError("invalid delivery identity")
    is_runtime = value["kind"] == "runtime"
    if is_runtime:
        if value["platform"] not in ("windows-x86_64", "linux-x86_64") or value["licenseStatus"] != "runtime-licenses-included":
            raise ValueError("invalid runtime delivery profile")
    elif value["platform"] != "all" or value["licenseStatus"] not in ("notice-only", "repository-texts-included"):
        raise ValueError("invalid scripts delivery profile")
    rows = value["entries"]
    if not isinstance(rows, list) or not 1 <= len(rows) <= MAX_ENTRIES:
        raise ValueError("delivery entry count exceeded")
    names, folded, total = [], set(), 0
    for row in rows:
        if not isinstance(row, dict) or set(row) != {"path", "size", "sha256", "executable"}:
            raise ValueError("invalid delivery entry")
        name = runtime.relative_path(row["path"])
        if name.casefold() in folded or type(row["size"]) is not int or not 0 <= row["size"] <= MAX_FILE_BYTES:
            raise ValueError("duplicate entry or invalid byte count")
        if type(row["executable"]) is not bool or not scripts.digest(row["sha256"]):
            raise ValueError("invalid delivery entry identity")
        if not is_runtime and ("/" in name or row["executable"]):
            raise ValueError("scripts delivery must be flat nonexecutable data")
        folded.add(name.casefold())
        names.append(name)
        total += row["size"]
    limit = MAX_TREE_BYTES if is_runtime else MAX_SCRIPTS_BYTES
    manifest_name = runtime.MANIFEST if is_runtime else scripts.MANIFEST
    if total > limit or names != sorted(names) or manifest_name not in names:
        raise ValueError("delivery tree/order/manifest mismatch")
    manifest_row = rows[names.index(manifest_name)]
    if manifest_row["sha256"] != value["manifestSha256"] or manifest_row["executable"]:
        raise ValueError("delivery manifest identity mismatch")
    maximum = MAX_HEADER_BYTES if is_runtime else scripts.MAX_MANIFEST_BYTES
    if not 0 < manifest_row["size"] <= maximum:
        raise ValueError("delivery manifest budget exceeded")
    # Include implicit directories: Dir/a and dir/b alias on Windows even though
    # the complete file names differ. Bound the implied tree before allocation.
    paths = set(names)
    components = {}
    for name in names:
        if any(parent.as_posix() in paths for parent in Path(name).parents if parent.as_posix() != "."):
            raise ValueError("delivery file/directory collision")
        parts = name.split("/")
        for count in range(1, len(parts) + 1):
            prefix = "/".join(parts[:count])
            key = prefix.casefold()
            if key in components and components[key] != prefix:
                raise ValueError("delivery directory case-folded alias")
            components[key] = prefix
            if len(components) > MAX_ENTRIES * 3:
                raise ValueError("delivery directory budget exceeded")
    return total


def generation_header(kind, generation, anchor):
    if kind not in ("runtime", "scripts") or not scripts.digest(anchor):
        raise ValueError("explicit generation kind and external manifest SHA required")
    root = scripts.directory(generation)
    _source_tree(root)
    lock, source_sha = runtime.load_lock()
    name = runtime.MANIFEST if kind == "runtime" else scripts.MANIFEST
    raw = _bounded_bytes(root / name, MAX_HEADER_BYTES if kind == "runtime" else scripts.MAX_MANIFEST_BYTES)
    if hashlib.sha256(raw).hexdigest() != anchor:
        raise ValueError("external manifest SHA mismatch")
    manifest = scripts.strict_json(raw)
    if not isinstance(manifest, dict):
        raise ValueError("manifest must be a JSON object")
    if kind == "runtime":
        required = {"files", "interpreter", "numpyVersion", "platform", "profile", "pythonVersion", "runtimeVersion", "schema", "sourceLockSha256", "treeSize"}
        if set(manifest) != required or manifest["schema"] != runtime.SCHEMA or runtime.canonical(manifest) != raw:
            raise ValueError("unsupported/noncanonical runtime manifest")
        platform = manifest["platform"]
        if not isinstance(platform, str) or platform not in lock["platforms"] or manifest["interpreter"] != lock["platforms"][platform]["interpreter"]:
            raise ValueError("runtime platform/interpreter mismatch")
        if not isinstance(manifest["files"], list) or not 1 <= len(manifest["files"]) < MAX_ENTRIES or any(not isinstance(row, dict) for row in manifest["files"]):
            raise ValueError("invalid runtime file list")
        entries = [dict(row) for row in manifest["files"]]
        license_status = "runtime-licenses-included"
    else:
        manifest = scripts.verify_tree(root, anchor)
        unused, current, unused_sha = scripts.source_bundle()
        if manifest["scripts"] != current["scripts"]:
            raise ValueError("scripts do not match this host's fixed template hashes")
        entries = [{"path": row["path"], "size": row["size"], "sha256": row["sha256"], "executable": False}
                   for row in manifest["scripts"] + manifest["licenses"]]
        platform, license_status = "all", manifest["licenseStatus"]
    _compatible_runtime(manifest, lock, source_sha)
    entries.append({"path": name, "size": len(raw), "sha256": anchor, "executable": False})
    header = {"schema": SCHEMA, "kind": kind, "platform": platform, "manifestSha256": anchor,
              "licenseStatus": license_status, "entries": sorted(entries, key=lambda row: row["path"])}
    validate_header(header)
    if kind == "runtime":
        if type(manifest["treeSize"]) is not int or manifest["treeSize"] != sum(row["size"] for row in manifest["files"]) or manifest["treeSize"] > MAX_FILE_BYTES:
            raise ValueError("runtime tree budget mismatch")
        if not any(row["path"] == manifest["interpreter"] and row["executable"] for row in manifest["files"]):
            raise ValueError("runtime interpreter must be a declared executable")
        runtime.verify_tree(root, anchor)
    if len(canonical(header)) > MAX_HEADER_BYTES:
        raise ValueError("delivery header budget exceeded")
    return root, header


def _copy_entry(root, row, write):
    with regular_open(root / row["path"]) as source:
        before = os.fstat(source.fileno())
        if before.st_size != row["size"]:
            raise ValueError("source size changed before copying")
        remaining, digest = row["size"], hashlib.sha256()
        while remaining:
            block = source.read(min(BLOCK_BYTES, remaining))
            if not block:
                raise ValueError("source truncated while copying")
            remaining -= len(block)
            digest.update(block)
            write(block)
        after = os.fstat(source.fileno())
        if source.read(1) or digest.hexdigest() != row["sha256"] or after.st_nlink != 1 or (
                after.st_size, after.st_mtime_ns, after.st_ctime_ns) != (before.st_size, before.st_mtime_ns, before.st_ctime_ns):
            raise ValueError("source changed while copying")


def _existing_matches(path, size, expected):
    with regular_open(path) as source:
        before = os.fstat(source.fileno())
        if before.st_size != size:
            return False
        digest, remaining = hashlib.sha256(), size
        while remaining:
            block = source.read(min(BLOCK_BYTES, remaining))
            if not block:
                return False
            remaining -= len(block)
            digest.update(block)
        after = os.fstat(source.fileno())
        extra = source.read(1)
    return not extra and digest.hexdigest() == expected and (after.st_size, after.st_mtime_ns, after.st_ctime_ns) == (
        before.st_size, before.st_mtime_ns, before.st_ctime_ns)


def bundle(kind, generation, anchor, output, purpose="internal"):
    if purpose not in ("internal", "redistribute"):
        raise ValueError("unknown delivery purpose")
    if purpose == "redistribute":
        raise ValueError("external redistribution requires an independent owner-approved licence record; unavailable")
    root, header = generation_header(kind, generation, anchor)
    parent = scripts.directory(Path(output).absolute().parent, create=True)
    output = parent / Path(output).name
    if output == root or root in output.parents:
        raise ValueError("output must be outside the immutable generation")
    temporary = parent / (".python-delivery-" + uuid.uuid4().hex)
    digest, size = hashlib.sha256(), 0
    try:
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
        with os.fdopen(os.open(temporary, flags, 0o600), "wb") as target:
            def write(block):
                nonlocal size
                if target.write(block) != len(block):
                    raise OSError("delivery write incomplete")
                digest.update(block)
                size += len(block)
            raw = canonical(header)
            write(MAGIC + struct.pack("<I", len(raw)) + raw)
            for row in header["entries"]:
                _copy_entry(root, row, write)
            target.flush()
            os.fsync(target.fileno())
        # Recheck names, source identity and content before publishing any output.
        if generation_header(kind, root, anchor)[1] != header:
            raise ValueError("source generation changed while bundling")
        package_sha = digest.hexdigest()
        try:
            # Atomic no-replace publication; an existing different file is never overwritten.
            os.link(temporary, output)
        except FileExistsError:
            if not _existing_matches(output, size, package_sha):
                raise ValueError("refusing to replace a different delivery bundle") from None
        temporary.unlink()
        scripts._sync_directory(parent)
        if not _existing_matches(output, size, package_sha):
            raise ValueError("delivery output identity changed")
        return {"archive": str(output), "archiveSha256": package_sha, "archiveSize": size,
                "manifestSha256": anchor, "kind": kind, "platform": header["platform"],
                "licenseStatus": header["licenseStatus"], "purpose": "internal", "redistributionApproved": False}
    finally:
        temporary.unlink(missing_ok=True)


def inspect_archive(path, anchor):
    """Stream-validate with an independently supplied whole-file SHA; never extract."""
    if not scripts.digest(anchor):
        raise ValueError("external delivery SHA required")
    with regular_open(path) as source:
        before = os.fstat(source.fileno())
        if before.st_size > len(MAGIC) + 4 + MAX_HEADER_BYTES + MAX_TREE_BYTES:
            raise ValueError("delivery package budget exceeded")
        digest = hashlib.sha256()
        def read(size):
            raw = source.read(size)
            digest.update(raw)
            if len(raw) != size:
                raise ValueError("delivery truncated")
            return raw
        if read(len(MAGIC)) != MAGIC:
            raise ValueError("delivery magic mismatch")
        length = struct.unpack("<I", read(4))[0]
        if not 0 < length <= MAX_HEADER_BYTES:
            raise ValueError("delivery header budget exceeded")
        raw = read(length)
        header = scripts.strict_json(raw)
        total = validate_header(header)
        if canonical(header) != raw or before.st_size != len(MAGIC) + 4 + length + total:
            raise ValueError("noncanonical delivery or package length mismatch")
        for row in header["entries"]:
            remaining, entry_sha = row["size"], hashlib.sha256()
            while remaining:
                block = read(min(BLOCK_BYTES, remaining))
                remaining -= len(block)
                entry_sha.update(block)
            if entry_sha.hexdigest() != row["sha256"]:
                raise ValueError("delivery payload SHA mismatch")
        after = os.fstat(source.fileno())
        if source.read(1) or digest.hexdigest() != anchor or (after.st_size, after.st_mtime_ns, after.st_ctime_ns) != (
                before.st_size, before.st_mtime_ns, before.st_ctime_ns):
            raise ValueError("delivery SHA/source identity mismatch")
    return header


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kind", required=True, choices=("runtime", "scripts"))
    parser.add_argument("--generation", required=True, type=Path)
    parser.add_argument("--manifest-sha256", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--purpose", choices=("internal", "redistribute"), default="internal")
    args = parser.parse_args(argv)
    try:
        print(canonical(bundle(args.kind, args.generation, args.manifest_sha256, args.output, args.purpose)).decode("utf-8"), end="")
        return 0
    except (OSError, ValueError, RuntimeError, RecursionError) as error:
        print("agent-python-delivery: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
