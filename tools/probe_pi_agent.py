#!/usr/bin/env python3
# 层：测试壳
"""Explicit fixed-Node wire acceptance. This does not test the C++ launcher or claim a sandbox."""
from __future__ import annotations
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time
from pi_supply_common import canonical, load_lock, verify_runtime


def frame(value):
    raw = canonical(value).rstrip(b"\n"); return struct.pack("<I", len(raw)) + raw


def read_frame(process):
    header = process.stdout.read(4)
    if len(header) != 4: raise ValueError("missing response header")
    size, = struct.unpack("<I", header)
    if not 0 < size <= 65536: raise ValueError("response byte budget exceeded")
    raw = process.stdout.read(size)
    if len(raw) != size: raise ValueError("truncated response")
    result = json.loads(raw.decode("utf-8"))
    if canonical(result).rstrip(b"\n") != raw: raise ValueError("noncanonical response")
    return result


def exercise(root, manifest_sha, *, loader=None):
    lock, _ = load_lock(); manifest = verify_runtime(root, manifest_sha, lock); root = Path(root).resolve()
    command = [str(root / manifest["node"]), "--no-addons", "--disable-proto=throw", "--no-warnings", str(root / manifest["entry"])]
    results = []
    with tempfile.TemporaryDirectory(prefix="pi-wire-") as private:
        environment = {"HOME": private, "USERPROFILE": private, "TEMP": private, "TMP": private, "LANG": "C.UTF-8"}
        if os.name == "nt": environment["SystemRoot"] = os.environ["SystemRoot"]
        if loader is not None: environment["LD_LIBRARY_PATH"] = str(loader)
        def launch():
            return subprocess.Popen(command, cwd=private, env=environment, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        process = launch(); started = time.monotonic()
        try:
            hello = {"protocol": "paleo.pi-agent.v1", "type": "hello", "sessionId": "wire_acceptance",
                     "generation": "18446744073709551615", "sequence": "1", "nonce": "a" * 32}
            # Pipe fragmentation has no influence on canonical identity.
            packet = frame(hello)
            for part in (packet[:1], packet[1:3], packet[3:17], packet[17:]): process.stdin.write(part); process.stdin.flush()
            with ThreadPoolExecutor(max_workers=1) as reader:
                pending = reader.submit(read_frame, process)
                try: response = pending.result(timeout=6)
                except BaseException: process.kill(); process.wait(timeout=3); raise
                expected = dict(hello, type="hello_result", upstreamCommit=lock["upstream"]["commit"], coreVersion=lock["coreVersion"],
                                nodeVersion=lock["nodeVersion"], toolExecution="sequential", modelTransport="host-only-unconnected", sandboxed=False)
                if response != expected: raise ValueError("peer identity mismatch")
                closing = dict(hello, type="close", sequence="2"); process.stdin.write(frame(closing)); process.stdin.flush()
                pending = reader.submit(read_frame, process)
                try: response = pending.result(timeout=6)
                except BaseException: process.kill(); process.wait(timeout=3); raise
                if response != dict(closing, type="closed"): raise ValueError("close identity mismatch")
            process.stdin.close(); process.wait(timeout=3)
            if process.returncode or process.stdout.read(1) or process.stderr.read(65537): raise ValueError("unexpected handshake exit/extra bytes")
            results.append({"case": "fragmented-hello-close-uint64", "code": 0, "elapsedMs": round(1000 * (time.monotonic() - started))})
        finally:
            if process.poll() is None: process.kill(); process.wait(timeout=3)
        for name, payload in [("eof", b""), ("partial-header", b"\x01"), ("zero-length", b"\0" * 4),
                              ("oversized-length", struct.pack("<I", 65537)), ("invalid-utf8", struct.pack("<I", 1) + b"\xff"),
                              ("duplicate-key", struct.pack("<I", 13) + b'{"a":1,"a":2}')]:
            process = launch()
            try:
                output, error = process.communicate(payload, timeout=7)
                if output or len(error) > 65536 or process.returncode != (0 if name == "eof" else 2):
                    raise ValueError("unexpected fixed-entry negative result: " + name)
                results.append({"case": name, "code": process.returncode})
            finally:
                if process.poll() is None: process.kill(); process.wait(timeout=3)
    return {"schema": "paleo.pi-wire-acceptance.v1", "platform": manifest["platform"], "manifestSha256": manifest_sha,
            "nodeVersion": lock["nodeVersion"], "isSandbox": False, "cppLauncherTested": False, "results": results}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-root", required=True, type=Path); parser.add_argument("--manifest-sha256", required=True)
    args = parser.parse_args(); print(json.dumps(exercise(args.runtime_root, args.manifest_sha256), indent=2))
