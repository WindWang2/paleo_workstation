"""Record the installed payload after a clean, archive-verified OSGeo4W setup."""
import argparse
import json
import os
import sys
import tempfile
from pathlib import Path

from osgeo4w_offline import LOCK, REPO, digest

RECEIPT = ".paleo-prefix-files.json"


def snapshot(prefix, native):
    prefix = prefix.resolve(strict=True)
    locks = {"osgeo4w": digest(LOCK), "profile": "native" if native else "desktop"}
    deps = json.loads((REPO / "vendor/manifest.json").read_text(encoding="utf-8"))["deps"]
    locks["zstd"] = deps["zstd-source"]["sha256"]
    if not native:
        locks["opencl"] = digest(REPO / "vendor/opencl-source.lock.json")
    files = {}
    def walk_error(error):
        raise error
    for directory, children, names in os.walk(prefix, onerror=walk_error):
        folder = Path(directory)
        if folder == prefix and "var" in children:
            children.remove("var")  # Installer logs are mutable.
        if "__pycache__" in children:
            children.remove("__pycache__")
        for name in children:
            child = folder / name
            if child.resolve(strict=True) != child:
                raise ValueError(f"Unexpected directory link in OSGeo4W prefix: {child}")
        for name in names:
            path = folder / name
            relative = path.relative_to(prefix).as_posix()
            if relative in (RECEIPT, ".paleo-osgeo4w-lock.sha256"):
                continue
            if path.is_symlink() or not path.is_file():
                raise ValueError(f"Unexpected OSGeo4W file: {path}")
            files[relative] = digest(path)
    return {"schema": 1, "locks": locks, "files": files}


def check(prefix, native, record=False):
    receipt = prefix / RECEIPT
    if not record and not receipt.is_file():
        raise ValueError(f"Missing {receipt}. Move the legacy prefix aside and bootstrap from verified archives.")
    current = snapshot(prefix, native)
    if record:
        if receipt.exists():
            raise ValueError(f"Prefix already recorded: {receipt}")
        with tempfile.NamedTemporaryFile(dir=prefix.parent, delete=False, mode="w", encoding="utf-8") as temp:
            json.dump(current, temp, sort_keys=True)
        try:
            Path(temp.name).replace(receipt)
        finally:
            Path(temp.name).unlink(missing_ok=True)
    else:
        expected = json.loads(receipt.read_text(encoding="utf-8"))
        if expected.get("schema") != 1 or expected.get("locks") != current["locks"]:
            raise ValueError(f"OSGeo4W prefix inputs changed: {receipt}")
        files = expected["files"]
        for name in sorted(files.keys() | current["files"].keys()):
            if files.get(name) != current["files"].get(name):
                raise ValueError(f"OSGeo4W prefix file mismatch: {prefix / name}")
    print(f"Verified {len(current['files'])} OSGeo4W prefix files")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("record", "verify"))
    parser.add_argument("prefix", type=Path)
    parser.add_argument("--native", action="store_true")
    args = parser.parse_args()
    try:
        check(args.prefix, args.native, args.action == "record")
    except (OSError, KeyError, ValueError) as error:
        sys.exit(str(error))
