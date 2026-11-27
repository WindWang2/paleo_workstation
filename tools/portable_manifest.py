"""Track the source and byte identity of files in a portable runtime."""

import hashlib
import json
from pathlib import Path
import shutil


REPO_ROOT = Path(__file__).resolve().parent.parent
MANIFEST_NAME = "RUNTIME_SOURCES.json"
PI_LOCKS = ("vendor/pi/source.lock.json", "vendor/pi/runtime-manifests.lock.json")


def verified_pi_generation(prefix: Path, platform: str) -> Path:
    from pi_supply_common import load_lock, verify_runtime

    anchors = json.loads((REPO_ROOT / PI_LOCKS[1]).read_text(encoding="utf-8"))
    anchor = anchors[platform]
    root = prefix / "libexec/pi-agent/generations" / anchor
    lock, _ = load_lock()
    verify_runtime(root, anchor, lock)
    return root


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def load_cmake_cache(build: Path) -> dict[str, str]:
    cache = {}
    for entry in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if entry.startswith(("#", "//")):
            continue
        key, sep, value = entry.partition("=")
        if sep:
            cache[key.partition(":")[0]] = value
    return cache


def verify_receipt(prefix: Path, receipt: str, lock: str, normalize=False):
    data = (REPO_ROOT / lock).read_bytes()
    if normalize:
        data = data.replace(b"\r\n", b"\n")
    expected = hashlib.sha256(data).hexdigest()
    path = prefix / receipt
    if not path.is_file() or path.read_text(encoding="ascii").strip() != expected:
        raise ValueError(f"{prefix} does not match {lock}. Prepare the locked dependency prefix first.")


class PackageFiles:
    def __init__(self, output: Path):
        self.output = output.resolve()
        self.origins = {}

    def copy(self, source: Path, destination: str, origin: str, source_root: Path) -> Path:
        source_root = source_root.resolve(strict=True)
        resolved = source.resolve(strict=True)
        if not resolved.is_relative_to(source_root):
            raise ValueError(f"Package source leaves {origin} prefix: {source} -> {resolved}")
        path = self.output / destination
        if not path.is_relative_to(self.output) or ".." in path.relative_to(self.output).parts:
            raise ValueError(f"Invalid package path: {destination}")
        path.parent.mkdir(parents=True, exist_ok=True)
        checksum = hashlib.sha256()
        with path.open("xb") as output, resolved.open("rb") as content:
            for block in iter(lambda: content.read(1024 * 1024), b""):
                output.write(block)
                checksum.update(block)
        shutil.copystat(resolved, path)
        sha = checksum.hexdigest()
        self.origins[path.relative_to(self.output).as_posix()] = {
            "origin": origin,
            "source": resolved.relative_to(source_root).as_posix(),
            "sourceSha256": sha,
            "sha256": sha,
            "size": path.stat().st_size,
        }
        return path

    def tree(self, source: Path, destination: str, origin: str, source_root: Path, include=None):
        if not source.is_dir():
            raise ValueError(f"Required package tree is missing: {source}")
        for selected in sorted(source.rglob("*")):
            if selected.is_file() and (include is None or include(selected)):
                relative = selected.relative_to(source).as_posix()
                self.copy(selected, f"{destination}/{relative}", origin, source_root)

    def write(self, locks: list[str], rewritten=()):
        rewritten = set(rewritten)
        for entry in locks:
            self.copy(REPO_ROOT / entry, f"share/paleo/supply/{entry}", "source", REPO_ROOT)
        for installed in sorted(self.output.rglob("*")):
            if not installed.is_file():
                continue
            name = installed.relative_to(self.output).as_posix()
            row = self.origins.setdefault(name, {"origin": "package"})
            if installed.is_symlink() and not installed.resolve().is_relative_to(self.output):
                raise ValueError(f"Installed runtime link leaves the package: {name}")
            if installed in rewritten or installed.is_symlink() or "sha256" not in row:
                row.update(size=installed.stat().st_size, sha256=digest(installed))
        (self.output / MANIFEST_NAME).write_text(
            json.dumps({"schema": "paleo.runtime-sources.v1", "files": self.origins},
                       indent=2, sort_keys=True, ensure_ascii=False) + "\n", encoding="utf-8")


def verify(package: Path):
    package = package.resolve(strict=True)
    data = json.loads((package / MANIFEST_NAME).read_text(encoding="utf-8"))
    if data.get("schema") != "paleo.runtime-sources.v1":
        raise ValueError("Unsupported portable runtime manifest")
    for path in package.rglob("*"):
        if (path.is_file() and path.relative_to(package).as_posix() not in data["files"]
                and (path.suffix.lower() in {".dll", ".exe", ".so"} or ".so." in path.name)):
            raise ValueError(f"Untracked runtime library or executable: {path.relative_to(package)}")
    for name, row in data["files"].items():
        relative = Path(name)
        path = (package / relative).resolve()
        if relative.is_absolute() or ".." in relative.parts or not path.is_relative_to(package):
            raise ValueError(f"Runtime file leaves the package: {name}")
        if not path.is_file():
            raise ValueError(f"Missing runtime file: {name}")
        if path.stat().st_size != row["size"] or digest(path) != row["sha256"]:
            raise ValueError(f"Runtime file failed integrity validation: {name}")
