#!/usr/bin/env python3
# 层：测试壳
"""Validate the optional Pi supply closure without discovering or starting Node."""
from __future__ import annotations
import argparse
from pathlib import Path
import re
import sys
from pi_supply_common import ROOT, VENDOR, digest, hex_digest, load_lock, read_json, relative, verify_runtime


def check_sources(lock):
    inventory, _ = read_json(VENDOR / lock["upstream"]["sourceFiles"])
    if set(inventory) != {"schema", "commit", "files"} or inventory["schema"] != "paleo.pi-source-files.v1" or inventory["commit"] != lock["upstream"]["commit"]:
        raise ValueError("source inventory identity mismatch")
    rows = {}; total = 0; folded = set()
    for row in inventory["files"]:
        if set(row) != {"path", "size", "sha256"} or type(row["size"]) is not int or row["size"] < 1 or not hex_digest(row["sha256"]):
            raise ValueError("invalid source file identity")
        name = relative(row["path"])
        if name.casefold() in folded:
            raise ValueError("duplicate source file")
        if name != "LICENSE" and name != "package-lock.json" and not any(name.startswith("packages/" + p + "/") for p in ("agent", "ai", "schema", "telemetry")):
            raise ValueError("unreviewed source package")
        folded.add(name.casefold()); rows[name] = row; total += row["size"]
    if not rows or len(rows) > lock["limits"]["maxSourceFiles"] or total > lock["limits"]["maxSourceBytes"]:
        raise ValueError("selected source budget exceeded")
    if rows["package-lock.json"]["sha256"] != lock["upstream"]["packageLockSha256"] or rows["LICENSE"]["sha256"] != digest(VENDOR / "LICENSE.upstream"):
        raise ValueError("upstream lock/license mismatch")
    graph, _ = read_json(VENDOR / lock["build"]["graph"])
    if set(graph) != {"schema", "esbuildVersion", "inputs", "externalImports"} or graph["schema"] != "paleo.pi-bundle-graph.v1" or graph["esbuildVersion"] != lock["build"]["esbuildVersion"] or graph["externalImports"]:
        raise ValueError("bundle graph is not a closed reviewed build")
    for name, usage in graph["inputs"].items():
        if name != "paleo/sidecar.mjs" and name not in {"paleo/" + module for module in lock["build"].get("localModules", {})} and name not in rows:
            raise ValueError("bundle input is not source-pinned")
        if set(usage) != {"bytesInOutput"} or type(usage["bytesInOutput"]) is not int or usage["bytesInOutput"] <= 0:
            raise ValueError("invalid retained bundle input")
        if name.startswith("packages/ai/src/") and any(part in name for part in ("/api", "/providers/", "/auth", "/compat", "/env-api-keys", "/models", "/images")):
            raise ValueError("provider/auth/network implementation retained in bundle")
    notices = {row["path"] for row in lock["notices"]}
    actual = {"LICENSE.upstream"} | {p.relative_to(VENDOR).as_posix() for p in (VENDOR / "THIRD_PARTY_NOTICES").rglob("*") if p.is_file()}
    if notices != actual:
        raise ValueError("notice inventory differs from source lock")
    return len(rows), len(graph["inputs"])


def check_boundary():
    entry = (VENDOR / "sidecar.mjs").read_text("utf-8")
    conversation = (VENDOR / "conversation.mjs").read_text("utf-8") if (VENDOR / "conversation.mjs").is_file() else ""
    active = entry + conversation
    for forbidden in ("pi-coding-agent", "createAgentSession", "node:fs", "node:child_process", "node:net", "node:http", "node:tls", "node:dns", "node:worker_threads", "process.env", "eval(", "import(", "fetch(", "WebSocket("):
        if forbidden in active:
            raise ValueError("unreviewed fixed-entry capability: " + forbidden)
    for required in ('toolExecution: "sequential"', 'tools: []', 'pi_model_transport_unconnected'):
        if required not in entry:
            raise ValueError("fixed-entry boundary missing: " + required)
    if conversation:
        for required in ('"paleo.pi-conversation.v1"', '"tool_execution_start"', 'toolExecution: "sequential"', '"model_request"', '"tool_request"'):
            if required not in conversation:
                raise ValueError("conversation boundary missing: " + required)
    cmake = (ROOT / "cmake/extra-pi-agent.cmake").read_text("utf-8")
    for required in ("paleo_services_piagent", "EXCLUDE_FROM_ALL", "AgentPiRuntime", "pi_agent", "pi_agent_selftest"):
        if required not in cmake:
            raise ValueError("optional CMake boundary missing: " + required)
    if "extra-pi-agent.cmake" not in (ROOT / "CMakeLists.txt").read_text("utf-8"):
        raise ValueError("Pi source target is not registered")
    if "find_program(" in cmake or "execute_process(" in cmake:
        raise ValueError("Pi configure must not discover/run Node")
    installs = re.findall(r"install\s*\(([^)]*)\)", cmake)
    if len(installs) != 1 or not re.search(r"SCRIPT\s+[^\n]+\s+COMPONENT\s+AgentPiRuntime\s+EXCLUDE_FROM_ALL", installs[0]):
        raise ValueError("Pi active-JS install must be one explicit excluded component")
    guard = (ROOT / "cmake/install-pi-agent.cmake.in").read_text("utf-8")
    if re.search(r"execute_process\s*\(|file\s*\(\s*(DOWNLOAD|UPLOAD)\b", guard, re.IGNORECASE):
        raise ValueError("Pi installer must not execute Node or access the network")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--runtime-root", type=Path)
    parser.add_argument("--manifest-sha256")
    args = parser.parse_args()
    lock, anchor = load_lock(); sources, inputs = check_sources(lock); check_boundary()
    if bool(args.runtime_root) != bool(args.manifest_sha256):
        parser.error("--runtime-root and --manifest-sha256 must be supplied together")
    if args.runtime_root:
        verify_runtime(args.runtime_root, args.manifest_sha256, lock)
    if args.selftest:
        from pi_supply_tests import run
        run(lock)
    print(f"Pi supply: OK ({sources} pinned source files, {inputs} retained inputs, zero external imports; sourceLock={anchor})")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError) as error:
        print("Pi supply check failed: " + str(error), file=sys.stderr)
        raise SystemExit(1)
