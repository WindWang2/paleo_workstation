#!/usr/bin/env python3
# 层：测试壳
"""Validate the pinned optional Python supply contract; --selftest exercises archive and activation failures."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
import unittest
from unittest import mock
import zipfile

import prepare_agent_python as supply


def check(lock_path):
    lock, sha = supply.load_lock(lock_path)
    required = {"schema", "profile", "runtimeVersion", "pythonVersion", "numpyVersion", "limits", "platforms"}
    if set(lock) != required:
        raise ValueError("unexpected lock root fields")
    if (lock["pythonVersion"], lock["numpyVersion"], lock["runtimeVersion"]) != ("3.14.10", "2.5.4", "cpython-3.14.10-numpy-2.5.4-1"):
        raise ValueError("profile version drift; update C++ identity and tests in the same change")
    for target, spec in lock["platforms"].items():
        if not spec.get("osRequirements"):
            raise ValueError("OS/ABI requirements missing")
        python, numpy = [next(row for row in spec["sources"] if row["role"] == role) for role in ("python", "numpy")]
        if target.startswith("windows"):
            if python["supplier"] != "Python Software Foundation" or spec["interpreter"] != "python.exe":
                raise ValueError("Windows runtime source drift")
            if spec.get("textFiles") != {"python314._pth": "python314.zip\n.\n"}:
                raise ValueError("Windows isolated import path drift")
            wheel_tag = "cp314-cp314-win_amd64.whl"
        else:
            if "Astral" not in python["supplier"] or spec["interpreter"] != "bin/python3.14":
                raise ValueError("Linux runtime source drift")
            if spec.get("numpyDirectory") != "lib/python3.14/site-packages":
                raise ValueError("Linux package layout drift")
            wheel_tag = "cp314-cp314-manylinux_2_27_x86_64.manylinux_2_28_x86_64.whl"
        if "numpy-2.5.4-" + wheel_tag != numpy["name"] or "freethreaded" in python["name"]:
            raise ValueError("Python/NumPy ABI mismatch")
    return sha


class SupplyTests(unittest.TestCase):
    def setUp(self):
        self.sandbox = Path(tempfile.mkdtemp(prefix="paleo-python-supply-"))
        self.cache = self.sandbox / "cache"
        self.cache.mkdir()
        self.store = self.sandbox / "store"
        self.lock, self.anchor = supply.load_lock()
        self.spec = self.lock["platforms"]["windows-x86_64"]
        self.python = self._zip("python.zip", [("python.exe", b"fake executable"), ("python314._pth", b"original")])
        self.numpy = self._zip("numpy.whl", [("numpy/__init__.py", b"__version__='2.5.4'"), ("numpy.libs/notice.txt", b"license")])
        self.spec["sources"] = [self.python, self.numpy]
        self.python["role"], self.numpy["role"] = "python", "numpy"

    def tearDown(self):
        def retry(function, path, exc):
            os.chmod(path, 0o700)
            function(path)
        shutil.rmtree(self.sandbox, onerror=retry)

    def _zip(self, name, entries):
        path = self.cache / name
        with zipfile.ZipFile(path, "w") as z:
            for member, value in entries:
                info = zipfile.ZipInfo(member)
                # ZipInfo's Windows constructor normalizes backslashes; exercise raw hostile names.
                info.filename = member
                z.writestr(info, value)
        return {"name": name, "role": "python", "format": "zip", "prefix": "", "url": "https://www.python.org/test/" + name,
                "sha256": supply.digest_file(path), "size": path.stat().st_size}

    def _prepare(self, **kwargs):
        return supply.prepare(self.lock, self.anchor, "windows-x86_64", self.cache, self.store, **kwargs)

    def test_offline_atomic_idempotence_and_old_generation_retained(self):
        first = self._prepare()
        second = self._prepare()
        self.assertEqual(first["manifestSha256"], second["manifestSha256"])
        root = Path(first["directory"])
        self.assertEqual((root / "python314._pth").read_bytes(), b"python314.zip\n.\n")
        self.assertEqual(len(supply.verify_tree(root, first["manifestSha256"])["files"]), 4)
        self.spec["textFiles"]["python314._pth"] += "# new controlled configuration\n"
        third = self._prepare()
        self.assertNotEqual(first["manifestSha256"], third["manifestSha256"])
        supply.verify_tree(root, first["manifestSha256"])
        self.assertEqual(json.loads((self.store / "current-windows-x86_64.json").read_bytes())["manifestSha256"], third["manifestSha256"])

    def test_bad_archive_keeps_previous_activation_and_no_partial_generation(self):
        first = self._prepare()
        (self.cache / self.numpy["name"]).write_bytes(b"corrupt")
        with self.assertRaisesRegex(ValueError, "offline source absent or corrupt"):
            self._prepare()
        self.assertEqual(json.loads((self.store / "current-windows-x86_64.json").read_bytes())["manifestSha256"], first["manifestSha256"])
        self.assertFalse(list(self.store.glob(".stage-*")))
        self.assertEqual(len(list((self.store / "generations").iterdir())), 1)

    def test_missing_cache_is_offline_error(self):
        (self.cache / self.python["name"]).unlink()
        with self.assertRaisesRegex(ValueError, "offline source absent or corrupt"):
            self._prepare()

    def test_zip_traversal_duplicates_aliases_and_links_rejected(self):
        for member in ("../escape", "/absolute", "a/../escape", "C:/escape", "a\\escape", "aux.txt"):
            with self.subTest(member=member):
                self.spec["sources"][1] = self._zip("bad.whl", [(member, b"bad")])
                self.spec["sources"][1]["role"] = "numpy"
                with self.assertRaises(ValueError):
                    self._prepare()
        self.spec["sources"][1] = self._zip("alias.whl", [("numpy/A.py", b"a"), ("numpy/a.py", b"b")])
        self.spec["sources"][1]["role"] = "numpy"
        with self.assertRaisesRegex(ValueError, "duplicate archive file"):
            self._prepare()
        linked = self.cache / "link.zip"
        with zipfile.ZipFile(linked, "w") as z:
            row = zipfile.ZipInfo("link")
            row.create_system = 3
            row.external_attr = (stat.S_IFLNK | 0o777) << 16
            z.writestr(row, "outside")
        self.spec["sources"][1] = {**self.numpy, "name": "link.zip", "sha256": supply.digest_file(linked), "size": linked.stat().st_size}
        with self.assertRaisesRegex(ValueError, "nonregular zip"):
            self._prepare()

    def test_tree_manifest_anchor_tamper_and_extra_file(self):
        prepared = self._prepare()
        root = Path(prepared["directory"])
        with self.assertRaisesRegex(ValueError, "anchor mismatch"):
            supply.verify_tree(root, "1" * 64)
        (root / "rogue.py").write_bytes(b"do not import")
        with self.assertRaisesRegex(ValueError, "content mismatch"):
            supply.verify_tree(root, prepared["manifestSha256"])
        (root / "rogue.py").unlink()
        target = root / "numpy/__init__.py"
        target.chmod(0o600)
        target.write_bytes(b"x" * target.stat().st_size)
        with self.assertRaisesRegex(ValueError, "content mismatch"):
            supply.verify_tree(root, prepared["manifestSha256"])

    def test_process_lock_and_cleanup_boundary(self):
        self.store.mkdir()
        with supply.StoreLock(self.store):
            with self.assertRaisesRegex(RuntimeError, "already in progress"):
                with supply.StoreLock(self.store):
                    self.fail("second lock must not succeed")
        with supply.StoreLock(self.store):
            pass
        with self.assertRaisesRegex(ValueError, "outside runtime store"):
            supply.remove_stage(self.sandbox, self.store)

    def test_process_death_releases_preparation_lock(self):
        self.store.mkdir()
        marker = self.sandbox / "locked.marker"
        code = ("import sys,time;from pathlib import Path;sys.path.insert(0," + repr(str(Path(supply.__file__).parent)) + ");"
                "from prepare_agent_python import StoreLock;"
                "lock=StoreLock(" + repr(str(self.store)) + ");lock.__enter__();"
                "Path(" + repr(str(marker)) + ").write_text('locked');time.sleep(30)")
        flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        child = subprocess.Popen([sys.executable, "-c", code], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, creationflags=flags)
        try:
            until = time.monotonic() + 5
            while not marker.exists() and child.poll() is None and time.monotonic() < until:
                time.sleep(0.01)
            self.assertTrue(marker.exists(), "child did not acquire store lock")
            with self.assertRaisesRegex(RuntimeError, "already in progress"):
                with supply.StoreLock(self.store):
                    self.fail("live child lock was ignored")
            child.kill()
            child.wait(timeout=5)
            with supply.StoreLock(self.store):
                pass
        finally:
            if child.poll() is None:
                child.kill()
                child.wait(timeout=5)
            child.stderr.close()

    def test_failed_activation_keeps_old_pointer(self):
        first = self._prepare()
        self.spec["textFiles"]["python314._pth"] += "# new\n"
        actual_replace = supply.os.replace
        def fail_pointer(source, destination):
            if Path(destination).name.startswith("current-"):
                raise OSError("injected activation failure")
            return actual_replace(source, destination)
        with mock.patch.object(supply.os, "replace", side_effect=fail_pointer):
            with self.assertRaisesRegex(OSError, "activation failure"):
                self._prepare()
        self.assertEqual(json.loads((self.store / "current-windows-x86_64.json").read_bytes())["manifestSha256"], first["manifestSha256"])
        supply.verify_tree(first["directory"], first["manifestSha256"])
        self.assertFalse(list(self.store.glob(".stage-*")))

    def test_archive_budgets(self):
        self.lock["limits"]["maxTreeBytes"] = 2
        with self.assertRaisesRegex(ValueError, "budget"):
            self._prepare()

    def test_tar_internal_link_materialized_external_and_cycle_rejected(self):
        path = self.cache / "python.tar.gz"
        def archive(link):
            with tarfile.open(path, "w:gz") as tar:
                row = tarfile.TarInfo("python/bin/python3.14")
                row.size, row.mode = 6, 0o755
                tar.addfile(row, io.BytesIO(b"binary"))
                row = tarfile.TarInfo("python/bin/python3")
                row.type, row.linkname = tarfile.SYMTYPE, link
                tar.addfile(row)
            return {"name": path.name, "role": "python", "format": "tar.gz", "prefix": "python/", "size": path.stat().st_size,
                    "sha256": supply.digest_file(path)}
        destination = self.sandbox / "tar-out"
        destination.mkdir()
        source = archive("python3.14")
        supply.unpack(source, path, destination, set(), self.lock["limits"], "linux-x86_64")
        self.assertFalse((destination / "bin/python3").is_symlink())
        self.assertEqual((destination / "bin/python3").read_bytes(), b"binary")
        for link in ("../../escape", "/bin/sh", "python3"):
            with self.subTest(link=link):
                source = archive(link)
                target = self.sandbox / ("bad-tar-" + hashlib.sha256(link.encode()).hexdigest())
                target.mkdir()
                with self.assertRaises(ValueError):
                    supply.unpack(source, path, target, set(), self.lock["limits"], "linux-x86_64")

    def test_lock_digest_source_and_abi_contract(self):
        for mutate in (
            lambda lock: lock["platforms"]["windows-x86_64"]["sources"][0].update(sha256="0" * 64),
            lambda lock: lock["platforms"]["windows-x86_64"]["sources"][0].update(url="http://www.python.org/untrusted.zip"),
            lambda lock: lock["platforms"]["windows-x86_64"].update(textFiles={"python314._pth": ".\nimport site\n"}),
        ):
            lock = json.loads(supply.DEFAULT_LOCK.read_bytes())
            mutate(lock)
            path = self.sandbox / "bad-lock.json"
            path.write_bytes(supply.canonical(lock))
            with self.assertRaises(ValueError):
                check(path)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", type=Path, default=supply.DEFAULT_LOCK)
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--runtime-root", type=Path)
    parser.add_argument("--manifest-sha256")
    args = parser.parse_args(argv)
    if args.selftest:
        return 0 if unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(SupplyTests)).wasSuccessful() else 1
    try:
        anchor = check(args.lock)
        if args.runtime_root:
            if not args.manifest_sha256:
                parser.error("--runtime-root requires --manifest-sha256")
            manifest = supply.verify_tree(args.runtime_root, args.manifest_sha256)
            if manifest["sourceLockSha256"] != anchor:
                raise ValueError("runtime is not derived from the selected source lock")
        elif args.manifest_sha256:
            parser.error("--manifest-sha256 requires --runtime-root")
        print("agent-python-runtime: source lock and requested tree OK (" + anchor + ")")
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print("agent-python-runtime: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
