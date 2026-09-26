#!/usr/bin/env python3
"""Resolve apt --print-uris entries to apt's signed SHA256 package metadata."""

import os
import re
import subprocess
import sys
from urllib.parse import unquote, urlparse


def available_hashes():
    records = subprocess.check_output(["apt-cache", "dumpavail"], text=True)
    hashes = {}
    for paragraph in records.split("\n\n"):
        fields = {}
        for line in paragraph.splitlines():
            if ": " in line and not line.startswith(" "):
                key, value = line.split(": ", 1)
                fields[key] = value
        if "Filename" in fields and "SHA256" in fields:
            name = os.path.basename(fields["Filename"])
            hashes.setdefault(name, set()).add(fields["SHA256"])
    return hashes


def main():
    hashes = available_hashes()
    pattern = re.compile(r"^'([^']+)'\s+(\S+\.deb)\s+(\d+)\s+\S+$")
    count = 0
    for line in sys.stdin:
        match = pattern.fullmatch(line.strip())
        if not match:
            continue
        url, name, size = match.groups()
        archive_name = unquote(os.path.basename(urlparse(url).path))
        candidates = hashes.get(archive_name, set())
        if len(candidates) != 1:
            raise SystemExit(f"missing or ambiguous apt SHA256 for {archive_name}")
        digest = next(iter(candidates))
        if not re.fullmatch(r"[a-f0-9]{64}", digest):
            raise SystemExit(f"invalid apt SHA256 for {archive_name}")
        print(f"'{url}' {name} {size} SHA256:{digest}")
        count += 1
    if count == 0:
        raise SystemExit("apt resolved no package URI entries")


if __name__ == "__main__":
    main()
