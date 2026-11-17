#!/usr/bin/env python3
# 层：测试壳
"""Offline, nonexecuting Pi supplier regression fixtures; all writes are temporary."""
from __future__ import annotations
import copy
import hashlib
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch
from pi_supply_common import VENDOR, MANIFEST, archive_member, canonical, check_archive, digest, load_lock, read_json, relative, verify_runtime
import prepare_pi_agent as supplier


def run(source_lock):
    class SupplyTests(unittest.TestCase):
        def setUp(self):
            self.temporary = tempfile.TemporaryDirectory(); self.addCleanup(self.temporary.cleanup)
            self.base = Path(self.temporary.name); self.lock = copy.deepcopy(source_lock)
            self.lock["platforms"]["linux-x86_64"]["programSha256"] = hashlib.sha256(b"node").hexdigest()
            self.lock["build"]["bundleSha256"] = hashlib.sha256(b"fixed bundle").hexdigest()

        def fixture(self, root):
            root.mkdir(parents=True, exist_ok=True); (root / "bin").mkdir(); (root / "agent").mkdir()
            (root / "bin/node").write_bytes(b"node"); (root / "agent/sidecar.mjs").write_bytes(b"fixed bundle")
            rows = [{"path": p.relative_to(root).as_posix(), "size": p.stat().st_size, "sha256": digest(p), "executable": p.name == "node"}
                    for p in sorted(root.rglob("*")) if p.is_file()]
            value = {"schema": "paleo.pi-runtime.v1", "sourceLockSha256": digest(VENDOR / "source.lock.json"),
                     "runtimeVersion": self.lock["runtimeVersion"], "upstreamCommit": self.lock["upstream"]["commit"],
                     "coreVersion": self.lock["coreVersion"], "nodeVersion": self.lock["nodeVersion"], "platform": "linux-x86_64",
                     "node": "bin/node", "entry": "agent/sidecar.mjs", "treeSize": sum(r["size"] for r in rows), "files": rows}
            return self.manifest(root, value), value

        def manifest(self, root, value):
            raw = canonical(value); (root / MANIFEST).write_bytes(raw); return hashlib.sha256(raw).hexdigest()

        def test_valid_identity_and_full_hash(self):
            root = self.base / "runtime"; anchor, _ = self.fixture(root)
            self.assertEqual(verify_runtime(root, anchor, self.lock)["platform"], "linux-x86_64")
            (root / "bin/node").write_bytes(b"bad!")
            with self.assertRaises(ValueError): verify_runtime(root, anchor, self.lock)

        def test_imported_store_lock_source_is_pinned(self):
            def altered_digest(path, algorithm="sha256"):
                return "f" * 64 if Path(path).name == "prepare_agent_python.py" else digest(path, algorithm)
            with patch("pi_supply_common.digest", side_effect=altered_digest):
                with self.assertRaisesRegex(ValueError, "hash mismatch: prepare_agent_python.py"):
                    load_lock()

        def test_local_conversation_source_is_pinned(self):
            modules = source_lock["build"].get("localModules", {})
            if not modules:
                self.skipTest("handshake-only producer has no conversation module")
            self.assertEqual(set(modules), {"conversation.mjs"})
            def altered_digest(path, algorithm="sha256"):
                return "f" * 64 if Path(path) == VENDOR / "conversation.mjs" else digest(path, algorithm)
            with patch("pi_supply_common.digest", side_effect=altered_digest):
                with self.assertRaisesRegex(ValueError, "hash mismatch: conversation.mjs"):
                    load_lock()

        def test_anchor_and_manifest_metadata_are_not_authorization(self):
            root = self.base / "runtime"; anchor, value = self.fixture(root)
            with self.assertRaises(ValueError): verify_runtime(root, "a" * 64, self.lock)
            value["nodeVersion"] = "unreviewed"; anchor = self.manifest(root, value)
            with self.assertRaises(ValueError): verify_runtime(root, anchor, self.lock)

        def test_extra_and_missing_file(self):
            root = self.base / "runtime"; anchor, _ = self.fixture(root); extra = root / "unlisted"
            extra.write_bytes(b"x")
            with self.assertRaises(ValueError): verify_runtime(root, anchor, self.lock)
            extra.unlink(); (root / "bin/node").unlink()
            with self.assertRaises(ValueError): verify_runtime(root, anchor, self.lock)

        def test_path_and_casefold_duplicates(self):
            for value in ("../x", "/x", "x\\y", "x:y", "x/../y", "NUL.txt", "x/", "a.", "a ", "a//b"):
                with self.subTest(value=value), self.assertRaises(ValueError): relative(value)
            root = self.base / "runtime"; _, value = self.fixture(root)
            duplicate = dict(value["files"][0]); duplicate["path"] = duplicate["path"].upper(); value["files"].append(duplicate)
            anchor = self.manifest(root, value)
            with self.assertRaises(ValueError): verify_runtime(root, anchor, self.lock)

        def test_runtime_entry_and_byte_caps(self):
            root = self.base / "runtime"; _, value = self.fixture(root); original = copy.deepcopy(value)
            value["files"][0]["size"] = self.lock["limits"]["maxTreeBytes"] + 1
            with self.assertRaises(ValueError): verify_runtime(root, self.manifest(root, value), self.lock)
            value = original; value["files"] *= self.lock["limits"]["maxFiles"]
            with self.assertRaises(ValueError): verify_runtime(root, self.manifest(root, value), self.lock)

        def test_duplicate_json_key(self):
            path = self.base / "duplicate.json"; path.write_bytes(b'{"schema":1,"schema":2}')
            with self.assertRaises(ValueError): read_json(path)

        def test_archive_byte_sha_and_sri(self):
            path = self.base / "archive"; path.write_bytes(b"archive")
            pin = {"size": 7, "sha256": digest(path)}; check_archive(path, pin)
            pin["integrity"] = "sha512-" + "A" * 88
            with self.assertRaises(ValueError): check_archive(path, pin)
            pin.pop("integrity"); path.write_bytes(b"corrupt")
            with self.assertRaises(ValueError): check_archive(path, pin)

        def test_archive_links_and_duplicates(self):
            path = self.base / "linked.tgz"
            with tarfile.open(path, "w:gz") as archive:
                item = tarfile.TarInfo("node"); item.type = tarfile.SYMTYPE; item.linkname = "../outside"; archive.addfile(item)
            with self.assertRaises(ValueError): archive_member(path, "node", self.base / "out")
            import io
            path = self.base / "duplicate.tgz"
            with tarfile.open(path, "w:gz") as archive:
                for _ in range(2):
                    item = tarfile.TarInfo("node"); item.size = 1; archive.addfile(item, io.BytesIO(b"x"))
            with self.assertRaises(ValueError): archive_member(path, "node", self.base / "out")

        def test_package_is_reproducible_and_refuses_overwrite(self):
            root = self.base / "runtime"; self.fixture(root); first = self.base / "one.zip"; second = self.base / "two.zip"
            supplier.package_generation(root, first); supplier.package_generation(root, second)
            self.assertEqual(digest(first), digest(second))
            with self.assertRaises(ValueError): supplier.package_generation(root, first)

        def test_inventory_uses_portable_ordinal_order(self):
            root = self.base / "runtime"; self.fixture(root)
            (root / "Z-notice").write_bytes(b"Z"); (root / "a-notice").write_bytes(b"a")
            paths = [p.relative_to(root).as_posix() for p in supplier.payload_files(root)]
            self.assertEqual(paths, sorted(paths)); self.assertLess(paths.index("Z-notice"), paths.index("a-notice"))

        def test_failed_prepare_preserves_pointer_and_old_generation(self):
            cache = self.base / "cache"; store = self.base / "store"; cache.mkdir(); store.mkdir()
            (store / ".pi-store.json").write_bytes(canonical({"schema": "paleo.pi-store.v1"}))
            old = store / "generations/old"; old.mkdir(parents=True); (old / "old").write_bytes(b"retained")
            current = store / "current.json"; current.write_bytes(b"original pointer")
            self.lock["archives"] = {}
            with patch.object(supplier, "materialize", side_effect=InterruptedError("staging interrupted")):
                with self.assertRaises(InterruptedError): supplier.prepare(cache, store, "linux-x86_64", self.lock, "a" * 64)
            self.assertEqual(current.read_bytes(), b"original pointer"); self.assertEqual((old / "old").read_bytes(), b"retained")
            self.assertFalse(list((store / "generations").glob(".pi-stage-*")))

        def test_late_package_failure_preserves_pointer(self):
            cache = self.base / "cache"; store = self.base / "store"; cache.mkdir(); self.lock["archives"] = {}
            def materialize(_cache, _work, stage, _target, _lock, _source_sha): return self.fixture(stage)[0]
            with patch.object(supplier, "materialize", side_effect=materialize):
                root, anchor = supplier.prepare(cache, store, "linux-x86_64", self.lock, "a" * 64)
                pointer = (store / "current.json").read_bytes(); before = digest(root / MANIFEST)
                occupied = self.base / "existing.zip"; occupied.write_bytes(b"keep")
                with self.assertRaises(ValueError): supplier.prepare(cache, store, "linux-x86_64", self.lock, "a" * 64, occupied)
                self.assertEqual((store / "current.json").read_bytes(), pointer); self.assertEqual(digest(root / MANIFEST), before)
                self.assertEqual(occupied.read_bytes(), b"keep"); self.assertEqual(anchor, before)

    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(SupplyTests))
    if not result.wasSuccessful(): raise ValueError("Pi supply regression failed")
