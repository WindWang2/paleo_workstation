#!/usr/bin/env python3
"""Record locally resolved dependency paths without changing the build or loader."""

import argparse
import hashlib
import json
import os
import platform
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

SCHEMA = "paleo.dependency-origins.v1"
PATH_TYPES = {"PATH", "FILEPATH"}
CACHE_LINE = re.compile(r"^([^#/:][^:]*):([^=]+)=(.*)$")
DEPENDENCY = re.compile(r"(?:_DIR|_INCLUDE(?:_DIRS?)?|_LIB(?:RARY|RARIES)?(?:_RELEASE|_DEBUG)?|_EXECUTABLE|_PREFIX|_ROOT)$", re.I)
TOOLS = re.compile(r"^(?:CMAKE_(?:C|CXX|RC|ASM)_COMPILER|CMAKE_(?:LINKER|AR|RANLIB|MAKE_PROGRAM|MT)|PALEO_(?:PYTHON3|TEST_PYTHON|PWSH|CCACHE_EXE)|Python3_EXECUTABLE)$")
WINDOWS_API_SET_PREFIXES = ("api-ms-win-", "ext-ms-win-")
LIBRARY = re.compile(r"\.(?:so(?:\.[\w.-]+)?|a|dylib|dll|lib)$", re.I)


def run(args, cwd=None):
    return subprocess.run(args, cwd=cwd, check=True, capture_output=True,
                          text=True, encoding="utf-8", errors="replace",
                          timeout=60).stdout.strip()


def parts(name):
    label, sep, value = name.partition("=")
    if not sep or not label or not value:
        raise ValueError(f"expected LABEL=VALUE, got {name!r}")
    return label, value


def path_key(path):
    return os.path.normcase(str(path))


class Report:
    def __init__(self, prefixes, abi):
        self.prefixes = sorted(((name, Path(path).resolve(strict=True)) for name, path in prefixes),
                               key=lambda entry: (-len(entry[1].parts), entry[0]))
        if any(root == Path(root.anchor) or not root.is_dir() for _, root in self.prefixes):
            raise ValueError("project prefixes must be existing, non-root directories")
        self.abi = {name.casefold(): Path(path).resolve(strict=True) for name, path in abi}
        for name, path in self.abi.items():
            if not path.is_file() or not name or "/" in name or "\\" in name:
                raise ValueError(f"ABI entry must name the exact file: {name}={path}")
        self.rows = {}

    def add(self, stage, consumer, name, value=None, tool=False, api_set=False):
        path = Path(value).resolve() if value else None
        prefix = None
        if not path or not path.exists():
            origin = "api-set-unresolved" if api_set else "missing"
        elif (pinned := self.abi.get(name.casefold())) and path == pinned:
            origin = "host-abi"
        else:
            prefix = next((label for label, root in self.prefixes if path.is_relative_to(root)), None)
            origin = "project" if prefix else "toolchain" if tool else "outside"
        versions = re.search(r"\.so\.(\d[\d.]*)$", path.name) if path else None
        row = dict(stage=stage, consumer=consumer, name=name, origin=origin,
                   libraryVersion=versions.group(1) if versions else None,
                   path=str(path) if path else None, observedPath=value, prefix=prefix)
        key = (stage, consumer, name, path_key(path) if path else "")
        self.rows[key] = row

    def cache(self, build):
        for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
            match = CACHE_LINE.match(line)
            if not match:
                continue
            name, kind, values = match.groups()
            tool = bool(TOOLS.fullmatch(name))
            if (kind not in PATH_TYPES and not tool) or not (tool or DEPENDENCY.search(name)):
                continue
            if name.startswith(("CMAKE_INSTALL_", "CMAKE_FIND_PACKAGE_REDIRECTS", "FETCHCONTENT_")):
                continue
            for value in values.split(";"):
                if value.endswith("-NOTFOUND"):
                    self.add("configure", "CMakeCache", name)
                elif Path(value).is_absolute():
                    self.add("configure", "CMakeCache", name, value, tool)

    def file_api(self, build):
        reply = build / ".cmake/api/v1/reply"
        indexes = sorted(reply.glob("index-*.json"))
        if not indexes:
            return False
        index = json.loads(indexes[-1].read_text(encoding="utf-8"))
        entry = index.get("reply", {}).get("codemodel-v2")
        if not entry:
            return False
        model = json.loads((reply / entry["jsonFile"]).read_text(encoding="utf-8"))
        for config in model["configurations"]:
            for target in config["targets"]:
                data = json.loads((reply / target["jsonFile"]).read_text(encoding="utf-8"))
                caller = target["name"]
                for group in data.get("compileGroups", []):
                    for include in group.get("includes", []):
                        path = Path(include["path"])
                        if path.is_absolute():
                            self.add("compile-include", caller, path.name, str(path))
                for fragment in data.get("link", {}).get("commandFragments", []):
                    if fragment["role"] != "libraries":
                        continue
                    path = Path(fragment["fragment"].strip('"'))
                    if path.is_absolute() and LIBRARY.search(path.name):
                        self.add("link", caller, path.name, str(path))
        return True

    def elf(self, binary):
        # ldd is intentionally restricted to explicit, caller-built artifacts.
        binary = binary.resolve(strict=True)
        with binary.open("rb") as source:
            if source.read(4) != b"\x7fELF":
                raise ValueError(f"not an ELF file: {binary}")
        consumer = str(binary)
        try:
            listing = run(["ldd", str(binary)])
        except subprocess.CalledProcessError as error:
            raise ValueError(f"loader could not inspect {binary}: {error.stderr.strip() or error.stdout.strip()}") from error
        for line in listing.splitlines():
            name, sep, value = line.strip().partition(" => ")
            if sep and value.startswith("not found"):
                self.add("loader-resolution", consumer, Path(name).name)
            elif sep and value.startswith("/"):
                self.add("loader-resolution", consumer, Path(name).name, value.rsplit(" (", 1)[0])
            elif name.startswith("/"):
                value = name.rsplit(" (", 1)[0]
                self.add("loader-resolution", consumer, Path(value).name, value)

    def pe(self, binary, dumpbin, roots):
        binary = binary.resolve(strict=True)
        queue = [binary]
        seen = set()
        while queue:
            current = queue.pop()
            if current in seen:
                continue
            seen.add(current)
            listing = run([dumpbin, "/nologo", "/dependents", str(current)])
            for name in sorted(set(re.findall(r"^ {4}([^\s]+\.dll)\s*$", listing, re.M | re.I))):
                match = next((root / name for root in (binary.parent, *roots)
                              if (root / name).is_file()), None)
                if not match:
                    match = self.abi.get(name.casefold())
                api_set = name.casefold().startswith(WINDOWS_API_SET_PREFIXES)
                self.add("pe-import-search", str(current), name, str(match) if match else None, api_set=api_set)
                if match and not (self.abi.get(name.casefold()) == match.resolve()):
                    queue.append(match.resolve())

    def loaded(self, consumer, manifest):
        # A normalized list exported from a local process inspection, one path per line.
        for line in manifest.read_text(encoding="utf-8-sig").splitlines():
            if not line.strip():
                continue
            path = Path(line.strip())
            if not path.is_absolute():
                raise ValueError(f"{manifest}: loaded-module path is not absolute: {line!r}")
            self.add("process", consumer, path.name, str(path))


def audit(args):
    report = Report(map(parts, args.prefix), map(parts, args.abi))
    build = args.build.resolve(strict=True)
    report.cache(build)
    file_api_available = report.file_api(build)
    for binary in args.elf:
        report.elf(binary)
    for binary in args.pe:
        if not args.dumpbin:
            raise ValueError("--pe requires an explicit --dumpbin path")
        report.pe(binary, args.dumpbin, [p.resolve(strict=True) for p in args.dll_dir])
    for specification in args.loaded:
        consumer, manifest = parts(specification)
        report.loaded(consumer, Path(manifest))
    cwd = args.repository.resolve(strict=True)
    locks = {p: hashlib.sha256((cwd / p).read_bytes()).hexdigest() for p in args.lock}
    entries = sorted(report.rows.values(), key=lambda x: (x["stage"], x["consumer"],
                                                          x["name"], x["path"] or ""))
    compilers = []
    for file in sorted((build / "CMakeFiles").glob("*/CMake*Compiler.cmake")):
        fields = {k: v for k, v in re.findall(
            r'^set\((CMAKE_(?:C|CXX)_COMPILER(?:_ID|_VERSION)?) "([^"]*)"\)',
            file.read_text(encoding="utf-8"), re.M)}
        if fields:
            compilers.append(fields)
    metadata = dict(commit=run(["git", "rev-parse", "HEAD"], cwd=cwd),
                    sourceTree=run(["git", "rev-parse", "HEAD^{tree}"], cwd=cwd),
                    dirty=bool(run(["git", "status", "--porcelain", "-uno"], cwd=cwd)),
                    os=platform.platform(), route=args.route,
                    locks=locks, fileApi=file_api_available, compilers=compilers)
    return dict(schema=SCHEMA, metadata=metadata, counts=dict(Counter(r["origin"] for r in entries)),
                entries=entries)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, default=Path.cwd())
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--route", required=True)
    parser.add_argument("--prefix", action="append", default=[], metavar="LABEL=DIR")
    parser.add_argument("--abi", action="append", default=[], metavar="NAME=FILE")
    parser.add_argument("--lock", action="append", default=[])
    parser.add_argument("--elf", action="append", type=Path, default=[], metavar="LOCAL_BINARY")
    parser.add_argument("--pe", action="append", type=Path, default=[])
    parser.add_argument("--dumpbin", type=Path)
    parser.add_argument("--dll-dir", action="append", type=Path, default=[])
    parser.add_argument("--loaded", action="append", default=[], metavar="CONSUMER=PATH_LIST")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        report = audit(args)
        with args.output.open("x", encoding="utf-8") as output:
            json.dump(report, output, ensure_ascii=False, indent=2, sort_keys=True)
            output.write("\n")
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, f"dependency audit: {error}\n")


if __name__ == "__main__":
    main()
