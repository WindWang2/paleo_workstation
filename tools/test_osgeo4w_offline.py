import hashlib
import io
import json
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ensure_qgis_resources import Fail, osgeo4w_database
from osgeo4w_prefix import RECEIPT, check as check_prefix, upgrade_legacy_pdal
from build_pdal_arrow import PLUGINS, STAMP
from osgeo4w_offline import build_lock, check_installed, fetch, materialize


class OfflinePackagesTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.original = self.root / "original"
        self.cache = self.root / "cache"
        self.index = self.root / "setup.ini"
        self.installer = self.original / "osgeo4w-setup.exe"
        self.archive = "x86_64/release/sample/sample-1-2.tar.bz2"
        self.archive_file = self.original / self.archive
        self.archive_file.parent.mkdir(parents=True)
        self.installer.write_bytes(b"setup")
        self.archive_file.write_bytes(b"archive")
        upstream = hashlib.sha256(b"archive").hexdigest()
        (self.original / "x86_64/setup.ini").write_text(
            "arch: x86_64\n\n@ sample\nversion: 1-2\n"
            f"install: {self.archive} 7 {upstream}\n[prev]\nversion: 1-1\n\n"
            "@ unused\nversion: 2-0\n",
            encoding="utf-8",
        )
        self.pin = {"site": "https://example.invalid", "installer_url": "https://example.invalid/setup",
                    "installer_sha256": hashlib.sha256(b"setup").hexdigest(),
                    "packages": {"sample": "1-2"}, "closure": {"sample": "1-2"}}
        self.lock = build_lock(self.original, self.installer, self.index, self.pin)

    def prepare(self):
        return materialize(self.lock, self.pin, self.index, self.cache, offline=True)

    def test_offline_verifies_bytes_and_rejects_extra_index(self):
        with patch("osgeo4w_offline.urllib.request.urlopen", side_effect=AssertionError("network")):
            with self.assertRaisesRegex(ValueError, "missing offline archive"):
                self.prepare()
            (self.cache / "osgeo4w-setup.exe").write_bytes(self.installer.read_bytes())
            archive = self.cache / "archives" / self.archive
            archive.parent.mkdir(parents=True)
            archive.write_bytes(b"corrupt")
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                self.prepare()
            archive.write_bytes(self.archive_file.read_bytes())
            self.prepare()
            self.assertNotIn("[prev]", self.index.read_text(encoding="utf-8"))
            self.assertNotIn("@ unused", self.index.read_text(encoding="utf-8"))
            (archive.parent / "setup.ini").write_text("unexpected", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "unexpected files"):
                self.prepare()

    def test_truncated_download_retries_before_publishing(self):
        destination = self.cache / self.archive
        data = self.archive_file.read_bytes()
        with patch("osgeo4w_offline.urllib.request.urlopen",
                   side_effect=[io.BytesIO(data[:3]), io.BytesIO(data)]) as get:
            with patch("osgeo4w_offline.time.sleep"):
                fetch("https://example.invalid/archive", destination, self.lock["packages"]["sample"])
        self.assertEqual(get.call_count, 2)
        self.assertEqual(destination.read_bytes(), data)
        self.assertEqual(list(destination.parent.iterdir()), [destination])

    def test_prefix_reuse_requires_original_files_and_profile(self):
        prefix = self.root / "prefix"
        prefix.mkdir()
        database = prefix / "srs.db"
        database.write_bytes(b"initial")
        with self.assertRaisesRegex(ValueError, "Missing"):
            check_prefix(prefix, False)
        check_prefix(prefix, False, record=True)
        check_prefix(prefix, False)
        with self.assertRaisesRegex(ValueError, "inputs changed"):
            check_prefix(prefix, True)
        database.write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "srs.db"):
            check_prefix(prefix, False)
        database.unlink()
        with self.assertRaisesRegex(ValueError, "srs.db"):
            check_prefix(prefix, False)

    def test_verified_legacy_prefix_upgrade_retries_only_its_two_plugins(self):
        prefix = self.root / "prefix"
        binary = prefix / "bin"
        binary.mkdir(parents=True)
        unrelated = binary / "gdal.dll"
        unrelated.write_bytes(b"original")
        for dll in PLUGINS.values():
            (binary / dll).write_bytes(b"upstream")
        check_prefix(prefix, False, record=True)
        receipt = prefix / RECEIPT
        original = json.loads(receipt.read_text(encoding="utf-8"))
        source_lock = original["locks"].pop("pdal_arrow")
        receipt.write_text(json.dumps(original), encoding="utf-8")

        def interrupted():
            (binary / PLUGINS["Reader"]).write_bytes(b"rebuilt")
            raise OSError("writer is open")

        unrelated.write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "gdal.dll"):
            upgrade_legacy_pdal(prefix, self.fail)
        unrelated.write_bytes(b"original")
        with self.assertRaisesRegex(OSError, "writer is open"):
            upgrade_legacy_pdal(prefix, interrupted)
        self.assertEqual(json.loads(receipt.read_text(encoding="utf-8")), original)

        def complete():
            for dll in PLUGINS.values():
                (binary / dll).write_bytes(b"rebuilt")
            (prefix / STAMP).write_text(source_lock, encoding="ascii")

        upgrade_legacy_pdal(prefix, complete)
        check_prefix(prefix, False)
        upgrade_legacy_pdal(prefix, self.fail)

    def test_recover_only_locked_windows_srs(self):
        # Build a real, digest-locked package and index with the production name.
        payload = b"windows-crs"
        pkg = "qgis-common"
        with tarfile.open(self.archive_file, "w:bz2") as archive:
            info = tarfile.TarInfo("./apps/qgis/resources/srs.db")
            info.size = len(payload)
            archive.addfile(info, io.BytesIO(payload))
        raw = self.archive_file.read_bytes()
        index = self.original / "x86_64/setup.ini"
        index.write_text(f"arch: x86_64\n\n@ {pkg}\nversion: 1-2\n"
                         f"install: {self.archive} {len(raw)} {hashlib.sha256(raw).hexdigest()}\n")
        packages = {pkg: "1-2"}
        pin = dict(self.pin, packages=packages, closure=packages)
        locked_index = self.root / "osgeo4w-setup.ini"
        lock = build_lock(self.original, self.installer, locked_index, pin)
        lockfile = self.root / "lock.json"
        lockfile.write_text(json.dumps(lock))
        cached = self.cache / "archives" / self.archive
        cached.parent.mkdir(parents=True)
        cached.write_bytes(raw)
        prefix = self.root / "qgis"
        prefix.mkdir()
        with patch("ensure_qgis_resources.LOCK", lockfile), patch("ensure_qgis_resources.manifest", return_value=pin):
            with self.assertRaisesRegex(Fail, "Missing or damaged"):
                osgeo4w_database(prefix, self.cache, check_only=True)
            target = osgeo4w_database(prefix, self.cache)
            self.assertEqual(target.read_bytes(), payload)
            target.write_bytes(b"corrupt")
            cached.write_bytes(b"broken")
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                osgeo4w_database(prefix, self.cache)
            self.assertEqual(target.read_bytes(), b"corrupt")
            cached.write_bytes(raw)
            osgeo4w_database(prefix, self.cache)
            osgeo4w_database(prefix, self.cache, check_only=True)
            self.assertEqual(target.read_bytes(), payload)

    def test_exact_installed_versions(self):
        database = self.root / "installed.db"
        exact = "sample sample-1-2.tar.bz2 0\n"
        for content, reason in (("INSTALLED.DB 2\nsample sample-1-1.tar.bz2 0\n", "differs"),
                                ("INSTALLED.DB 2\n" + exact * 2, "duplicate")):
            with self.subTest(reason=reason):
                database.write_text(content, encoding="utf-8")
                with self.assertRaisesRegex(ValueError, reason):
                    check_installed(database, self.pin["closure"])
        database.write_text("INSTALLED.DB 2\n" + exact, encoding="utf-8")
        check_installed(database, self.pin["closure"])


if __name__ == "__main__":
    unittest.main()
