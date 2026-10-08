#!/usr/bin/env python3
# CI 工作流脚本引用存在性门禁（方向 75：asan job 断链防回升）。
#
# 用法：
#   check_ci_scripts.py [--root PATH]
#   check_ci_scripts.py --selftest
#   未知参数一律 exit 2（与 check_layering.py 同约定——参数被静默忽略即假绿）。
#
# 背景：linux-asan job 曾 run 方向 71 已删除的 tools/ci_apt_qgis.sh，安装步
# 即红并被 continue-on-error 吞掉——观察档自合入起从未跑进编译步，无人发现。
# 本门禁静态解析 .github/workflows/*.{yml,yaml} 的 `run:` 步脚本文本，提取
# `tools/` 路径引用并做存在性检查（Test-Path 语义；不查执行位——Windows
# checkout 丢 x bit，查了必假红）。
#
# 解析口径（不引第三方 yaml 依赖，CI 侧只有裸 python3）：
#   - 只扫 `run:` 的值：单行值，或 `|`/`>`（含 |-/>- 等变体）块标量按缩进
#     收割；yml 注释行天然不在收割范围内（ci.yml 顶部「原 tools/… 已删」
#     类历史注释不得误伤）；
#   - 块内 shell 注释行（首个非空白字符为 #）剔除后再提取；
#   - 引用形如 `tools/<name>`（允许子目录与 .sh/.py 后缀），带参数/引号
#     包裹均可命中，尾部标点剥离。
#
# 一致性定义（任一不满足即 exit 1）：
#   - workflows 目录存在且含至少一个 yml/yaml（整目录缺失视为树损坏）；
#   - 每个 run: 块引用的 tools/ 路径在源码根下存在。
import re
import sys
import tempfile
import shutil
from pathlib import Path

# Windows 裸跑 ctest 时控制台可能是 cp1252：中文报告行直接 UnicodeEncodeError
# 假红（CI 的 Source gates 已设 UTF-8 locale，这里兜底手动场景）。
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(errors="replace")
    sys.stderr.reconfigure(errors="replace")

WORKFLOWS_DIR = ".github/workflows"

RUN_LINE_RE = re.compile(r"^(\s*)(?:-\s+)?run:\s*(.*)$")
TOKEN_RE = re.compile(r"tools/[A-Za-z0-9_][A-Za-z0-9_./-]*")


def extract_run_texts(text: str):
    """收割全部 run: 步的脚本文本（单行值 + 块标量按缩进）。"""
    lines = text.splitlines()
    runs = []
    i = 0
    while i < len(lines):
        m = RUN_LINE_RE.match(lines[i])
        if not m:
            i += 1
            continue
        indent, rest = m.group(1), m.group(2).strip()
        if rest and rest[0] not in "|>":
            runs.append(rest)
            i += 1
            continue
        # 块标量（| / > / |- / >- …）：吃掉比 run: 键更深缩进的连续行。
        block = []
        i += 1
        while i < len(lines):
            line = lines[i]
            if not line.strip():
                block.append(line)
                i += 1
                continue
            if len(line) - len(line.lstrip()) <= len(indent):
                break
            block.append(line)
            i += 1
        runs.append("\n".join(block))
    return runs


def extract_tokens(run_text: str):
    """run 脚本文本 → tools/ 引用集合（shell 注释行剔除，尾部标点剥离）。"""
    body = "\n".join(
        ln for ln in run_text.splitlines()
        if not ln.lstrip().startswith("#")
    )
    return {t.rstrip('.,:;)"\'') for t in TOKEN_RE.findall(body)}


def check(root: Path):
    """返回 (exit_code, 报告行列表)。0=引用全存在，1=断链/树损坏。"""
    wf_dir = root / WORKFLOWS_DIR
    ymls = sorted(p for p in wf_dir.glob("*.yml")) + \
        sorted(p for p in wf_dir.glob("*.yaml"))
    if not ymls:
        return 1, ["FAIL %s 下没有任何 yml/yaml（工作流目录损坏？）" % WORKFLOWS_DIR]
    lines = []
    missing = 0
    total_refs = 0
    for yml in ymls:
        refs = set()
        for run_text in extract_run_texts(yml.read_text(encoding="utf-8")):
            refs |= extract_tokens(run_text)
        for ref in sorted(refs):
            total_refs += 1
            if (root / ref).exists():
                lines.append("  %-14s %s OK" % (yml.name, ref))
            else:
                missing += 1
                lines.append("  %-14s %s MISSING" % (yml.name, ref))
    if missing:
        lines.append("FAIL %d/%d 个 tools/ 引用断链（脚本不存在）" % (missing, total_refs))
        return 1, lines
    lines.append("OK %d 个 tools/ 引用全部存在（%d 个 workflow 文件）" % (total_refs, len(ymls)))
    return 0, lines


# ---- selftest：合成树验证 通过/各类断链/注释不误伤 都能正确判定 ----------------
SELFTEST_OK_SCRIPT = "#!/bin/sh\nexit 0\n"
SELFTEST_YML_BASE = """name: t
on: [push]
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - run: tools/ok.sh
"""


def _selftest_write_tree(base: Path, yml: str, with_ok: bool = True):
    (base / WORKFLOWS_DIR).mkdir(parents=True)
    (base / WORKFLOWS_DIR / "ci.yml").write_text(yml, encoding="utf-8")
    if with_ok:
        (base / "tools").mkdir()
        (base / "tools" / "ok.sh").write_text(SELFTEST_OK_SCRIPT, encoding="utf-8")


def selftest() -> int:
    cases = [
        ("全存在（单行+块标量）", SELFTEST_YML_BASE + """      - run: |
          tools/ok.sh --flag arg
""", True, 0),
        ("单行引用缺失", SELFTEST_YML_BASE + """      - run: tools/ghost.sh
""", True, 1),
        ("块标量引用缺失", SELFTEST_YML_BASE + """      - run: |
          echo start
          python3 tools/ghost.py --strict
""", True, 1),
        ("折叠标量引用缺失", SELFTEST_YML_BASE + """      - run: >-
          echo start &&
          tools/ghost.sh
""", True, 1),
        ("yml 注释提及不误伤", """# 原 tools/ghost.sh 已删（方向 71）
""" + SELFTEST_YML_BASE, True, 0),
        ("shell 注释行不误伤", SELFTEST_YML_BASE + """      - run: |
          # tools/ghost.sh 旧路已删
          tools/ok.sh
""", True, 0),
        ("带参数/引号引用", SELFTEST_YML_BASE + """      - run: python3 'tools/ok.sh' --base "$BASE"
""", True, 0),
        ("workflows 目录缺失", None, False, 1),
    ]
    base = Path(tempfile.mkdtemp(prefix="ci-scripts-selftest-"))
    failed = 0
    try:
        for name, yml, with_ok, want in cases:
            tree = base / re.sub(r"[\s/]+", "_", name)
            tree.mkdir()
            if yml is not None:
                _selftest_write_tree(tree, yml, with_ok)
            code, _ = check(tree)
            status = "ok" if code == want else "FAIL"
            if code != want:
                failed += 1
            print("  selftest %-22s -> exit %d (期望 %d) %s" % (name, code, want, status))
    finally:
        shutil.rmtree(base, ignore_errors=True)
    print("  selftest %s" % ("OK" if failed == 0 else "FAILED (%d)" % failed))
    return 1 if failed else 0


def main(argv) -> int:
    root = None
    i = 1
    while i < len(argv):
        if argv[i] == "--selftest":
            return selftest()
        if argv[i] == "--root":
            if i + 1 >= len(argv):
                print("FAIL --root 需要一个参数", file=sys.stderr)
                return 2
            root = Path(argv[i + 1])
            i += 2
            continue
        print("FAIL 未知参数: %s（用法：--root PATH | --selftest）" % argv[i], file=sys.stderr)
        return 2
    if root is None:
        root = Path(__file__).resolve().parent.parent
    code, lines = check(root)
    for line in lines:
        print(line)
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv))
