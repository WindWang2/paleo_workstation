"""Identify the source recipe shared by the superbuild, consumers and packager."""
import argparse
import hashlib
import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
COMPLETE = ".paleo-superbuild-source.sha256"
INSTANCE = ".paleo-superbuild-instance"


def inputs():
    recipe = ["CMakeLists.txt", "debian13-base.lock", "debian-sources.lock",
              "closed-dependencies.cmake", "prepare-prefix.sh", "prepare-debian-source.cmake", "source_receipt.py"]
    return [ROOT / name for name in recipe] + sorted((ROOT / "patches").glob("*.*"))


def fingerprint():
    result = hashlib.sha256()
    for path in inputs():
        content = path.read_bytes().replace(b"\r\n", b"\n")
        result.update(path.relative_to(ROOT).as_posix().encode("utf-8") + b"\0")
        result.update(hashlib.sha256(content).digest())
    return result.hexdigest()


def require(prefix, expected, build_only=False):
    identity = (prefix / INSTANCE).read_text(encoding="ascii").splitlines()
    if len(identity) != 3 or identity[0] != expected:
        raise ValueError(f"Source recipe changed for {prefix}. Prepare a new build and prefix.")
    if not build_only and (prefix / COMPLETE).read_text(encoding="ascii").splitlines() != identity:
        raise ValueError(f"Incomplete or stale source prefix: {prefix}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("digest", "prepared", "verify", "finish", "package-inputs"))
    parser.add_argument("prefix", type=Path, nargs="?")
    parser.add_argument("--build", type=Path)
    args = parser.parse_args()
    expected = fingerprint()
    if args.action == "digest":
        print(expected)
        return
    if not args.prefix:
        parser.error("a source prefix is required")
    require(args.prefix, expected, build_only=args.action in ("prepared", "finish"))
    if args.action == "finish":
        if not args.build or (args.build / INSTANCE).read_bytes() != (args.prefix / INSTANCE).read_bytes():
            raise ValueError("This build did not prepare the selected source prefix")
        with tempfile.NamedTemporaryFile(dir=args.prefix, delete=False) as temp:
            temp.write((args.prefix / INSTANCE).read_bytes())
        os.replace(temp.name, args.prefix / COMPLETE)
    elif args.action == "package-inputs":
        for path in inputs():
            print(path.relative_to(ROOT.parent.parent).as_posix())


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError) as error:
        sys.exit(f"Source prefix verification failed: {error}")
