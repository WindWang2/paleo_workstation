#!/usr/bin/env python3
"""P3 本地测试调度器：系统 ctest 4.5 与主机 libxml2 ABI 不匹配（缺
XML_GetCurrentColumnNumber64，与仓库代码无关），本脚本按 CTestTestfile.cmake
语义（subdirs/add_test/set_tests_properties ENVIRONMENT + LABELS）直接驱动测试。

用法：python3 tools/run_tests_without_ctest.py [-LE core] [-R regex] [-j N] [build_dir]
退出码：0=全过；1=有失败；2=用法/环境错误。
"""
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

PREFIX = os.environ.get("PALEO_TEST_PREFIX", "/home/kevin/projects/paleo-deb-prefix-20261119/usr")


def parse_tests(build_dir):
    """返回 [(name, command_list, labels_set, env_dict)]，含子目录递归。"""
    tests = []
    seen = set()

    def env_from_cmake(raw):
        env = {}
        for item in raw.strip().strip('"').split(";"):
            if not item or "=" not in item:
                continue
            k, v = item.split("=", 1)
            env[k] = v
        return env

    def load(path, base):
        if path in seen or not os.path.exists(path):
            return
        seen.add(path)
        text = open(path, encoding="utf-8").read()
        props = {}
        for m in re.finditer(r'set_tests_properties\("([^"]+)" PROPERTIES\s+(.*?)\s+_BACKTRACE', text, re.S):
            name, body = m.group(1), m.group(2)
            labels = re.search(r'LABELS "([^"]*)"', body)
            env = re.search(r'ENVIRONMENT "([^"]*)"', body)
            props[name] = (
                set(labels.group(1).split(";")) if labels and labels.group(1) else set(),
                env_from_cmake(env.group(1)) if env else {},
            )
        for m in re.finditer(r'add_test\("([^"]+)"\s+(.*?)\)\s*\n', text, re.S):
            name, args = m.group(1), m.group(2)
            cmds = re.findall(r'"([^"]*)"', args)
            labels, env = props.get(name, (set(), {}))
            tests.append((name, cmds, labels, env))
        for m in re.finditer(r'subdirs\("([^"]+)"\)', text):
            sub = os.path.join(base, m.group(1), "CTestTestfile.cmake")
            load(sub, os.path.dirname(sub))

    root = os.path.join(build_dir, "CTestTestfile.cmake")
    load(root, build_dir)
    return tests


def main(argv):
    args = list(argv[1:])
    exclude, include, jobs = [], [], 1
    build_dir = None
    i = 0
    while i < len(args):
        a = args[i]
        if a == "-LE":
            exclude = args[i + 1].split("|"); i += 2
        elif a == "-L":
            include = args[i + 1].split("|"); i += 2
        elif a == "-R":
            include = ["__regex__:" + args[i + 1]]; i += 2
        elif a == "-j":
            jobs = max(1, int(args[i + 1])); i += 2
        else:
            build_dir = a; i += 1
    build_dir = build_dir or "build"
    if not os.path.isdir(build_dir):
        print(f"build dir not found: {build_dir}", file=sys.stderr)
        return 2

    tests = parse_tests(build_dir)
    selected = []
    for name, cmds, labels, env in tests:
        if labels & set(exclude):
            continue
        if include:
            matched = any(lab in labels for lab in include if not lab.startswith("__regex__:"))
            if not matched:
                rx = next((l[len("__regex__:"):] for l in include if l.startswith("__regex__:")), None)
                if rx:
                    matched = re.search(rx, name) is not None
            if not matched:
                continue
        selected.append((name, cmds, labels, env))

    print(f"selected {len(selected)} / {len(tests)} tests (jobs={jobs})", flush=True)

    base_env = dict(os.environ)
    base_env["QT_QPA_PLATFORM"] = "offscreen"
    base_env["QGIS_PREFIX_PATH"] = PREFIX
    ma = os.path.join(PREFIX, "lib/x86_64-linux-gnu")
    base_env["LD_LIBRARY_PATH"] = f"{ma}:{PREFIX}/lib:" + base_env.get("LD_LIBRARY_PATH", "")
    base_env["QT_PLUGIN_PATH"] = os.path.join(ma, "qt6/plugins")
    base_env["GDAL_DATA"] = f"{PREFIX}/share/gdal"
    base_env["PROJ_LIB"] = f"{PREFIX}/share/proj"

    failures = []

    def run_one(entry):
        name, cmds, _, env = entry
        merged = dict(base_env)
        merged.update(env)
        try:
            proc = subprocess.run(cmds, cwd=os.path.dirname(os.path.abspath(build_dir)),
                                  env=merged, capture_output=True, text=True, timeout=900)
            ok = proc.returncode == 0
            return name, ok, (proc.stdout + proc.stderr)[-4000:]
        except subprocess.TimeoutExpired:
            return name, False, "timeout"

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        for n, (name, ok, output) in enumerate(pool.map(run_one, selected), 1):
            status = "PASS" if ok else "FAIL"
            print(f"[{n}/{len(selected)}] {status} {name}", flush=True)
            if not ok:
                failures.append(name)
                print(output, flush=True)

    print(f"\n==== summary: {len(selected) - len(failures)} passed, {len(failures)} failed ====")
    if failures:
        print("failed:", ", ".join(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
