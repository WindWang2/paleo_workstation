#!/usr/bin/env python3
# relink-surface.py — 测量「touch 单模块源文件后的重链接面」（T1 对比表口径）。
#
# 口径：touch 一个模块的 .cpp → 真实 ninja 构建 → 统计
#   "Linking CXX executable tst_*" 行数 = 受牵连重链的测试可执行数。
# 用真实构建而非 ninja -n：本机 ninja(kitware fork) 在 regen console 边 +
# dry-run 组合下会把真实工作漏报为空（实测假绿），链接行计数不受影响。
#
# 用法（先保证 build 是 settled 的——所有产物最新）：
#   python3 scripts/relink-surface.py --build build --src src/domain/types.cpp src/io/... 
# 输出：每个文件的 tst relink 计数行，可直接粘进 docs/progress/devex.md 对比表。

import argparse
import subprocess
import sys


def measure(build, src_file):
    subprocess.run(["touch", src_file], check=True)
    run = subprocess.run(["ninja", "-C", build], capture_output=True, text=True)
    if run.returncode != 0:
        print(f"FAIL build after touching {src_file}:\n{run.stdout[-2000:]}", file=sys.stderr)
        sys.exit(1)
    n = sum(1 for l in run.stdout.splitlines() if "Linking CXX executable tst_" in l)
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build")
    ap.add_argument("--src", nargs="+", required=True, help="仓库根相对路径的 .cpp")
    args = ap.parse_args()
    for f in args.src:
        n = measure(args.build, f)
        print(f"{f}\t{n}")


if __name__ == "__main__":
    sys.exit(main())
