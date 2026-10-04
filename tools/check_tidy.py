#!/usr/bin/env python3
# clang-tidy 增量门禁（T4）：只扫「相对 base 改动过」的 src/ 翻译单元。
#
# 为什么只扫改动文件：全仓 300+ TU 一次 clang-tidy 分钟级且存量告警会淹没
# 门禁语义；增量口径让"新代码不欠账、旧代码不回头清算"。存量清算走
# TODOS.md 既有的 include-order 等递延项（见 PR 说明），不混进门禁。
#
# 为什么只扫 .cpp：compile_commands.json 只登记 TU；改动的头文件经由
# 包含它的 TU 被分析（header-filter 限定 src/），告警归属照常输出。
#
# 用法（CI 与本地同一入口）：
#   python3 tools/check_tidy.py                       # base=merge-base(origin/master)
#   python3 tools/check_tidy.py --base HEAD~3         # 自选基线
#   python3 tools/check_tidy.py --build-dir build --all  # 全量扫（清算用，非门禁）
#
# 退出码：0 干净 / 1 有告警 / 2 环境不可用（无 clang-tidy、无 compile_commands）。

import argparse
import json
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_CONFIG = os.path.join(REPO, "tools", ".clang-tidy")
SRC_ONLY = ("src/",)

# GCC 专属 flag——clang 不认识会让 clang-tidy 直接报 "unknown argument"。
# -mno-direct-extern-access：部分发行版（Arch gcc≥15）默认注入，clang 同语义
# flag 名不同（-mno-direct-access-external-data），tidy 口径直接剥掉。
# 发现新的不认 flag 时在此追加。
GCC_ONLY_ARG = re.compile(r"^-mno-direct-extern-access$")


def tidy_compile_db(entries, build_dir):
    """生成剥掉 GCC 专属 flag 的 tidy 专用 db；clang-tidy 只认自己的方言。
    必须落在子目录且文件名恰为 compile_commands.json——clang-tidy 的 -p
    只按这个名字查找，.tidy.json 之类后缀对它不可见。"""
    subdir = os.path.join(build_dir, "tidy-db")
    os.makedirs(subdir, exist_ok=True)
    out_path = os.path.join(subdir, "compile_commands.json")
    cleaned = []
    for e in entries:
        if "arguments" in e:
            args = [a for a in e["arguments"] if not GCC_ONLY_ARG.match(a)]
            cleaned.append({**e, "arguments": args})
        else:
            cmd = re.sub(r"\s+-mno-direct-extern-access\b", "", e["command"])
            cleaned.append({**e, "command": cmd})
    with open(out_path, "w", encoding="utf-8") as fh:
        json.dump(cleaned, fh)
    return os.path.dirname(out_path)


def sh(args, cwd=REPO):
    return subprocess.run(args, cwd=cwd, capture_output=True, text=True)


def git_changed_files(base):
    """merge-base 起的已提交改动 + 工作区未提交改动，去重。"""
    mb = sh(["git", "merge-base", base, "HEAD"])
    if mb.returncode != 0:
        print(f"FAIL git merge-base {base}: {mb.stderr.strip()}", file=sys.stderr)
        sys.exit(2)
    ref = mb.stdout.strip()
    head = sh(["git", "rev-parse", "HEAD"]).stdout.strip()
    if ref == head:
        # #137：直推 master 时 origin/master == HEAD，merge-base 就是 HEAD 本身，
        # 旧逻辑 diff 为空 → 永远「无改动」静默空跑。回退到 HEAD~1（首提交无父
        # 时只看工作区），并把实际 base 打印出来，日志可审计。
        parent = sh(["git", "rev-parse", "--verify", "--quiet", "HEAD~1"])
        if parent.returncode == 0 and parent.stdout.strip():
            print(f"check-tidy：merge-base({base}, HEAD) == HEAD，回退 base=HEAD~1")
            ref = parent.stdout.strip()
    print(f"check-tidy：base {base} -> {ref[:12]}")
    committed = sh(["git", "diff", "--name-only", f"{ref}..HEAD"])
    worktree = sh(["git", "diff", "--name-only"])
    files = set()
    for out in (committed.stdout, worktree.stdout):
        files.update(l.strip() for l in out.splitlines() if l.strip())
    return files


def main():
    ap = argparse.ArgumentParser(description="clang-tidy 增量门禁（只扫 src/ 改动 TU）")
    ap.add_argument("--base", default="origin/master", help="对比基线 ref（默认 origin/master）")
    ap.add_argument("--build-dir", default="build", help="含 compile_commands.json 的构建目录")
    ap.add_argument("--config-file", default=DEFAULT_CONFIG)
    ap.add_argument("--clang-tidy", default=os.environ.get("PALEO_CLANG_TIDY", "clang-tidy"))
    ap.add_argument("--all", action="store_true", help="扫全部 src/ TU（清算模式，非门禁口径）")
    args = ap.parse_args()

    ver = sh([args.clang_tidy, "--version"])
    if ver.returncode != 0:
        print(f"FAIL 找不到 {args.clang_tidy}（CI 钉版本安装，本地见 BUILDING.md）",
              file=sys.stderr)
        sys.exit(2)
    ccdb = os.path.join(args.build_dir, "compile_commands.json")
    if not os.path.exists(ccdb):
        print(f"FAIL {ccdb} 不存在——先 cmake 配置（CMAKE_EXPORT_COMPILE_COMMANDS 已钉 ON）",
              file=sys.stderr)
        sys.exit(2)

    with open(ccdb, encoding="utf-8") as fh:
        entries = json.load(fh)
    # GCC 方言 flag 剥离：clang-tidy 只认 clang 方言（详见 tidy_compile_db）
    db_dir = tidy_compile_db(entries, args.build_dir)

    if args.all:
        tus = sorted(e["file"] for e in entries)
    else:
        changed = git_changed_files(args.base)
        db = {os.path.normpath(e["file"]): e for e in entries}
        tus = []
        for f in sorted(changed):
            if not f.startswith(SRC_ONLY) or not f.endswith((".cpp", ".cc", ".cxx")):
                continue  # 门禁只认 src/ 产品代码 TU；tests/ 不扫（存量面大，PR 说明）
            norm = os.path.normpath(os.path.join(REPO, f))
            if norm in db:
                tus.append(norm)
            else:
                print(f"NOTE {f} 改动过但不在 compile_commands（新增未编译？先 build）")
    if not tus:
        print("check-tidy：无改动 src/ TU，门禁绿。")
        return 0

    print(f"check-tidy：{len(tus)} 个 TU，checks 见 {args.config_file}")
    rc = 0
    for tu in tus:
        run = sh([args.clang_tidy, f"-p={db_dir}",
                  f"--config-file={args.config_file}", tu])
        out = run.stdout
        # warnings-as-errors 命中时 clang-tidy 退出码非 0；输出照抄供修
        lines = [l for l in out.splitlines()
                 if "warning:" in l or "error:" in l]
        if run.returncode != 0 or lines:
            rc = 1
            print(f"\nFAIL {os.path.relpath(tu, REPO)}")
            for l in lines:
                print(f"  {l}")
        else:
            print(f"OK   {os.path.relpath(tu, REPO)}")
    if rc:
        print("\nclang-tidy 门禁红：修掉或（证明误报时）在 tools/.clang-tidy 收窄选题。")
    return rc


if __name__ == "__main__":
    sys.exit(main())
