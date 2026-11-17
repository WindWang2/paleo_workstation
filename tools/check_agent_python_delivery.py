# 层：测试壳
"""Check optional Python delivery sources, streaming bundles and offline selftests."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
import bundle_agent_python as delivery


def check(archive=None, anchor=None):
    if archive is not None:
        value = delivery.inspect_archive(archive, anchor)
        return {key: value[key] for key in ("schema", "kind", "platform", "manifestSha256", "licenseStatus")}
    lock, source_sha = delivery.runtime.load_lock()
    payloads, manifest, scripts_sha = delivery.scripts.source_bundle()
    delivery._compatible_runtime(manifest, lock, source_sha)
    if delivery.MAGIC != b"PALEOPY1\n" or delivery.MAX_ENTRIES != 16384:
        raise ValueError("delivery protocol drift")
    return {"schema": delivery.SCHEMA, "sourceRuntimeLockSha256": source_sha,
            "scriptsManifestSha256": scripts_sha, "licenseStatus": manifest["licenseStatus"],
            "redistributionApproved": False}


class DeliveryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="paleo-python-delivery-test-")
        self.root = Path(self.temporary.name)
        self.generation = self.root / "runtime"
        self.generation.mkdir()
        self.lock, self.source_sha = delivery.runtime.load_lock()
        self.payloads = {"python.exe": b"fixture", "LICENSE.txt": b"Fixture notice, not a licence grant.\n",
                         "numpy-2.5.4.dist-info/licenses/LICENSE.txt": b"Fixture NumPy notice.\n"}
        for name, raw in self.payloads.items():
            path = self.generation / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(raw)
            path.chmod(0o555 if name.endswith(".exe") else 0o444)
        self.manifest = {key: self.lock[key] for key in ("profile", "runtimeVersion", "pythonVersion", "numpyVersion")}
        self.manifest.update(schema=delivery.runtime.SCHEMA, platform="windows-x86_64", interpreter="python.exe",
                             sourceLockSha256=self.source_sha, treeSize=sum(map(len, self.payloads.values())),
                             files=[{"path": name, "size": len(raw), "sha256": hashlib.sha256(raw).hexdigest(),
                                     "executable": name.endswith(".exe")} for name, raw in sorted(self.payloads.items())])
        self._save_manifest()
        self.output = self.root / "runtime.paleo-python"

    def tearDown(self):
        # Only this test's owned temporary tree may be made writable. Restore
        # parents before unlinking sealed generations; never follow a link.
        if self.root != Path(self.temporary.name) or delivery.scripts._linked(self.root.lstat()):
            raise AssertionError("cleanup escaped the owned test directory")
        for parent, directories, files in os.walk(self.root, topdown=True, followlinks=False):
            parent = Path(parent)
            if not parent.is_relative_to(self.root):
                raise AssertionError("cleanup escaped the owned test directory")
            parent.chmod(0o700)
            directories[:] = [name for name in directories
                              if not delivery.scripts._linked((parent / name).lstat())]
            if os.name == "nt":
                for name in files:
                    path = parent / name
                    info = path.lstat()
                    if not delivery.scripts._linked(info) and stat.S_ISREG(info.st_mode) and info.st_nlink == 1:
                        path.chmod(0o600)
        shutil.rmtree(self.root)
        self.temporary.cleanup()

    def _save_manifest(self):
        raw = delivery.runtime.canonical(self.manifest)
        (self.generation / delivery.runtime.MANIFEST).write_bytes(raw)
        self.anchor = hashlib.sha256(raw).hexdigest()

    def _bundle(self, **kwargs):
        return delivery.bundle("runtime", self.generation, self.anchor, self.output, **kwargs)

    def _scripts(self, license_text=False):
        source = self.root / "source"
        templates = source / "vendor/agent-python/templates"
        templates.mkdir(parents=True)
        for identity in delivery.scripts.SCRIPTS:
            name = identity + ".py"
            (templates / name).write_bytes((delivery.scripts.ROOT / "vendor/agent-python/templates" / name).read_bytes())
        (source / "vendor/agent-python/runtime.lock.json").write_bytes(
            (delivery.scripts.ROOT / "vendor/agent-python/runtime.lock.json").read_bytes())
        (source / "NOTICE.md").write_bytes(b"Fixture notice-only; no external grant.\n")
        if license_text:
            (source / "LICENSE").write_bytes(b"Fixture text, no redistribution approval asserted.\n")
        archive = self.root / "source-scripts.zip"
        built = delivery.scripts.bundle(source, archive)
        prepared = delivery.scripts.prepare(archive, built["manifestSha256"], self.root / "scripts-store")
        return Path(prepared["directory"]), built["manifestSha256"]

    def _inspect(self, raw):
        path = self.root / "candidate.paleo-python"
        path.write_bytes(raw)
        return delivery.inspect_archive(path, hashlib.sha256(raw).hexdigest())

    def test_runtime_deterministic_streaming_no_execution_or_network(self):
        with mock.patch.object(subprocess, "Popen", side_effect=AssertionError("must not execute")), \
                mock.patch.object(delivery.runtime.urllib.request, "urlopen", side_effect=AssertionError("must not fetch")):
            first = self._bundle()
            previous = self.output.read_bytes()
            second = self._bundle()
        self.assertEqual(first, second)
        self.assertEqual(previous, self.output.read_bytes())
        self.assertFalse(first["redistributionApproved"])
        self.assertEqual(delivery.inspect_archive(self.output, first["archiveSha256"])["manifestSha256"], self.anchor)

    def test_scripts_notice_and_fixed_source_hashes_are_preserved(self):
        root, anchor = self._scripts()
        result = delivery.bundle("scripts", root, anchor, self.output)
        header = delivery.inspect_archive(self.output, result["archiveSha256"])
        self.assertEqual(header["platform"], "all")
        self.assertEqual(header["licenseStatus"], "notice-only")
        self.assertTrue(all(not row["executable"] for row in header["entries"]))
        self.assertIn(b"Fixture notice-only; no external grant.", self.output.read_bytes())

    def test_redistribution_is_denied_for_runtime_and_both_scripts_statuses(self):
        with self.assertRaisesRegex(ValueError, "redistribution"):
            self._bundle(purpose="redistribute")
        for licensed in (False, True):
            if licensed:
                shutil.rmtree(self.root / "source")
                (self.root / "source-scripts.zip").unlink()
            root, anchor = self._scripts(licensed)
            with self.assertRaisesRegex(ValueError, "redistribution"):
                delivery.bundle("scripts", root, anchor, self.output, "redistribute")
        self.assertFalse(self.output.exists())

    def test_wrong_anchor_and_corrupt_source_do_not_create_output(self):
        with self.assertRaises(ValueError):
            delivery.bundle("runtime", self.generation, "1" * 64, self.output)
        path = self.generation / "python.exe"
        path.chmod(0o600)
        path.write_bytes(b"changed")
        with self.assertRaises(ValueError):
            self._bundle()
        self.assertFalse(self.output.exists())

    def test_existing_different_output_is_not_replaced(self):
        self.output.write_bytes(b"existing delivery")
        with self.assertRaisesRegex(ValueError, "replace"):
            self._bundle()
        self.assertEqual(self.output.read_bytes(), b"existing delivery")
        self.assertFalse(list(self.root.glob(".python-delivery-*")))

    def test_extra_file_and_case_alias_are_rejected(self):
        (self.generation / "extra").write_bytes(b"extra")
        with self.assertRaises(ValueError):
            self._bundle()
        (self.generation / "extra").unlink()
        self.manifest["files"].append({**self.manifest["files"][0], "path": "license.TXT"})
        self._save_manifest()
        with self.assertRaises(ValueError):
            self._bundle()

    def test_hardlinked_and_reparse_sources_are_rejected(self):
        os.link(self.generation / "python.exe", self.root / "hardlink")
        with self.assertRaisesRegex(ValueError, "hard link"):
            self._bundle()
        (self.root / "hardlink").chmod(0o600)
        (self.root / "hardlink").unlink()
        (self.generation / "python.exe").chmod(0o555)
        with mock.patch.object(delivery.scripts, "_linked", return_value=True):
            with self.assertRaises(ValueError):
                self._bundle()

    def test_incompatible_source_lock_version_and_interpreter_are_rejected(self):
        for key, value in (("sourceLockSha256", "1" * 64), ("pythonVersion", "9.9.9"), ("interpreter", "elsewhere.exe")):
            old = self.manifest[key]
            self.manifest[key] = value
            self._save_manifest()
            with self.assertRaises(ValueError):
                self._bundle()
            self.manifest[key] = old

    def test_output_inside_generation_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "outside"):
            delivery.bundle("runtime", self.generation, self.anchor, self.generation / "output")
        self.assertFalse((self.generation / "output").exists())

    def test_modified_template_requires_matching_host_source_review(self):
        self._scripts()
        source = self.root / "source"
        path = source / "vendor/agent-python/templates/summary.py"
        path.write_bytes(path.read_bytes() + b"# Different fixed template identity\n")
        bundle = delivery.scripts.bundle(source, self.root / "next-scripts.zip")
        prepared = delivery.scripts.prepare(self.root / "next-scripts.zip", bundle["manifestSha256"], self.root / "scripts-store")
        with self.assertRaisesRegex(ValueError, "fixed template hashes"):
            delivery.bundle("scripts", Path(prepared["directory"]), bundle["manifestSha256"], self.output)

    def test_same_size_change_between_validation_and_copy_is_rejected(self):
        original = delivery._copy_entry
        changed = False
        def copying(root, row, write):
            nonlocal changed
            if row["path"] == "python.exe" and not changed:
                path = root / row["path"]
                path.chmod(0o600)
                path.write_bytes(b"changed")
                changed = True
            original(root, row, write)
        with mock.patch.object(delivery, "_copy_entry", side_effect=copying):
            with self.assertRaisesRegex(ValueError, "changed"):
                self._bundle()
        self.assertFalse(self.output.exists())
        self.assertFalse(list(self.root.glob(".python-delivery-*")))

    def test_copy_reads_and_writes_only_bounded_chunks(self):
        raw = b"fixture bytes" * 12000
        path = self.generation / "large-fixture"
        path.write_bytes(raw)
        row = {"path": path.name, "size": len(raw), "sha256": hashlib.sha256(raw).hexdigest(), "executable": False}
        opened = delivery.regular_open
        reads, writes = [], []
        class BoundedFile:
            def __init__(self, source):
                self.source = source
            def __enter__(self):
                return self
            def __exit__(self, *unused):
                self.source.close()
            def fileno(self):
                return self.source.fileno()
            def read(self, count):
                if not 0 <= count <= delivery.BLOCK_BYTES:
                    raise AssertionError("unbounded payload read")
                reads.append(count)
                return self.source.read(count)
        with mock.patch.object(delivery, "regular_open", side_effect=lambda name: BoundedFile(opened(name))):
            delivery._copy_entry(self.generation, row, lambda data: writes.append(len(data)))
        self.assertEqual(sum(writes), len(raw))
        self.assertLessEqual(max(reads), delivery.BLOCK_BYTES)
        self.assertLessEqual(max(writes), delivery.BLOCK_BYTES)

    def test_interrupted_copy_keeps_previous_output_and_cleans_own_temporary(self):
        self.output.write_bytes(b"prior package")
        with mock.patch.object(delivery, "_copy_entry", side_effect=OSError("simulated short/full disk")):
            with self.assertRaises(OSError):
                self._bundle()
        self.assertEqual(self.output.read_bytes(), b"prior package")
        self.assertFalse(list(self.root.glob(".python-delivery-*")))

    def test_payload_corruption_truncation_and_trailing_data_are_rejected(self):
        self._bundle()
        raw = self.output.read_bytes()
        for damaged in (raw[:-1], raw + b"extra", raw[:-1] + bytes([raw[-1] ^ 1])):
            with self.assertRaises(ValueError):
                self._inspect(damaged)
        with self.assertRaises(ValueError):
            delivery.inspect_archive(self.output, "1" * 64)

    def test_header_budgets_and_duplicate_keys_are_rejected_before_payload(self):
        for length in (0, delivery.MAX_HEADER_BYTES + 1, 0xffffffff):
            with self.assertRaises(ValueError):
                self._inspect(delivery.MAGIC + struct.pack("<I", length))
        raw = b'{"schema":"a","schema":"b"}\n'
        with self.assertRaises(ValueError):
            self._inspect(delivery.MAGIC + struct.pack("<I", len(raw)) + raw)

    def test_entry_path_alias_types_and_total_budgets_are_rejected(self):
        header = delivery.generation_header("runtime", self.generation, self.anchor)[1]
        for name in ("../escape", "/absolute", "C:drive", "a\\b", "CON", "a/../b", "a. ", "a\x01b"):
            changed = json.loads(json.dumps(header))
            changed["entries"][0]["path"] = name
            with self.assertRaises(ValueError):
                delivery.validate_header(changed)
        for key, value in (("size", True), ("size", -1), ("size", delivery.MAX_FILE_BYTES + 1), ("executable", 1)):
            changed = json.loads(json.dumps(header))
            changed["entries"][0][key] = value
            with self.assertRaises(ValueError):
                delivery.validate_header(changed)
        changed = json.loads(json.dumps(header))
        changed["entries"][0]["size"] = delivery.MAX_FILE_BYTES
        changed["entries"][1]["size"] = delivery.MAX_FILE_BYTES
        with self.assertRaises(ValueError):
            delivery.validate_header(changed)

    def test_directory_file_collision_and_manifest_mismatch_are_rejected(self):
        header = delivery.generation_header("runtime", self.generation, self.anchor)[1]
        header["entries"].append({"path": "python.exe/child", "size": 0, "sha256": hashlib.sha256(b"").hexdigest(), "executable": False})
        header["entries"].sort(key=lambda row: row["path"])
        with self.assertRaisesRegex(ValueError, "collision"):
            delivery.validate_header(header)
        header = delivery.generation_header("runtime", self.generation, self.anchor)[1]
        header["manifestSha256"] = "1" * 64
        with self.assertRaises(ValueError):
            delivery.validate_header(header)

    def test_directory_case_alias_and_noncanonical_header_are_rejected(self):
        header = delivery.generation_header("runtime", self.generation, self.anchor)[1]
        for path in ("Dir/a", "dir/b"):
            header["entries"].append({"path": path, "size": 0, "sha256": hashlib.sha256(b"").hexdigest(), "executable": False})
        header["entries"].sort(key=lambda row: row["path"])
        with self.assertRaisesRegex(ValueError, "case-folded"):
            delivery.validate_header(header)
        self._bundle()
        raw = self.output.read_bytes()
        length = struct.unpack("<I", raw[len(delivery.MAGIC):len(delivery.MAGIC) + 4])[0]
        start = len(delivery.MAGIC) + 4
        spaced = json.dumps(json.loads(raw[start:start + length]), indent=1).encode("utf-8")
        with self.assertRaises(ValueError):
            self._inspect(delivery.MAGIC + struct.pack("<I", len(spaced)) + spaced + raw[start + length:])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--sha256")
    args = parser.parse_args(argv)
    if args.selftest:
        if args.archive or args.sha256:
            parser.error("--selftest does not accept an archive")
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(DeliveryTests)
        return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
    if bool(args.archive) != bool(args.sha256):
        parser.error("--archive and external --sha256 are required together")
    try:
        print(delivery.canonical(check(args.archive, args.sha256)).decode("utf-8"), end="")
        return 0
    except (OSError, ValueError, RuntimeError, RecursionError) as error:
        print("agent-python-delivery: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
