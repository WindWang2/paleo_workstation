# 层：测试壳
"""Manually bundle or activate optional trusted templates; never execute supplied code."""
from __future__ import annotations

import argparse
import ast
import hashlib
import io
import json
import os
from pathlib import Path
import re
import stat
import sys
import uuid
import zipfile

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ("attribute_correlation", "summary", "well_quality")
LICENSE_NAMES = ("COPYING", "COPYING.md", "COPYING.txt", "LICENSE", "LICENSE.md", "LICENSE.txt")
MANIFEST = "scripts-manifest.json"
SCHEMA = "paleo.python-scripts.v1"
SELECTION_SCHEMA = "paleo.python-scripts.selection.v1"
MAX_CODE_BYTES = 256 * 1024
MAX_TEXT_BYTES = 256 * 1024
MAX_MANIFEST_BYTES = 64 * 1024
MAX_TREE_BYTES = 3 * 1024 * 1024
MAX_ARCHIVE_BYTES = MAX_TREE_BYTES + 128 * 1024
MAX_ENTRIES = len(SCRIPTS) + len(LICENSE_NAMES) + 2


def canonical(value):
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False) + "\n").encode("utf-8")


def sha256(value):
    return hashlib.sha256(value).hexdigest()


def digest(value):
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) and value != "0" * 64


def _pairs(items):
    result = {}
    for key, value in items:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


def strict_json(raw):
    def invalid(unused):
        raise ValueError("nonfinite JSON value")
    try:
        return json.loads(raw.decode("utf-8"), object_pairs_hook=_pairs, parse_constant=invalid)
    except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
        raise ValueError("invalid UTF-8/JSON document") from error


def _linked(info):
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, "st_file_attributes", 0) & 0x400)


def directory(path, create=False):
    """Check each component before use; do not silently resolve operator links."""
    path = Path(os.path.abspath(path))
    for current in reversed((path, *path.parents)):
        try:
            info = current.lstat()
        except FileNotFoundError:
            if not create:
                raise ValueError("directory missing: " + str(current)) from None
            current.mkdir(mode=0o700)
            info = current.lstat()
        if _linked(info) or not stat.S_ISDIR(info.st_mode):
            raise ValueError("directory link/reparse point or non-directory")
    return path


def regular_bytes(path, maximum):
    path = Path(path)
    directory(path.parent)
    before = path.lstat()
    if _linked(before) or not stat.S_ISREG(before.st_mode) or before.st_nlink != 1 or not 0 < before.st_size <= maximum:
        raise ValueError("linked/nonregular/oversized source: " + path.name)
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    with os.fdopen(os.open(path, flags), "rb") as stream:
        opened = os.fstat(stream.fileno())
        if (opened.st_dev, opened.st_ino) != (before.st_dev, before.st_ino):
            raise ValueError("source identity changed")
        raw = stream.read(maximum + 1)
        after = os.fstat(stream.fileno())
    if len(raw) != before.st_size or len(raw) > maximum or (after.st_size, after.st_mtime_ns) != (before.st_size, before.st_mtime_ns):
        raise ValueError("source changed while reading")
    return raw


def script_bytes(raw, name):
    if not 0 < len(raw) <= MAX_CODE_BYTES or b"\r" in raw or b"\0" in raw or not raw.endswith(b"\n"):
        raise ValueError("script must be bounded UTF-8 LF text")
    try:
        ast.parse(raw.decode("utf-8"), filename=name)
    except (UnicodeError, SyntaxError, RecursionError) as error:
        raise ValueError("invalid script text/syntax: " + name) from error
    return raw


def notice_bytes(raw):
    if not 0 < len(raw) <= MAX_TEXT_BYTES or b"\r" in raw or b"\0" in raw:
        raise ValueError("licence/notice must be bounded UTF-8 LF text")
    try:
        raw.decode("utf-8")
    except UnicodeError as error:
        raise ValueError("licence/notice is not UTF-8") from error
    return raw


def source_bundle(root=ROOT):
    root = directory(root)
    templates = directory(root / "vendor/agent-python/templates")
    if {path.name for path in templates.iterdir()} != {name + ".py" for name in SCRIPTS}:
        raise ValueError("template directory must contain exactly the three registered files")
    lock_raw = regular_bytes(root / "vendor/agent-python/runtime.lock.json", 128 * 1024)
    lock = strict_json(lock_raw)
    if not isinstance(lock, dict) or lock.get("schema") != "paleo.agent-python.supply.v1" or lock.get("profile") != "analysis-core":
        raise ValueError("unsupported source runtime lock")
    for key in ("runtimeVersion", "pythonVersion", "numpyVersion"):
        if not isinstance(lock.get(key), str) or not re.fullmatch(r"[A-Za-z0-9_.+-]{1,128}", lock[key]):
            raise ValueError("invalid runtime identity")
    payloads, scripts, licenses = {}, [], []
    for identity in SCRIPTS:
        name = identity + ".py"
        raw = script_bytes(regular_bytes(templates / name, MAX_CODE_BYTES), name)
        payloads[name] = raw
        scripts.append({"id": identity, "path": name, "source": "vendor/agent-python/templates/" + name,
                        "size": len(raw), "sha256": sha256(raw)})
    for name in ("NOTICE.md", *LICENSE_NAMES):
        path = root / name
        if name != "NOTICE.md" and not path.exists() and not path.is_symlink():
            continue
        text = regular_bytes(path, MAX_TEXT_BYTES).decode("utf-8")
        if "\0" in text:
            raise ValueError("invalid licence/notice text")
        raw = notice_bytes(text.replace("\r\n", "\n").replace("\r", "\n").encode("utf-8"))
        payloads[name] = raw
        licenses.append({"path": name, "source": name, "kind": "notice" if name == "NOTICE.md" else "license-text",
                         "size": len(raw), "sha256": sha256(raw)})
    manifest = {"schema": SCHEMA, "version": 1, "profile": "analysis-core", "sourceRuntimeLockSha256": sha256(lock_raw),
                "runtimeVersion": lock["runtimeVersion"], "pythonVersion": lock["pythonVersion"], "numpyVersion": lock["numpyVersion"],
                "scripts": scripts, "licenses": sorted(licenses, key=lambda row: row["path"]),
                "licenseStatus": "repository-texts-included" if len(licenses) > 1 else "notice-only",
                "treeSize": sum(map(len, payloads.values()))}
    raw = canonical(manifest)
    validate_manifest(raw, sha256(raw))
    payloads[MANIFEST] = raw
    return payloads, manifest, sha256(raw)


def validate_manifest(raw, anchor):
    if not digest(anchor) or len(raw) > MAX_MANIFEST_BYTES or sha256(raw) != anchor:
        raise ValueError("manifest anchor mismatch or size exceeded")
    value = strict_json(raw)
    keys = {"schema", "version", "profile", "runtimeVersion", "pythonVersion", "numpyVersion", "sourceRuntimeLockSha256",
            "scripts", "licenses", "licenseStatus", "treeSize"}
    if not isinstance(value, dict) or set(value) != keys or value["schema"] != SCHEMA or type(value["version"]) is not int or value["version"] != 1:
        raise ValueError("unsupported scripts manifest")
    if value["profile"] != "analysis-core" or not digest(value["sourceRuntimeLockSha256"]):
        raise ValueError("invalid scripts runtime source identity")
    if any(not isinstance(value[k], str) or not re.fullmatch(r"[A-Za-z0-9_.+-]{1,128}", value[k]) for k in ("runtimeVersion", "pythonVersion", "numpyVersion")):
        raise ValueError("invalid runtime version")
    scripts, licenses = value["scripts"], value["licenses"]
    if not isinstance(scripts, list) or len(scripts) != len(SCRIPTS) or not isinstance(licenses, list) or not 1 <= len(licenses) <= len(LICENSE_NAMES) + 1:
        raise ValueError("invalid script/licence list")
    names, identities, total = set(), [], 0
    for index, row in enumerate(scripts + licenses):
        if not isinstance(row, dict):
            raise ValueError("invalid manifest row")
        script = index < len(scripts)
        expected = {"id", "path", "source", "size", "sha256"} if script else {"path", "source", "kind", "size", "sha256"}
        if set(row) != expected or type(row["size"]) is not int or not 0 < row["size"] <= (MAX_CODE_BYTES if script else MAX_TEXT_BYTES) or not digest(row["sha256"]):
            raise ValueError("invalid manifest row fields or bounds")
        name = row["path"]
        if not isinstance(name, str) or name in names:
            raise ValueError("duplicate manifest path")
        if script:
            if row["id"] not in SCRIPTS or name != row["id"] + ".py" or row["source"] != "vendor/agent-python/templates/" + name:
                raise ValueError("unregistered script id/path")
            identities.append(row["id"])
        elif name not in ("NOTICE.md", *LICENSE_NAMES) or row["source"] != name or row["kind"] != ("notice" if name == "NOTICE.md" else "license-text"):
            raise ValueError("unregistered licence/notice path")
        names.add(name)
        total += row["size"]
    status = "repository-texts-included" if len(licenses) > 1 else "notice-only"
    if identities != list(SCRIPTS) or "NOTICE.md" not in names or value["licenseStatus"] != status or [row["path"] for row in licenses] != sorted(row["path"] for row in licenses):
        raise ValueError("script/licence identity mismatch")
    if type(value["treeSize"]) is not int or total != value["treeSize"] or total > MAX_TREE_BYTES or canonical(value) != raw:
        raise ValueError("noncanonical manifest or tree budget mismatch")
    return value


def archive_bytes(payloads):
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_STORED, allowZip64=False) as archive:
        for name in sorted(payloads):
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.create_system, info.create_version, info.extract_version = 3, 20, 20
            info.external_attr = (stat.S_IFREG | 0o444) << 16
            archive.writestr(info, payloads[name])
    raw = stream.getvalue()
    if len(raw) > MAX_ARCHIVE_BYTES:
        raise ValueError("archive budget exceeded")
    return raw


def read_archive(path, anchor):
    raw = regular_bytes(path, MAX_ARCHIVE_BYTES)
    payloads = {}
    with zipfile.ZipFile(io.BytesIO(raw)) as archive:
        rows = archive.infolist()
        if not 1 <= len(rows) <= MAX_ENTRIES:
            raise ValueError("archive entry budget exceeded")
        allowed = {name + ".py" for name in SCRIPTS} | {MANIFEST, "NOTICE.md", *LICENSE_NAMES}
        for row in rows:
            name = row.orig_filename
            mode = row.external_attr >> 16
            maximum = MAX_MANIFEST_BYTES if name == MANIFEST else MAX_CODE_BYTES if name.endswith(".py") else MAX_TEXT_BYTES
            if name != row.filename or name not in allowed or name in payloads:
                raise ValueError("duplicate/extra/unsafe archive path")
            if row.is_dir() or stat.S_IFMT(mode) not in (0, stat.S_IFREG) or row.external_attr & 0x410 or row.extra or row.comment or row.flag_bits or row.compress_type != zipfile.ZIP_STORED:
                raise ValueError("linked/nonregular/noncanonical archive entry")
            if not 0 < row.file_size <= maximum or row.compress_size != row.file_size:
                raise ValueError("archive file size exceeded")
            with archive.open(row) as member:
                data = member.read(maximum + 1)
            if len(data) != row.file_size:
                raise ValueError("archive file length mismatch")
            payloads[name] = data
    if MANIFEST not in payloads:
        raise ValueError("manifest missing")
    manifest = validate_manifest(payloads[MANIFEST], anchor)
    expected = {row["path"] for row in manifest["scripts"] + manifest["licenses"]} | {MANIFEST}
    if set(payloads) != expected:
        raise ValueError("archive content mismatch")
    for row in manifest["scripts"] + manifest["licenses"]:
        data = payloads[row["path"]]
        if len(data) != row["size"] or sha256(data) != row["sha256"]:
            raise ValueError("archive SHA/size mismatch")
        if row["path"].endswith(".py"):
            script_bytes(data, row["path"])
        else:
            notice_bytes(data)
    if archive_bytes(payloads) != raw:
        raise ValueError("noncanonical archive representation")
    return payloads, manifest


def _write_new(path, raw):
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    with os.fdopen(os.open(path, flags, 0o600), "wb") as output:
        for offset in range(0, len(raw), 64 * 1024):
            output.write(raw[offset:offset + 64 * 1024])
        output.flush()
        os.fsync(output.fileno())


def bundle(root, output):
    payloads, manifest, anchor = source_bundle(root)
    output = directory(Path(output).parent, create=True) / Path(output).name
    raw = archive_bytes(payloads)
    if output.exists() or output.is_symlink():
        if regular_bytes(output, MAX_ARCHIVE_BYTES) != raw:
            raise ValueError("refusing to replace a different bundle")
    else:
        temporary = output.with_name(".scripts-bundle-" + uuid.uuid4().hex)
        try:
            _write_new(temporary, raw)
            if output.exists() or output.is_symlink():
                raise ValueError("bundle destination appeared")
            os.replace(temporary, output)
        finally:
            temporary.unlink(missing_ok=True)
    return {"archive": str(output), "archiveSha256": sha256(raw), "manifestSha256": anchor,
            "sourceRuntimeLockSha256": manifest["sourceRuntimeLockSha256"], "scripts": manifest["scripts"]}


def verify_tree(root, anchor, sealed=True):
    root = directory(root)
    manifest = validate_manifest(regular_bytes(root / MANIFEST, MAX_MANIFEST_BYTES), anchor)
    expected = {row["path"] for row in manifest["scripts"] + manifest["licenses"]} | {MANIFEST}
    if {path.name for path in root.iterdir()} != expected:
        raise ValueError("generation has missing/extra files")
    for row in manifest["scripts"] + manifest["licenses"]:
        path = root / row["path"]
        data = regular_bytes(path, MAX_CODE_BYTES if "id" in row else MAX_TEXT_BYTES)
        if len(data) != row["size"] or sha256(data) != row["sha256"]:
            raise ValueError("generation SHA/size mismatch")
        if "id" in row:
            script_bytes(data, row["path"])
        else:
            notice_bytes(data)
    if sealed:
        if os.name != "nt" and root.lstat().st_mode & 0o222:
            raise ValueError("generation directory is writable")
        for path in (root / name for name in expected):
            info = path.lstat()
            if info.st_mode & 0o222:
                raise ValueError("generation file is writable")
    return manifest


class StoreLock:
    def __init__(self, store):
        self.path = Path(store) / ".scripts.lock"
        self.file = None

    def __enter__(self):
        flags = os.O_RDWR | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
        try:
            fd = os.open(self.path, flags | os.O_CREAT | os.O_EXCL, 0o600)
        except FileExistsError:
            before = self.path.lstat()
            if _linked(before) or not stat.S_ISREG(before.st_mode) or before.st_nlink != 1 or before.st_size not in (0, 1):
                raise ValueError("invalid scripts store lock")
            fd = os.open(self.path, flags)
            after = os.fstat(fd)
            if (before.st_dev, before.st_ino) != (after.st_dev, after.st_ino):
                os.close(fd)
                raise ValueError("store lock identity changed")
        self.file = os.fdopen(fd, "r+b", buffering=0)
        try:
            if os.fstat(fd).st_size == 0:
                self.file.write(b"0")
            self.file.seek(0)
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BaseException:
            self.file.close()
            raise
        return self

    def __exit__(self, *unused):
        self.file.close()


def remove_stage(path, store):
    path, store = Path(path), directory(store)
    if path.parent != store or not re.fullmatch(r"\.scripts-stage-[0-9a-f]{32}", path.name):
        raise ValueError("cleanup target outside scripts store")
    if not path.exists() and not path.is_symlink():
        return
    directory(path)
    path.chmod(0o700)
    for child in path.iterdir():
        info = child.lstat()
        if _linked(info) or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            raise ValueError("unsafe staging cleanup entry")
        child.chmod(0o600)
        child.unlink()
    path.rmdir()


def _sync_directory(path):
    if os.name != "nt":
        fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | getattr(os, "O_NOFOLLOW", 0))
        try:
            os.fsync(fd)
        finally:
            os.close(fd)


def _selection(store):
    path = store / "current.json"
    if not path.exists() and not path.is_symlink():
        return None
    raw = regular_bytes(path, 1024)
    value = strict_json(raw)
    if not isinstance(value, dict) or set(value) != {"schema", "version", "generation", "manifestSha256"} or value["schema"] != SELECTION_SCHEMA or type(value["version"]) is not int or value["version"] != 1:
        raise ValueError("invalid scripts selection")
    anchor = value["manifestSha256"]
    if not digest(anchor) or value["generation"] != "generations/" + anchor or canonical(value) != raw:
        raise ValueError("invalid selection generation")
    verify_tree(store / value["generation"], anchor)
    return value


def _store_entries(store):
    for entry in store.iterdir():
        if _linked(entry.lstat()):
            raise ValueError("scripts store contains a link/reparse point")
        if entry.name not in {"generations", "current.json", ".scripts.lock"} and not re.fullmatch(r"\.scripts-(stage|current)-[0-9a-f]{32}", entry.name):
            raise ValueError("not a dedicated scripts store")


def prepare(archive, anchor, store):
    payloads, manifest = read_archive(archive, anchor)
    store = directory(store, create=True)
    _store_entries(store)
    with StoreLock(store):
        _store_entries(store)
        _selection(store)
        generations = directory(store / "generations", create=True)
        for entry in generations.iterdir():
            info = entry.lstat()
            staged = re.fullmatch(r"\.scripts-stage-[0-9a-f]{32}", entry.name)
            if not (digest(entry.name) or staged) or _linked(info) or not stat.S_ISDIR(info.st_mode):
                raise ValueError("invalid generation directory")
        generation = generations / anchor
        # A sealed directory cannot be moved across parents on all POSIX systems.
        # Same-parent rename publishes it without an intermediate writable generation.
        stage = generations / (".scripts-stage-" + uuid.uuid4().hex)
        stage.mkdir(mode=0o700)
        try:
            for name, raw in payloads.items():
                _write_new(stage / name, raw)
            verify_tree(stage, anchor, sealed=False)
            if generation.exists() or generation.is_symlink():
                verify_tree(generation, anchor)
            else:
                for child in stage.iterdir():
                    child.chmod(0o444)
                if os.name != "nt":
                    stage.chmod(0o555)
                os.replace(stage, generation)
            verify_tree(generation, anchor)
            for path in (generation, generations, store):
                _sync_directory(path)
            selection = {"schema": SELECTION_SCHEMA, "version": 1, "generation": "generations/" + anchor, "manifestSha256": anchor}
            temporary = store / (".scripts-current-" + uuid.uuid4().hex)
            try:
                _write_new(temporary, canonical(selection))
                os.replace(temporary, store / "current.json")
            finally:
                temporary.unlink(missing_ok=True)
            return {**selection, "directory": str(generation), "sourceRuntimeLockSha256": manifest["sourceRuntimeLockSha256"]}
        finally:
            remove_stage(stage, generations)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--bundle", type=Path, help="create a deterministic offline ZIP from the three repository templates")
    mode.add_argument("--archive", type=Path, help="validate an offline bundle and activate a complete generation")
    mode.add_argument("--selftest", action="store_true")
    parser.add_argument("--source-root", type=Path)
    parser.add_argument("--manifest-sha256")
    parser.add_argument("--store", type=Path)
    args = parser.parse_args(argv)
    if args.selftest:
        import check_agent_python_scripts
        return check_agent_python_scripts.selftest()
    if args.bundle and (args.store or args.manifest_sha256) or args.archive and (not args.store or not args.manifest_sha256 or args.source_root):
        parser.error("bundle uses only --source-root; archive requires --store and an independently supplied --manifest-sha256")
    try:
        result = bundle(args.source_root or ROOT, args.bundle) if args.bundle else prepare(args.archive, args.manifest_sha256, args.store)
        print(json.dumps(result, sort_keys=True, ensure_ascii=False))
        return 0
    except (OSError, ValueError, RuntimeError, UnicodeError, zipfile.BadZipFile, KeyError, TypeError) as error:
        print("agent-python-scripts: " + str(error), file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("agent-python-scripts: interrupted before completion", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
