#!/usr/bin/env python3
# 视图层规模防增长闸（#502）：src/ui 单文件 ≤1000 行 / 单函数 ≤200 行。
#
# 用法：
#   check_file_budget.py [--root PATH] [--baseline PATH] [--strict]
#   check_file_budget.py --selftest
#   未知参数一律 exit 2（与 check_layering.py 同约定——参数被静默忽略即假绿）。
#
# 背景：#502 实测 src/ui 113,101 行占全仓 38.6%，>1000 行文件 ui 内 12 个
#（全仓 43 个，TOP5 中 4 个在 ui/）。缺的不是一次性拆分而是闸——方向 96/97
# 拆掉一批后新巨型文件仍在冒出。本闸与 layering/qgs-audit 同族：
#
#   - 超标面 = 规则（file >1000 行 / 函数体 >200 行）× 当前值；
#   - tools/file-budget-baseline.txt（格式 `<src 相对路径>:<kind>:<冻结值>`，
#     kind = file 或 fn:<函数名>）冻结现状，只缩不涨：
#       * baseline 外的新超标 → FAIL；
#       * 当前值 > 冻结值（回升）→ FAIL；
#       * 当前值 < 冻结值（仍超标）→ NOTE 可收紧（--strict 红，防「瘦了
#         又胖回去」的锯齿）；当前值已回到阈值内 → NOTE 可收缩删行
#         （--strict 红）。
#   - 阈值不放宽：拆分是收敛方向（ctor 编排主文件 + 域 TU + *_internal.h，
#     见方向 96/97 先例），TOP5 拆分另行跟踪（#502 另派，本闸只管不涨）。
#
# 函数体口径（启发式，注释/字符串先剥离，行号不漂移）：`)`-前有标识符且
# 非控制关键字（if/for/while/switch/catch）、`)` 后只许 const/noexcept/
# override/final/尾置返回/构造初始化列表的 `{`…`}` 配对块；括号内出现的
# 块（lambda 实参等）不计。启发式的误差由 baseline 冻结值吸收——名字对
# 不上的新超标仍会红。行数含起止行（开 `{` 行到配对 `}` 行）。
"""视图层规模防增长闸（#502）：src/ui 单文件 ≤1000 行 / 单函数 ≤200 行，baseline 只缩不涨。"""
import argparse
import re
import sys
import tempfile
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(errors="replace")
    sys.stderr.reconfigure(errors="replace")

REPO = Path(__file__).resolve().parents[1]
BASELINE = REPO / "tools" / "file-budget-baseline.txt"
FILE_LIMIT = 1000
FN_LIMIT = 200

_CODE_ONLY_RE = re.compile(
    r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/', re.S)
CONTROL_KEYWORDS = {
    "if", "for", "while", "switch", "catch", "return", "sizeof",
    "alignof", "decltype", "noexcept", "static_assert", "else", "do", "try",
    "new", "delete", "throw", "co_return", "co_await", "Q_FOREACH",
}


def strip_comments_and_strings(text):
    """注释与字符串字面整体替换为空白（保留换行，行号不漂移）。"""
    return _CODE_ONLY_RE.sub(
        lambda m: re.sub(r"[^\n]", " ", m.group()), text)


def _match_sig(lead):
    """`... name(args) [tail]` → name；不像函数定义返回 None。
    构造函数带初始化列表时（`Ctor::Ctor() : a_(1), b_(2) {`），最右 `)`
    属于最后一个初始化项——优先取「后随 `:`」的 `)` 作参数表闭合。"""
    s = lead.rstrip()
    cands = [m.start() for m in re.finditer(r"\)", s)]
    if not cands:
        return None
    chosen = None
    for r in cands:  # 左→右找第一个后随 `:` 的（初始化列表引子）
        if re.match(r"\s*:", s[r + 1:]):
            chosen = r
            break
    if chosen is None:
        chosen = cands[-1]
        tail = s[chosen + 1:].strip()
        if tail and not re.match(r"^(const\b|noexcept\b|override\b|final\b|->)", tail):
            return None
    else:
        # `) : <init-list>`：冒号后须形如 `name(` / `name{`（首项即可判定）
        rest = s[chosen + 1:].lstrip()
        init = rest[1:].strip() if rest.startswith(":") else ""
        if not re.match(r"^[A-Za-z_~][A-Za-z0-9_]*\s*[( {]", init.split(",")[0]):
            return None
    # 回找 chosen 的配对 `(`（括号平衡）
    depth = 0
    l = -1
    for k in range(chosen, -1, -1):
        if s[k] == ")":
            depth += 1
        elif s[k] == "(":
            depth -= 1
            if depth == 0:
                l = k
                break
    if l <= 0:
        return None
    m = re.search(r"([A-Za-z_~][A-Za-z0-9_:]*)\s*$", s[:l])
    if not m:
        return None
    name = m.group(1).split("::")[-1].lstrip("~")
    if name in CONTROL_KEYWORDS or not name:
        return None
    return name


def iter_functions(stripped):
    """→ [(name, start_line, end_line)]，函数体配对块（1 起行号，含起止）。"""
    out = []
    # 行号定位：预计算每行起始偏移
    offs = [0]
    for i, ch in enumerate(stripped):
        if ch == "\n":
            offs.append(i + 1)

    def line_of(idx):
        lo, hi = 0, len(offs) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if offs[mid] <= idx:
                lo = mid
            else:
                hi = mid - 1
        return lo + 1  # 1 起

    paren = 0
    lead_start = 0
    brace_depth = 0
    open_stack = []  # (char_idx, is_fn, name)
    for i, c in enumerate(stripped):
        if c == "(":
            paren += 1
        elif c == ")":
            paren = max(0, paren - 1)
        elif c == "{":
            sig = None
            if paren == 0:
                sig = _match_sig(stripped[lead_start:i])
            open_stack.append((i, sig is not None, sig))
            brace_depth += 1
            lead_start = i + 1
        elif c == "}":
            if open_stack:
                start_idx, is_fn, name = open_stack.pop()
                if is_fn and name:
                    out.append((name, line_of(start_idx), line_of(i)))
            brace_depth -= 1
            lead_start = i + 1
        elif c == ";" and paren == 0:
            lead_start = i + 1
    return out


def collect_violations(root):
    """→ {(rel_path, kind): value}；kind = 'file' 或 'fn:<name>'。"""
    ui = root / "src" / "ui"
    out = {}
    for p in sorted(ui.rglob("*")):
        if p.suffix not in (".h", ".cpp"):
            continue
        rel = str(p.relative_to(root)).replace("\\", "/")  # 形如 src/ui/x.cpp
        raw = p.read_text(encoding="utf-8", errors="replace")
        n_lines = len(raw.splitlines())
        if n_lines > FILE_LIMIT:
            out[(rel, "file")] = n_lines
        stripped = strip_comments_and_strings(raw)
        for name, s, e in iter_functions(stripped):
            if e - s + 1 > FN_LIMIT:
                key = (rel, f"fn:{name}")
                # 同名函数（重载/多文件分区）取最大块，避免同键值抖动。
                out[key] = max(out.get(key, 0), e - s + 1)
    return out


def load_baseline(path):
    """→ {(path, kind): value}；行格式 `<path>:<kind>:<value>`，fn 条目的
    kind 为 `fn:<函数名>`（自身含冒号）：首段=路径，末段=数值，中间=kind。"""
    entries = {}
    p = Path(path)
    if not p.is_file():
        return entries
    for line in p.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(":")
        if len(parts) < 3 or not parts[-1].isdigit() or not parts[0]:
            continue
        entries[(parts[0], ":".join(parts[1:-1]))] = int(parts[-1])
    return entries


def run(root, baseline_path, strict=False):
    baseline = load_baseline(baseline_path)
    cur = collect_violations(root)
    new, grown, tighten, shrink, residue = [], [], [], [], []
    for (path, kind), value in sorted(cur.items()):
        frozen = baseline.get((path, kind))
        limit = FILE_LIMIT if kind == "file" else FN_LIMIT
        if frozen is None:
            new.append((path, kind, value, limit))
        elif value > frozen:
            grown.append((path, kind, value, frozen))
        elif value < frozen:
            tighten.append((path, kind, value, frozen))
        else:
            residue.append((path, kind, value))
    gone = sorted(set(baseline) - set(cur))
    for path, kind, value, limit in new:
        print(f"FAIL {path} [{kind}] {value} 行 > {limit}——新增超标（#502 防增长闸），"
              f"拆分或登记 baseline 冻结须给理由")
    for path, kind, value, frozen in grown:
        print(f"FAIL {path} [{kind}] {value} 行 > baseline 冻结 {frozen}——"
              f"超标面回升，只缩不涨")
    for path, kind, value, frozen in tighten:
        print(f"NOTE {path} [{kind}] {value} 行 < 冻结 {frozen}——可收紧，"
              f"请把 tools/file-budget-baseline.txt 该行数值下调")
    for key in gone:
        print(f"NOTE baseline 可收缩：{key[0]}:{key[1]}（已回到阈值内，请删除该行）")
    n_over = len(cur)
    print(f"file-budget：超标 {n_over} 项（file {sum(1 for (_p, k) in cur if k == 'file')}"
          f" / fn {sum(1 for (_p, k) in cur if k != 'file')}），"
          f"baseline {len(baseline)}，新增 {len(new)}，回升 {len(grown)}，"
          f"可收紧 {len(tighten)}，可收缩 {len(gone)}——阈值 1000/200 不放宽。")
    rc = 0
    if new or grown:
        rc = 1
    if strict and (tighten or gone):
        print("--strict：baseline 有可收紧/可收缩条目——同步更新基线再合入。")
        rc = 1
    return rc


def selftest():
    cases = []

    def check_tree(desc, files, baseline_text, strict, want_rc):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for rel, content in files.items():
                p = root / rel
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(content, encoding="utf-8")
            bl = root / "bl.txt"
            bl.write_text(baseline_text, encoding="utf-8")
            import contextlib, io as _io
            with contextlib.redirect_stdout(_io.StringIO()):
                rc = run(root, bl, strict=strict)
        cases.append((desc, rc, want_rc))

    small_fn = "void small() {\n" + "    int x = 0;\n" * 10 + "}\n"
    big_file = "// 层：视图\n" + "\n" * FILE_LIMIT  # 1001 行
    big_file_grown = "// 层：视图\n" + "\n" * (FILE_LIMIT + 4)  # 1005 行
    big_file_ok = "// 层：视图\n" + "\n" * (FILE_LIMIT - 6)  # 995 行
    fn_201 = "void bigFn() {\n" + "    int y = 1;\n" * 199 + "}\n"  # 201 行
    fn_205 = "void bigFn() {\n" + "    int y = 1;\n" * 203 + "}\n"  # 205 行
    fn_199 = "void bigFn() {\n" + "    int y = 1;\n" * 197 + "}\n"  # 199 行

    # (a) 全净 + 空 baseline：双绿
    check_tree("全净双绿", {"src/ui/a.cpp": small_fn}, "# empty\n", False, 0)
    check_tree("全净严格绿", {"src/ui/a.cpp": small_fn}, "# empty\n", True, 0)
    # (b) 新超标无 baseline：红
    check_tree("新超标文件红", {"src/ui/big.cpp": big_file}, "# empty\n", False, 1)
    check_tree("新超标函数红", {"src/ui/f.cpp": fn_201}, "# empty\n", False, 1)
    # (c) 冻结等值：双绿
    check_tree("冻结等值双绿", {"src/ui/big.cpp": big_file},
               f"src/ui/big.cpp:file:{FILE_LIMIT + 1}\n", True, 0)
    check_tree("函数冻结双绿", {"src/ui/f.cpp": fn_201},
               "src/ui/f.cpp:fn:bigFn:201\n", True, 0)
    # (d) 回升：红
    check_tree("文件回升红", {"src/ui/big.cpp": big_file_grown},
               f"src/ui/big.cpp:file:{FILE_LIMIT + 1}\n", False, 1)
    check_tree("函数回升红", {"src/ui/f.cpp": fn_205},
               "src/ui/f.cpp:fn:bigFn:201\n", False, 1)
    # (e) 收缩：非严格绿 / 严格红（删行 or 下调数值）
    check_tree("文件回阈值内非严格绿", {"src/ui/big.cpp": big_file_ok},
               f"src/ui/big.cpp:file:{FILE_LIMIT + 1}\n", False, 0)
    check_tree("文件回阈值内严格红", {"src/ui/big.cpp": big_file_ok},
               f"src/ui/big.cpp:file:{FILE_LIMIT + 1}\n", True, 1)
    check_tree("函数回阈值内严格红", {"src/ui/f.cpp": fn_199},
               "src/ui/f.cpp:fn:bigFn:201\n", True, 1)
    # (f) 坏行 baseline（格式错）不炸、按无条目处理 → 新超标红
    check_tree("坏 baseline 行按无条目", {"src/ui/big.cpp": big_file},
               "garbage line\n", False, 1)

    # 启发式夹具：控制流/lambda/初始化列表不误计，类内成员函数与命名空间
    # 内自由函数要计。
    heuristic_src = (
        "namespace ui {\n"
        "void nsFn() {\n" + "    int a = 0;\n" * 200 + "}\n}\n"          # 命名空间内 202 行 → 计
        "class W {\n"
        "public:\n"
        "  void memberFn() {\n" + "    int b = 0;\n" * 210 + "  }\n"     # 类内成员 213 行 → 计
        "};\n"
        "void tricky() {\n"
        "  if (auto x = compute(); x) {\n" + "    int c = 0;\n" * 5 + "  }\n"  # if-初始化器不误计
        "  auto lam = [](int v) {\n" + "    int d = v;\n" * 5 + "  };\n"      # lambda 赋值不误计
        "  for (int i = 0; i < 3; ++i) {\n    int e = i;\n  }\n"               # for 不误计
        "}\n"
        "Ctor::Ctor()\n  : member_(1), other_(2) {\n" + "  int f = 0;\n" * 230 + "}\n"  # 初始化列表 → 计
    )
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "src/ui").mkdir(parents=True, exist_ok=True)
        (root / "src/ui/h.cpp").write_text(heuristic_src, encoding="utf-8")
        bl = root / "bl.txt"
        bl.write_text(
            "src/ui/h.cpp:fn:nsFn:202\n"
            "src/ui/h.cpp:fn:memberFn:212\n"
            "src/ui/h.cpp:fn:Ctor:232\n", encoding="utf-8")
        import contextlib, io as _io
        with contextlib.redirect_stdout(_io.StringIO()):
            rc = run(root, bl, strict=False)
        if rc != 0:
            cases.append(("启发式：三类真函数命中且无幻影", rc, 0))
        else:
            cases.append(("启发式：三类真函数命中且无幻影", 0, 0))
    failures = [f"{d}: rc={r} 期望 {w}" for d, r, w in cases if r != w]
    if failures:
        for f in failures:
            print("SELFTEST FAIL", f)
        return 1
    print(f"check_file_budget selftest: PASS（{len(cases)} 夹具："
          f"净/新增/冻结/回升/收缩/严格/坏行/启发式）")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=str(REPO))
    ap.add_argument("--baseline", default=str(BASELINE))
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    return run(Path(a.root), Path(a.baseline), strict=a.strict)


if __name__ == "__main__":
    sys.exit(main())
