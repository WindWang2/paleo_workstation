# 层：测试壳
"""Check optional template supply sources, offline bundles and immutable generations."""
from __future__ import annotations

import argparse
from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock
import zipfile

sys.dont_write_bytecode = True
import prepare_agent_python_scripts as supply


def check(source_root=None, archive=None, generation=None, anchor=None):
    if archive:
        payloads, manifest = supply.read_archive(archive, anchor)
    elif generation:
        manifest = supply.verify_tree(generation, anchor)
        if Path(generation).name != anchor:
            raise ValueError("generation directory must be its manifest SHA")
        payloads = None
    else:
        payloads, manifest, anchor = supply.source_bundle(source_root or supply.ROOT)
    if (archive or generation) and source_root:
        expected, unused, source_anchor = supply.source_bundle(source_root)
        if anchor != source_anchor or payloads is not None and payloads != expected:
            raise ValueError("bundle does not match the selected repository sources")
    return {"schema": manifest["schema"], "manifestSha256": anchor,
            "sourceRuntimeLockSha256": manifest["sourceRuntimeLockSha256"], "licenseStatus": manifest["licenseStatus"],
            "scripts": {row["id"]: row["sha256"] for row in manifest["scripts"]}}


class ScriptsSupplyTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="paleo-python-scripts-test-")
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.templates = self.source / "vendor/agent-python/templates"
        self.templates.mkdir(parents=True)
        for identity in supply.SCRIPTS:
            (self.templates / (identity + ".py")).write_bytes(b"# fixture: must never execute\nraise RuntimeError('not executed')\n")
        lock = supply.regular_bytes(supply.ROOT / "vendor/agent-python/runtime.lock.json", 128 * 1024)
        (self.source / "vendor/agent-python/runtime.lock.json").write_bytes(lock)
        (self.source / "NOTICE.md").write_bytes(b"Test fixture notice only. No licence grant.\r\n")
        self.archive = self.root / "scripts.zip"
        self.store = self.root / "store"
        self.bundle = supply.bundle(self.source, self.archive)
        self.anchor = self.bundle["manifestSha256"]

    def tearDown(self):
        self.temporary.cleanup()

    def _prepare(self):
        return supply.prepare(self.archive, self.anchor, self.store)

    def _upgrade(self):
        path = self.templates / "summary.py"
        path.write_bytes(path.read_bytes() + b"# reviewed fixture revision\n")
        self.archive = self.root / "scripts-next.zip"
        self.bundle = supply.bundle(self.source, self.archive)
        self.anchor = self.bundle["manifestSha256"]

    def _unchanged(self, pointer):
        self.assertEqual((self.store / "current.json").read_bytes(), pointer)
        previous = json.loads(pointer)
        supply.verify_tree(self.store / previous["generation"], previous["manifestSha256"])
        self.assertFalse(list(self.store.rglob(".scripts-stage-*")))

    def _custom(self, entries, name="hostile.zip"):
        path = self.root / name
        with zipfile.ZipFile(path, "w") as archive:
            for filename, data, mode, compression in entries:
                row = zipfile.ZipInfo(filename)
                row.filename = row.orig_filename = filename
                row.create_system = 3
                row.external_attr = mode << 16
                row.compress_type = compression
                archive.writestr(row, data)
        return path

    def test_deterministic_offline_activation_and_no_execution(self):
        with mock.patch.object(subprocess, "Popen", side_effect=AssertionError("must not execute supplied code")):
            result = self._prepare()
            self.assertEqual(self._prepare(), result)
            again = supply.bundle(self.source, self.archive)
        self.assertEqual(again, self.bundle)
        self.assertEqual(check(self.source, archive=self.archive, anchor=self.anchor)["manifestSha256"], self.anchor)
        manifest = supply.verify_tree(result["directory"], self.anchor)
        self.assertEqual([row["id"] for row in manifest["scripts"]], list(supply.SCRIPTS))
        self.assertEqual(manifest["licenseStatus"], "notice-only")
        self.assertEqual((Path(result["directory"]) / "NOTICE.md").read_bytes(), b"Test fixture notice only. No licence grant.\n")
        self.assertEqual(set(p.name for p in Path(result["directory"]).iterdir()),
                         {identity + ".py" for identity in supply.SCRIPTS} | {supply.MANIFEST, "NOTICE.md"})

    def test_new_generation_keeps_all_previous_bytes(self):
        old = self._prepare()
        previous = {path.name: path.read_bytes() for path in Path(old["directory"]).iterdir()}
        self._upgrade()
        new = self._prepare()
        self.assertNotEqual(old["manifestSha256"], new["manifestSha256"])
        self.assertEqual(previous, {path.name: path.read_bytes() for path in Path(old["directory"]).iterdir()})
        self.assertEqual(len(list((self.store / "generations").iterdir())), 2)
        self.assertEqual(json.loads((self.store / "current.json").read_bytes())["manifestSha256"], self.anchor)

    def test_actual_optional_licence_is_retained_without_inventing_a_name(self):
        (self.source / "LICENSE").write_bytes(b"Fixture licence text, no SPDX identity asserted.\r\n")
        payloads, manifest, anchor = supply.source_bundle(self.source)
        self.assertEqual(manifest["licenseStatus"], "repository-texts-included")
        self.assertEqual(payloads["LICENSE"], b"Fixture licence text, no SPDX identity asserted.\n")
        self.assertEqual({row["path"] for row in manifest["licenses"]}, {"NOTICE.md", "LICENSE"})
        self.assertTrue(supply.digest(anchor))

    def test_wrong_anchor_and_corrupt_archive_do_not_touch_current(self):
        self._prepare()
        old = (self.store / "current.json").read_bytes()
        with self.assertRaises(ValueError):
            supply.prepare(self.archive, "1" * 64, self.store)
        raw = bytearray(self.archive.read_bytes())
        raw[60] ^= 1
        self.archive.write_bytes(raw)
        with self.assertRaises((ValueError, zipfile.BadZipFile)):
            self._prepare()
        self._unchanged(old)

    def test_archive_paths_duplicates_links_types_and_extra_items(self):
        self._prepare()
        old = (self.store / "current.json").read_bytes()
        raw_files, unused, unused_anchor = supply.source_bundle(self.source)
        ordinary = [(name, raw, stat.S_IFREG | 0o444, zipfile.ZIP_STORED) for name, raw in sorted(raw_files.items())]
        hostile = ["../escape", "/absolute", "C:/escape", "a\\escape", "a/../summary.py", "SUMMARY.PY", "rogue.py", "summary.py\0../bad"]
        variants = [[*ordinary, (name, b"x", stat.S_IFREG | 0o444, zipfile.ZIP_STORED)] for name in hostile]
        variants += [[*ordinary, ordinary[0]], ordinary[:-1],
                     [(name, raw, stat.S_IFLNK | 0o777 if name == "summary.py" else mode, comp) for name, raw, mode, comp in ordinary],
                     [(name, raw, stat.S_IFIFO | 0o600 if name == "summary.py" else mode, comp) for name, raw, mode, comp in ordinary],
                     [(name, raw, mode, zipfile.ZIP_DEFLATED) for name, raw, mode, comp in ordinary]]
        for index, entries in enumerate(variants):
            with self.subTest(index=index):
                path = self._custom(entries)
                with self.assertRaises((ValueError, zipfile.BadZipFile)):
                    supply.prepare(path, self.anchor, self.store)
                self._unchanged(old)

    def test_noncanonical_zip_comments_prefix_trailer_and_budget(self):
        self._prepare()
        old = (self.store / "current.json").read_bytes()
        raw = self.archive.read_bytes()
        for value in (b"prefix" + raw, raw + b"trailer", b"x" * (supply.MAX_ARCHIVE_BYTES + 1)):
            bad = self.root / "bad.zip"
            bad.write_bytes(value)
            with self.assertRaises((ValueError, zipfile.BadZipFile)):
                supply.prepare(bad, self.anchor, self.store)
        with zipfile.ZipFile(self.archive, "a") as archive:
            archive.comment = b"not part of the supply format"
        with self.assertRaises(ValueError):
            self._prepare()
        self._unchanged(old)

    def test_manifest_field_types_roles_and_budgets_are_strict(self):
        payloads, original, unused = supply.source_bundle(self.source)
        mutations = [lambda value: value.update(version=True), lambda value: value.update(extra="unexpected"),
                     lambda value: value.update(treeSize=0), lambda value: value.update(sourceRuntimeLockSha256="0" * 64),
                     lambda value: value["scripts"][0].update(size=supply.MAX_CODE_BYTES + 1),
                     lambda value: value["scripts"][0].update(id="unregistered"),
                     lambda value: value["scripts"][0].update(path="../escape"),
                     lambda value: value["scripts"][0].update(size=True),
                     lambda value: value["scripts"].__setitem__(0, value["licenses"][0])]
        for change in mutations:
            value = json.loads(json.dumps(original))
            change(value)
            raw = supply.canonical(value)
            with self.assertRaises(ValueError):
                supply.validate_manifest(raw, supply.sha256(raw))
        switched = json.loads(json.dumps(original))
        switched["scripts"] = [original["licenses"][0], *original["scripts"][:2]]
        switched["licenses"] = [original["scripts"][2]]
        raw = supply.canonical(switched)
        with self.assertRaises(ValueError):
            supply.validate_manifest(raw, supply.sha256(raw))
        for raw in (b'{"schema":1,"schema":2}', b'{"schema":NaN}', b'{}\r\n'):
            with self.assertRaises(ValueError):
                supply.validate_manifest(raw, supply.sha256(raw))
        changed = dict(payloads)
        changed["summary.py"] += b"# changed without manifest update\n"
        bad = self.root / "content.zip"
        bad.write_bytes(supply.archive_bytes(changed))
        with self.assertRaises(ValueError):
            supply.read_archive(bad, self.anchor)

    def test_invalid_source_text_size_links_and_extra_template(self):
        path = self.templates / "summary.py"
        original = path.read_bytes()
        for raw in (b"x=1\r\n", b"\xff\n", b"x=\0\n", b"if\n", b"x" * (supply.MAX_CODE_BYTES + 1)):
            path.write_bytes(raw)
            with self.assertRaises(ValueError):
                supply.source_bundle(self.source)
        path.write_bytes(original)
        alias = self.root / "alias.py"
        os.link(path, alias)
        with self.assertRaises(ValueError):
            supply.source_bundle(self.source)
        alias.unlink()
        (self.templates / "unregistered.py").write_bytes(b"pass\n")
        with self.assertRaises(ValueError):
            supply.source_bundle(self.source)

    def test_generation_tampering_and_extra_file_are_rejected(self):
        value = self._prepare()
        root = Path(value["directory"])
        root.chmod(0o700)
        extra = root / "extra.py"
        extra.write_bytes(b"pass\n")
        with self.assertRaises(ValueError):
            supply.verify_tree(root, self.anchor)
        extra.unlink()
        if os.name != "nt":
            root.chmod(0o555)
        path = root / "summary.py"
        path.chmod(0o600)
        before = path.stat()
        path.write_bytes(b"X" * before.st_size)
        os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns))
        with self.assertRaisesRegex(ValueError, "SHA/size"):
            supply.verify_tree(root, self.anchor)

    def test_readonly_generation_directory_contract(self):
        value = self._prepare()
        root = Path(value["directory"])
        self.assertTrue(all(path.stat().st_mode & 0o222 == 0 for path in root.iterdir()))
        if os.name != "nt":
            root.chmod(0o755)
            with self.assertRaisesRegex(ValueError, "directory is writable"):
                supply.verify_tree(root, self.anchor)

    def test_licence_text_is_checked_even_with_a_matching_external_anchor(self):
        self._prepare()
        old = (self.store / "current.json").read_bytes()
        for index, data in enumerate((b"\xff\n", b"x\0\n", b"text\r\n")):
            payloads, manifest, unused = supply.source_bundle(self.source)
            row = next(row for row in manifest["licenses"] if row["path"] == "NOTICE.md")
            row.update(size=len(data), sha256=supply.sha256(data))
            payloads["NOTICE.md"] = data
            manifest["treeSize"] = sum(len(raw) for name, raw in payloads.items() if name != supply.MANIFEST)
            raw = supply.canonical(manifest)
            anchor = supply.sha256(raw)
            payloads[supply.MANIFEST] = raw
            archive = self.root / "bad-text.zip"
            archive.write_bytes(supply.archive_bytes(payloads))
            with self.assertRaises(ValueError):
                supply.prepare(archive, anchor, self.store)
            self._unchanged(old)
            tree = self.root / ("text-tree-" + str(index))
            tree.mkdir()
            for name, raw in payloads.items():
                path = tree / name
                path.write_bytes(raw)
                path.chmod(0o444)
            if os.name != "nt":
                tree.chmod(0o555)
            with self.assertRaises(ValueError):
                supply.verify_tree(tree, anchor)

    def test_runtime_store_is_not_modified_or_mixed(self):
        self.store.mkdir()
        runtime_lock = self.store / ".prepare.lock"
        runtime_lock.write_bytes(b"0")
        with self.assertRaisesRegex(ValueError, "dedicated scripts store"):
            self._prepare()
        self.assertEqual(runtime_lock.read_bytes(), b"0")
        self.assertFalse((self.store / ".scripts.lock").exists())
        self.assertFalse((self.store / "generations").exists())

    def test_partial_write_interrupt_cleans_stage_and_keeps_pointer(self):
        self._prepare()
        old = (self.store / "current.json").read_bytes()
        self._upgrade()
        actual = supply._write_new
        calls = 0
        def interrupt(path, raw):
            nonlocal calls
            calls += 1
            if calls == 2:
                Path(path).write_bytes(b"")
                raise KeyboardInterrupt
            return actual(path, raw)
        with mock.patch.object(supply, "_write_new", side_effect=interrupt):
            with self.assertRaises(KeyboardInterrupt):
                self._prepare()
        self._unchanged(old)

    def test_failed_generation_publish_and_activation_keep_pointer(self):
        self._prepare()
        old = (self.store / "current.json").read_bytes()
        self._upgrade()
        actual = supply.os.replace
        for fail_pointer in (False, True):
            def fail(source, destination):
                target = Path(destination)
                selected = target.name == "current.json" if fail_pointer else target.parent.name == "generations"
                if selected:
                    raise OSError("injected commit interruption")
                return actual(source, destination)
            with mock.patch.object(supply.os, "replace", side_effect=fail):
                with self.assertRaises(OSError):
                    self._prepare()
            self._unchanged(old)
        supply.verify_tree(self.store / "generations" / self.anchor, self.anchor)

    def test_bad_existing_generation_and_pointer_are_never_repaired_in_place(self):
        self._prepare()
        old = (self.store / "current.json").read_bytes()
        self._upgrade()
        damaged = self.store / "generations" / self.anchor
        damaged.mkdir()
        with self.assertRaises((OSError, ValueError)):
            self._prepare()
        self.assertEqual(list(damaged.iterdir()), [])
        self._unchanged(old)
        selection = json.loads(old)
        selection["generation"] = "../outside"
        invalid = supply.canonical(selection)
        (self.store / "current.json").write_bytes(invalid)
        with self.assertRaises(ValueError):
            self._prepare()
        self.assertEqual((self.store / "current.json").read_bytes(), invalid)

    def test_lock_and_cleanup_boundary(self):
        self.store.mkdir()
        with supply.StoreLock(self.store):
            with self.assertRaises((OSError, RuntimeError)):
                with supply.StoreLock(self.store):
                    self.fail("second writer acquired lock")
        with supply.StoreLock(self.store):
            pass
        with self.assertRaises(ValueError):
            supply.remove_stage(self.root, self.store)

    def test_process_death_before_activation_keeps_old_pointer_and_releases_lock(self):
        self._prepare()
        old = (self.store / "current.json").read_bytes()
        self._upgrade()
        marker = self.root / "ready"
        program = "\n".join(["import sys,time", "from pathlib import Path", "sys.dont_write_bytecode=True",
            "sys.path.insert(0," + repr(str(Path(supply.__file__).parent)) + ")", "import prepare_agent_python_scripts as s",
            "actual=s.os.replace", "def pause(source,destination):", " if Path(destination).name=='current.json':",
            "  Path(" + repr(str(marker)) + ").write_text('ready')", "  time.sleep(30)",
            " return actual(source,destination)", "s.os.replace=pause",
            "s.prepare(" + repr(str(self.archive)) + "," + repr(self.anchor) + "," + repr(str(self.store)) + ")"])
        flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        child = subprocess.Popen([sys.executable, "-c", program], stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=flags)
        try:
            until = time.monotonic() + 5
            while not marker.exists() and child.poll() is None and time.monotonic() < until:
                time.sleep(0.01)
            self.assertTrue(marker.exists(), "preparer never reached activation boundary")
            self.assertEqual((self.store / "current.json").read_bytes(), old)
            child.kill()
            child.wait(timeout=5)
            self._unchanged(old)
            self.assertEqual(self._prepare()["manifestSha256"], self.anchor)
        finally:
            if child.poll() is None:
                child.kill()
                child.wait(timeout=5)
            child.stdout.close()
            child.stderr.close()

    def test_command_line_requires_independent_anchor(self):
        with self.assertRaises(SystemExit):
            supply.main(["--archive", str(self.archive), "--store", str(self.store)])
        output = io.StringIO()
        with redirect_stdout(output):
            self.assertEqual(supply.main(["--archive", str(self.archive), "--manifest-sha256", self.anchor, "--store", str(self.store)]), 0)
        self.assertEqual(json.loads(output.getvalue())["manifestSha256"], self.anchor)


def selftest():
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ScriptsSupplyTests)
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--archive", type=Path)
    mode.add_argument("--generation", type=Path)
    mode.add_argument("--selftest", action="store_true")
    parser.add_argument("--source-root", type=Path)
    parser.add_argument("--manifest-sha256")
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    if bool(args.archive or args.generation) != bool(args.manifest_sha256):
        parser.error("archive/generation checking requires an independently supplied --manifest-sha256")
    try:
        print(json.dumps(check(args.source_root, args.archive, args.generation, args.manifest_sha256), sort_keys=True))
        return 0
    except (OSError, ValueError, RuntimeError, zipfile.BadZipFile, KeyError, TypeError) as error:
        print("agent-python-scripts-check: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
