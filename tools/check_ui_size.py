#!/usr/bin/env python3
"""视图层文件规模闸（#502）。

背景：src/ui 占全仓 38.6%（113,101 行 / 293,344 行），>1000 行文件 12 个、
>1500 行 4 个（seismic3dviewpanel 1811 / entitypanel 1620 / wellsectionpanel
1541 / mappingworkbenchpage 1517），是合并冲突震中与分层退化的入口
（#473「attachWorkbench 1060 行巨型视图函数」是其面上实例，docs/UI_LAYER_PLAN.md
要求「视图只发信号不干活」）。既有闸都不管体量——
check_layering 管 include 层、check_ui_invariants 管动画/色值/DPR——
故新增本闸，口径「只缩不涨」。

规则（扫描 src/ui/** 的 .cpp/.h/.hpp）：
  R1 新超限  ：行数 > LIMIT 且不在 baseline        → 违规
  R2 涨幅    ：行数 > baseline 记录值               → 违规（只缩不涨）
  R3 条目失效：baseline 路径在树里不存在            → 违规（防改名/删除绕过）
  R4 STALE   ：baseline 条目已 ≤ LIMIT（可出名单）  → 仅 --strict 计红

baseline 格式：`<relpath>:<lines>`，`#` 注释与空行忽略。
`--emit-baseline` 按当前树重写 baseline（人工 review 后提交）；
行数口径由本脚本单点定义（`count_lines`），避免手写数字与工具漂移。

用法：
  check_ui_size.py [--strict] [--src ROOT] [--baseline PATH] [--limit N]
  check_ui_size.py --emit-baseline [--src ROOT] [--baseline PATH]
  check_ui_size.py --selftest
退出码：0 = 通过；1 = 有违规（--strict 另含 STALE）；2 = 参数错/扫描根缺失。
"""
import shutil
import sys
import tempfile
from pathlib import Path

DEFAULT_LIMIT = 1000
DEFAULT_BASELINE = "tools/ui-size-baseline.txt"
SCAN_SUBDIR = ("src", "ui")
SUFFIXES = (".cpp", ".h", ".hpp")


def count_lines(text: str) -> int:
    """行数口径（单点定义）：换行符数 + 末行无换行时补 1。

    与 `wc -l` 的差别仅在「文件末尾无换行」，补 1 后与编辑器显示行数一致；
    生成 baseline 与校验走同一函数，口径不会漂移。
    """
    if not text:
        return 0
    return text.count("\n") + (0 if text.endswith("\n") else 1)


def scan(root: Path):
    """返回 {relpath: lines}（relpath 用 / 分隔，相对 root）。"""
    ui_root = root.joinpath(*SCAN_SUBDIR)
    if not ui_root.is_dir():
        # 扫描根不存在 = 定位错误（ctest 的 cwd 是构建目录），不是「零违规」。
        raise FileNotFoundError(f"scan root not found: {ui_root}")
    out = {}
    for path in sorted(ui_root.rglob("*")):
        if not path.is_file() or path.suffix not in SUFFIXES:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        out[path.relative_to(root).as_posix()] = count_lines(text)
    if not out:
        raise FileNotFoundError(f"scan root has no sources: {ui_root}")
    return out


def load_baseline(path: Path):
    """返回 {relpath: lines}；行尾无 `:<lines>` 视为格式错。"""
    entries, errors = {}, []
    if not path.exists():
        return entries, errors
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        ln = raw.strip()
        if not ln or ln.startswith("#"):
            continue
        rel, _, num = ln.rpartition(":")
        if not rel or not num.isdigit():
            errors.append(f"{path.name}:{lineno}: 期望 `<relpath>:<lines>`，实得 {ln!r}")
            continue
        entries[rel] = int(num)
    return entries, errors


def check(sizes, baseline, limit):
    """返回 (violations, stale)；violations 为 [(kind, detail)]。"""
    violations, stale = [], []
    for rel, lines in sorted(sizes.items()):
        if rel in baseline:
            if lines > baseline[rel]:
                violations.append(
                    ("R2", f"{rel}:{lines} 行 > baseline {baseline[rel]} 行——巨型文件只缩不涨"))
            elif lines <= limit:
                stale.append(f"{rel}:{lines} 行（已 ≤ {limit}，可从 baseline 出名单）")
        elif lines > limit:
            violations.append(
                ("R1", f"{rel}:{lines} 行 > {limit} 行上限——请拆分，或人工确认后 --emit-baseline 登记"))
    for rel in sorted(baseline):
        if rel not in sizes:
            violations.append(("R3", f"{rel}: baseline 条目在树中不存在（改名/删除后请同步名单）"))
    return violations, stale


def emit_baseline(root: Path, path: Path, limit: int) -> int:
    sizes = scan(root)
    over = {rel: n for rel, n in sorted(sizes.items()) if n > limit}
    lines = [
        "# 视图层文件规模 baseline（#502）——只缩不涨。",
        f"# 生成：tools/check_ui_size.py --emit-baseline（口径由 count_lines 单点定义，勿手改数字）",
        f"# 上限：{limit} 行；条目降到 ≤ {limit} 行后应从本文件删除（--strict 会红）。",
        f"# 条目数：{len(over)}",
    ]
    lines += [f"{rel}:{n}" for rel, n in over.items()]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"baseline written: {path}（{len(over)} 条 > {limit} 行）")
    return 0


def selftest() -> int:
    """覆盖 R1/R2/R3/R4 四类判定与 baseline 解析。"""
    tmp = Path(tempfile.mkdtemp(prefix="ui-size-selftest-"))
    try:
        ui = tmp / "src" / "ui"
        ui.mkdir(parents=True)
        (ui / "small.cpp").write_text("int a;\n" * 10, encoding="utf-8")
        (ui / "grown.cpp").write_text("int b;\n" * 1200, encoding="utf-8")
        (ui / "shrunk.cpp").write_text("int c;\n" * 900, encoding="utf-8")
        (ui / "fresh.cpp").write_text("int d;\n" * 1100, encoding="utf-8")
        (ui / "noeol.cpp").write_text("int e;", encoding="utf-8")
        baseline_path = tmp / "baseline.txt"
        baseline_path.write_text(
            "# comment\n"
            "src/ui/grown.cpp:1100\n"
            "src/ui/shrunk.cpp:1200\n"
            "src/ui/gone.cpp:1500\n",
            encoding="utf-8")

        sizes = scan(tmp)
        ok = (sizes["src/ui/noeol.cpp"] == 1 and sizes["src/ui/small.cpp"] == 10)

        baseline, berr = load_baseline(baseline_path)
        ok = ok and not berr and baseline == {
            "src/ui/grown.cpp": 1100, "src/ui/shrunk.cpp": 1200, "src/ui/gone.cpp": 1500}

        violations, stale = check(sizes, baseline, DEFAULT_LIMIT)
        kinds = sorted(k for k, _ in violations)
        # grown 1200 > baseline 1100 → R2；fresh 1100 > 1000 → R1；gone 不存在 → R3
        ok = ok and kinds == ["R1", "R2", "R3"]
        ok = ok and len(stale) == 1 and "shrunk.cpp" in stale[0]

        # 恶意 baseline 格式必须报错，不能静默忽略。
        bad = tmp / "bad.txt"
        bad.write_text("src/ui/oops\n", encoding="utf-8")
        _, berr2 = load_baseline(bad)
        ok = ok and len(berr2) == 1

        # 扫描根缺失 = 定位错误（exit 2 语义），不是零违规。
        try:
            scan(tmp / "nope")
            ok = False
        except FileNotFoundError:
            pass
        print("selftest:", "PASS" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main() -> int:
    args = sys.argv[1:]
    if "--selftest" in args:
        return selftest()

    strict = "--strict" in args
    do_emit = "--emit-baseline" in args
    root = Path(__file__).resolve().parents[1]
    baseline_path = root / DEFAULT_BASELINE
    limit = DEFAULT_LIMIT
    rest, i = [], 0
    while i < len(args):
        a = args[i]
        if a == "--src" and i + 1 < len(args):
            root = Path(args[i + 1]).resolve()
            i += 2
        elif a == "--baseline" and i + 1 < len(args):
            baseline_path = Path(args[i + 1]).resolve()
            i += 2
        elif a == "--limit" and i + 1 < len(args):
            if not args[i + 1].isdigit():
                print("--limit 需要整数")
                return 2
            limit = int(args[i + 1])
            i += 2
        elif a in ("--strict", "--emit-baseline"):
            i += 1
        else:
            rest.append(a)
            i += 1
    if rest:
        print("unknown argument(s):", " ".join(rest))
        return 2

    if do_emit:
        try:
            return emit_baseline(root, baseline_path, limit)
        except FileNotFoundError as e:
            print("ERROR", e)
            return 2

    baseline, berr = load_baseline(baseline_path)
    try:
        sizes = scan(root)
    except FileNotFoundError as e:
        print("ERROR", e)
        return 2

    violations, stale = check(sizes, baseline, limit)
    rc = 0
    for kind, detail in violations:
        print(f"FAIL [{kind}] {detail}")
        rc = 1
    for detail in berr:
        print(f"FAIL [baseline] {detail}")
        rc = 1
    for detail in stale:
        if strict:
            print(f"FAIL [R4] {detail}")
            rc = 1
        else:
            print(f"NOTE {detail}")
    if rc == 0:
        over = sum(1 for n in sizes.values() if n > limit)
        print(f"ui size: clean（扫描 {len(sizes)} 文件，> {limit} 行 {over} 个 / baseline {len(baseline)} 条）")
    return rc


if __name__ == "__main__":
    sys.exit(main())
