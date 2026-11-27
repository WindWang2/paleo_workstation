"""Build the PDAL Arrow reader and writer against the locked Windows Arrow and GDAL ABI."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

from osgeo4w_offline import REPO, digest, fetch, manifest, validate_file
from pin_osgeo4w import installed_db_map

LOCK = REPO / "vendor/pdal-arrow-source.lock.json"
PATCH = REPO / "vendor/patches/pdal-arrow26.patch"
STAMP = ".paleo-pdal-arrow-source.sha256"
PLUGINS = {kind: f"libpdal_plugin_{kind.lower()}_arrow.dll" for kind in ("Reader", "Writer")}


def prepare(cache, offline):
    pin = json.loads(LOCK.read_text(encoding="utf-8"))
    versions = manifest()["closure"]
    if any(versions.get(name) != version for name, version in pin["packages"].items()):
        raise ValueError("PDAL Arrow source pins do not match the Windows package versions")
    patch = PATCH.read_text(encoding="utf-8").encode()
    if hashlib.sha256(patch).hexdigest() != pin["patch_sha256"]:
        raise ValueError("PDAL Arrow compatibility patch digest changed")
    for row in pin["sources"] + pin["sdk"]:
        dest = cache / Path(row["path"]).name
        if not dest.exists() and not offline:
            fetch(row["url"], dest, row)
        validate_file(dest, row)
    return pin, patch


def build(prefix, cache, pin, patch, force=False):
    versions = installed_db_map((prefix / "etc/setup/installed.db").read_text(encoding="utf-8"))
    if any(versions.get(name) != version for name, version in pin["packages"].items()):
        raise ValueError("The installed PDAL/Arrow/GDAL packages differ from the source build pins")
    output = prefix / "bin"
    stamp = prefix / STAMP
    identity = digest(LOCK)
    if not force and stamp.exists() and stamp.read_text(encoding="ascii").strip() == identity:
        if all((output / dll).is_file() for dll in PLUGINS.values()):
            return
    # Build and link both modules before replacing either installed plugin.
    with tempfile.TemporaryDirectory(prefix=".paleo-pdal-", dir=prefix.parent) as tmp:
        work = Path(tmp)
        sdk = work / "sdk"
        for row in pin["sdk"]:
            with tarfile.open(cache / row["path"]) as archive:
                for member in archive:
                    path = Path(member.name)
                    if (member.isfile() and path.parts and path.parts[0] in ("include", "lib")
                            and ".." not in path.parts):
                        dest = sdk / path
                        dest.parent.mkdir(parents=True, exist_ok=True)
                        with archive.extractfile(member) as src, dest.open("wb") as dst:
                            shutil.copyfileobj(src, dst)
        for row in pin["sources"]:
            dest = work / row["path"]
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(cache / dest.name, dest)
        subprocess.run(["git", "apply", "--no-index", "--whitespace=nowarn", "-"], cwd=work,
                       input=patch, check=True)
        flags = ["/nologo", "/EHsc", "/std:c++20", "/Zc:__cplusplus", "/permissive-", "/O2", "/MD",
                 "/bigobj", "/DNOMINMAX", "/DNDEBUG", "/DWIN32", "/D_WINDOWS", "/utf-8", "/LD",
                 f"/I{sdk / 'include'}", f"/I{sdk / 'include/pdal'}", f"/I{prefix / 'include'}",
                 f"/I{work / 'vendor/nlohmann'}"]
        for kind, dll in PLUGINS.items():
            subprocess.run(["cl", *flags, f"plugins/arrow/io/Arrow{kind}.cpp", "/link", "/Brepro",
                            f"/OUT:{dll}", f"/IMPLIB:{kind}.lib", f"/LIBPATH:{sdk / 'lib'}",
                            f"/LIBPATH:{prefix / 'lib'}", "pdalcpp.lib", "arrow.lib", "parquet.lib",
                            "gdal_i.lib", "ws2_32.lib"], cwd=work, check=True)
        for dll in PLUGINS.values():
            (work / dll).replace(output / dll)
        staged = work / "receipt"
        staged.write_text(identity + "\n", encoding="ascii")
        staged.replace(stamp)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("prepare", "build", "upgrade-prefix"))
    parser.add_argument("prefix", type=Path)
    parser.add_argument("--cache", type=Path, default=REPO / "vendor/cache/tarballs/pdal-arrow")
    args = parser.parse_args()
    pins, source_patch = prepare(args.cache, os.environ.get("PALEO_VENDOR_OFFLINE") == "1")
    if args.action == "build":
        build(args.prefix.resolve(strict=True), args.cache, pins, source_patch)
    elif args.action == "upgrade-prefix":
        from check_pdal_arrow import check
        from osgeo4w_prefix import upgrade_legacy_pdal
        prefix = args.prefix.resolve(strict=True)
        def rebuild():
            build(prefix, args.cache, pins, source_patch, force=True)
            check(prefix)
        upgrade_legacy_pdal(prefix, rebuild)
