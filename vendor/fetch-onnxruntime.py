#!/usr/bin/env python3
"""Install the pinned Microsoft native ONNX Runtime package for this host."""

import hashlib
import json
import platform
import shutil
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path


def download(pin, archive):
    expected = pin["archive"]
    sha256 = hashlib.sha256()
    count = 0
    with urllib.request.urlopen(pin["url"], timeout=60) as source, archive.open("xb") as out:
        while chunk := source.read(1024 * 1024):
            count += len(chunk)
            if count > expected["size_bytes"]:
                raise ValueError("ONNX Runtime archive exceeds its pinned size")
            sha256.update(chunk)
            out.write(chunk)
    if count != expected["size_bytes"] or sha256.hexdigest() != expected["sha256"]:
        raise ValueError("ONNX Runtime archive size or SHA-256 differs from vendor/manifest.json")


def extract(archive, stage, rid, linux):
    (stage / "include").mkdir()
    (stage / "lib").mkdir()
    with zipfile.ZipFile(archive) as package:
        def copy(member, path):
            with package.open(member) as source, path.open("xb") as out:
                shutil.copyfileobj(source, out)

        for member in package.namelist():
            directory, _, name = member.rpartition("/")
            if directory == "build/native/include" and name.endswith((".h", ".inc")):
                copy(member, stage / "include" / name)
        for name in ("LICENSE", "ThirdPartyNotices.txt"):
            copy(name, stage / name)
        if rid == "linux-x64":
            copy(f"runtimes/{rid}/native/libonnxruntime.so", stage / "lib" / linux["real"])
            copy(f"runtimes/{rid}/native/libonnxruntime_providers_shared.so",
                 stage / "lib/libonnxruntime_providers_shared.so")
            (stage / "lib" / linux["soname"]).symlink_to(linux["real"])
            (stage / "lib/libonnxruntime.so").symlink_to(linux["soname"])
        else:
            for name in ("onnxruntime.dll", "onnxruntime.lib",
                         "onnxruntime_providers_shared.dll", "onnxruntime_providers_shared.lib"):
                copy(f"runtimes/{rid}/native/{name}", stage / "lib" / name)


def install(vendor, rid):
    pin = json.loads((vendor / "manifest.json").read_text(encoding="utf-8"))["deps"]["onnxruntime"]
    dest = vendor / "onnxruntime"
    required = ["include/onnxruntime_c_api.h", "include/onnxruntime_cxx_api.h"]
    if rid == "linux-x64":
        required += ["lib/libonnxruntime.so", f'lib/{pin["linux-x64"]["soname"]}']
    else:
        required += ["lib/onnxruntime.dll", "lib/onnxruntime.lib"]
    if dest.exists() or dest.is_symlink():
        if all((dest / entry).is_file() for entry in required):
            print(f"ONNX Runtime already present: {dest}")
            return
        raise ValueError(f"Incomplete ONNX Runtime prefix: {dest}; move it aside and run bootstrap again")
    print(f"Fetching ONNX Runtime {pin['version']} ({rid}) from NuGet", flush=True)
    with tempfile.TemporaryDirectory(prefix=".onnxruntime-", dir=vendor) as tmp:
        archive = Path(tmp) / "runtime.nupkg"
        stage = Path(tmp) / "install"
        stage.mkdir()
        download(pin, archive)
        extract(archive, stage, rid, pin.get("linux-x64"))
        if not all((stage / entry).is_file() for entry in required):
            raise ValueError("Pinned ONNX Runtime package is missing native headers or libraries")
        stage.rename(dest)


if __name__ == "__main__":
    try:
        rid = {"Linux": "linux-x64", "Windows": "win-x64"}.get(platform.system())
        if not rid or platform.machine().lower() not in ("x86_64", "amd64"):
            raise ValueError("ONNX Runtime bootstrap requires x64 Linux or Windows")
        install(Path(__file__).resolve().parent, rid)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        sys.exit(f"ONNX Runtime bootstrap: {error}")
