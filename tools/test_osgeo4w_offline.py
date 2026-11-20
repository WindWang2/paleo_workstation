import hashlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from osgeo4w_prefix import check as check_prefix
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
