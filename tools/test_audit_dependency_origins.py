#!/usr/bin/env python3
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from audit_dependency_origins import Report


class OriginTest(unittest.TestCase):
    def test_same_name_outside_prefix_and_missing_transitive_elf(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            prefix, host = root / "prefix", root / "prefix-extra"
            prefix.mkdir()
            host.mkdir()
            binary = root / "program"
            binary.write_bytes(b"\x7fELF")
            (prefix / "libmatch.so").touch()
            (host / "libmatch.so").touch()
            (host / "libc.so.6").touch()
            audit = Report([("locked", prefix)], [("libc.so.6", host / "libc.so.6")])
            trace = f"""libmatch.so => {host / 'libmatch.so'} (0x1)
libchild.so => not found
libc.so.6 => {host / 'libc.so.6'} (0x2)
"""
            # ldd text follows ELF's / paths; normalize separators under Windows
            # without trying to run ldd on this fixture.
            if binary.drive:
                audit.add("loader-resolution", str(binary), "libmatch.so", host / "libmatch.so")
                audit.add("loader-resolution", str(binary), "libchild.so")
                audit.add("loader-resolution", str(binary), "libc.so.6", host / "libc.so.6")
            else:
                with patch("audit_dependency_origins.run", return_value=trace):
                    audit.elf(binary)
            self.assertEqual({row["name"]: row["origin"] for row in audit.rows.values()},
                             {"libmatch.so": "outside", "libchild.so": "missing", "libc.so.6": "host-abi"})

    @unittest.skipIf(Path.cwd().drive, "ELF paths require a POSIX host")
    def test_loader_absolute_spelling_uses_exact_abi_name(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            binary = root / "program"
            binary.write_bytes(b"\x7fELF")
            name = "ld-linux-x86-64.so.2"
            loader = root / name
            loader.touch()
            report = Report([], [(name, loader)])
            with patch("audit_dependency_origins.run", return_value=f"/lib64/{name} => {loader} (0x1)"):
                report.elf(binary)
            row, = report.rows.values()
            self.assertEqual((row["name"], row["origin"]), (name, "host-abi"))

    def test_pe_api_set_is_kept_separate_from_missing_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            binary = root / "program.exe"
            binary.touch()
            report = Report([], [])
            output = "    api-ms-win-core-file-l1-1-0.dll\n    native_dependency.dll\n"
            with patch("audit_dependency_origins.run", return_value=output):
                report.pe(binary, "dumpbin", [root])
            self.assertEqual({row["name"]: row["origin"] for row in report.rows.values()}, {
                "api-ms-win-core-file-l1-1-0.dll": "api-set-unresolved",
                "native_dependency.dll": "missing",
            })

    def test_link_file_api_preserves_target_and_exact_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            reply = build / ".cmake/api/v1/reply"
            reply.mkdir(parents=True)
            library = build / "linked.lib"
            library.touch()
            documents = {
                "index-1.json": {"reply": {"codemodel-v2": {"jsonFile": "model.json"}}},
                "model.json": {"configurations": [{"targets": [{"name": "example", "jsonFile": "target.json"}]}]},
                "target.json": {"link": {"commandFragments": [
                    {"role": "libraries", "fragment": f'"{library}"'},
                ]}},
            }
            for name, content in documents.items():
                (reply / name).write_text(json.dumps(content), encoding="utf-8")
            report = Report([("build", build)], [])
            self.assertTrue(report.file_api(build))
            row, = report.rows.values()
            self.assertEqual((row["stage"], row["consumer"], row["origin"]),
                             ("link", "example", "project"))


if __name__ == "__main__":
    unittest.main()
