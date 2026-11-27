"""Populate the Pi build cache from the repository lock using native Windows TLS."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from pi_supply_common import check_archive, fetch, linked, load_lock
from prepare_agent_python import StoreLock


def fetch_windows(cache, lock, curl):
    for name, pin in lock["archives"].items():
        target = cache / name
        if target.exists() or linked(target):
            check_archive(target, pin)
            continue
        with tempfile.NamedTemporaryFile(dir=cache, suffix=".part", delete=False) as handle:
            part = Path(handle.name)
        try:
            subprocess.run([
                str(curl), "--disable", "--proto", "=https", "--fail", "--silent", "--show-error",
                "--retry", "3", "--connect-timeout", "12", "--max-time", "420",
                "--max-filesize", str(pin["size"]), "--output", str(part), pin["url"],
            ], check=True)
            check_archive(part, pin)
            part.replace(target)
        finally:
            part.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", required=True, type=Path)
    cache = parser.parse_args().cache
    if linked(cache) or linked(cache / ".prepare.lock"):
        parser.error("linked Pi supply cache/lock")
    cache.mkdir(parents=True, exist_ok=True)
    lock, _ = load_lock()
    with StoreLock(cache):
        if os.name == "nt":
            curl = Path(os.environ["SystemRoot"]) / "System32/curl.exe"
            if curl.is_file():
                fetch_windows(cache, lock, curl)
                return
        fetch(cache, lock)


if __name__ == "__main__":
    main()
