#!/usr/bin/env python3
# Qgs 散布符号级防增长闸（#472）——独立工具，不动 check_layering.py。
#
# 背景：docs/PALEO_QGIS_PLAN.md §2 要求 QGIS API 只经 src/qgis/ 封装层散布
#（「以后无论迁移到 QGIS 4.x，只修改这一层」）。check_layering.py --qgs-audit
# 只审 include 级直连；本闸补符号级口径——凡 src/workflow、src/ui 中
#【直接 include 外部 qgs*.h 头】或【代码里使用 Qgs 前缀符号】（含
# `class QgsX;` 前向声明接口面暴露）的文件均计入散布面。
#
# 用法：
#   check_qgis_spread.py [--strict]              # 对照基线只缩不涨
#   check_qgis_spread.py --emit-baseline         # 按当前树重写基线（review 后提交）
#   check_qgis_spread.py --selftest              # 内置正/反夹具
# 退出码：0 通过；1 有违规（--strict 另含可收缩条目）；2 参数/环境错。
#
# 扫描面：src/workflow/** 与 src/ui/** 的 .h/.cpp。qgis/、app/、selfcheck/
# 天然不在扫描面（封装层/组装根/测试壳 by design 豁免）；ui/workflow 内的
# 逐文件豁免 = tools/layering_vocab.json 的 qgs_audit_whitelist（与
# --qgs-audit 单点同源；缺省为空）∪ 脚本内 QGIS_SPREAD_EXTRA_WHITELIST
#（当前为空，新条目须在 PR 里给出理由）。
#
# 基线 tools/qgis-spread-baseline.txt：每行一个 src/ 相对路径（脚本生成，
# 勿手写）。语义沿用 layering-baseline 只缩不涨机制：
#   - 基线外的新散布文件 → FAIL（exit 1）；
#   - 基线内文件收敛（无 qgs include 且无 Qgs 符号）→ NOTE 可收缩，
#     --strict 下红（强制删行）；
#   - 基线非空不判红（现状冻结，与已归零的 layering-baseline 不同期）。
# 收敛方向：把 QGIS 调用收进 src/qgis/ 封装（ui 侧 QGIS 原生控件视图可
# 暂缓，见 #472）；收敛一个删一行，跨闸（--qgs-audit）同批处理。

import json
import os
import re
import sys

for _s in (sys.stdout, sys.stderr):
    try:
        if _s and hasattr(_s, "reconfigure"):
            _s.reconfigure(encoding="utf-8", errors="replace")
    except (ValueError, OSError):
        pass

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "src")
VOCAB = os.path.join(REPO, "tools", "layering_vocab.json")
BASELINE = os.path.join(REPO, "tools", "qgis-spread-baseline.txt")

# E-2（full-audit-20261021）：扫描面与 check_layering --qgs-audit 对齐——
# 从 workflow+ui 扩到全部非视图模块；新增面按 #539 初冻口径入基线。
SCAN_DIRS = ("domain", "catalog", "io", "metadata", "services", "algorithms",
             "workflow", "linkage", "ai", "ui")
SUFFIXES = (".h", ".cpp")

# 逐文件豁免的脚本内补充面（与词表 qgs_audit_whitelist 取并集）。
# 语义同 qgs_audit_whitelist：该文件的 Qgs 散布是已裁决例外。
QGIS_SPREAD_EXTRA_WHITELIST = set()

# 外部 qgs 头：basename 匹配 qgs*.h（repo src/qgis/ 封装头不算，见下）。
QGS_HEADER_RE = re.compile(r"^(?:qgs[a-z0-9_]*|qgis)\.h$")  # qgis.h 同为外部 QGIS 头（2026-10-27 盲区修复）
# Qgs 前缀符号（类/函数/枚举一律 Qgs 打头）：如 QgsVectorLayer、
# QgsProject::instance()、qobject_cast<QgsRasterLayer *>、FID_NULL 之外的
# QgsCsException 等。只匹配代码（注释与字符串先剥除）。Qgis:: 命名空间
# 符号（Qgis::version()/Qgis::WkbType 等）同口径——2026-10-27 盲区修复：
# 此前仅 Qgis:: 的文件（如 evidencerecord.cpp）对符号级判定隐形。
QGS_SYMBOL_RE = re.compile(r"\b(?:Qgs[A-Za-z0-9_]+|Qgis::)")

_INC_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
_CODE_ONLY_RE = re.compile(
    r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'])*\'|//[^\n]*|/\*.*?\*/', re.S)


def strip_comments_and_strings(text):
    """注释与字符串字面整体替换为空白（保留换行，行号不漂移）。"""
    return _CODE_ONLY_RE.sub(
        lambda m: re.sub(r"[^\n]", " ", m.group()), text)


def load_whitelist():
    """词表 qgs_audit_whitelist 缺省为空（旧词表兼容：只会更严）。"""
    try:
        with open(VOCAB, encoding="utf-8") as fh:
            data = json.load(fh)
        wl = set(str(x) for x in data.get("qgs_audit_whitelist", []))
    except (OSError, ValueError, TypeError):
        # 词表不可用不阻断本闸：豁免面退化为脚本内清单（只会更严）。
        wl = set()
    return wl | QGIS_SPREAD_EXTRA_WHITELIST


def norm_include(file_dir, inc):
    """include 归一为 src/ 相对路径（与 check_layering.py 同口径，防 ../ 前缀
    绕过）；非 repo 内路径返回 None。顶层目录词表取 SCAN_DIRS + qgis + 全部
    既有 src/ 顶层（保守：只要能解析到 repo 内就算 repo 边）。"""
    top_dirs = set()
    try:
        for d in os.listdir(SRC):
            if os.path.isdir(os.path.join(SRC, d)):
                top_dirs.add(d)
    except OSError:
        top_dirs = set(SCAN_DIRS) | {"qgis"}
    if inc.startswith(tuple(d + "/" for d in top_dirs)):
        return inc
    cand = os.path.normpath(os.path.join(file_dir, inc))
    rel = os.path.relpath(cand, SRC)
    if rel.startswith(".."):
        return None
    return rel.replace(os.sep, "/")


def iter_sources():
    for d in SCAN_DIRS:
        root = os.path.join(SRC, d)
        for dirpath, _dirs, files in os.walk(root):
            for f in sorted(files):
                if f.endswith(SUFFIXES):
                    yield os.path.join(dirpath, f)


def file_signals(path):
    """返回 (has_qgs_include, has_qgs_symbol)。
    - has_qgs_include：直接 include 外部 qgs*.h（basename 匹配；include 落在
      repo qgis/ 且文件存在的封装头不算）。
    - has_qgs_symbol：剥除注释/字符串后出现 Qgs 前缀符号（含前向声明）。"""
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return False, False
    has_inc = False
    for m in _INC_RE.finditer(text):
        inc = m.group(1)
        norm = norm_include(os.path.dirname(path), inc)
        if norm is not None and norm.split("/", 1)[0] == "qgis" \
                and os.path.exists(os.path.join(SRC, norm)):
            continue  # repo 封装头通道：合法
        if QGS_HEADER_RE.match(os.path.basename(inc)):
            has_inc = True
            break
    has_sym = bool(QGS_SYMBOL_RE.search(strip_comments_and_strings(text)))
    return has_inc, has_sym


def collect_spread(whitelist):
    """→ (offenders 集合[src/ 相对路径], per-file 信号表)。"""
    offenders = set()
    signals = {}
    for path in iter_sources():
        rel = "src/" + os.path.relpath(path, SRC).replace(os.sep, "/")
        has_inc, has_sym = file_signals(path)
        signals[rel] = (has_inc, has_sym)
        if (has_inc or has_sym) and rel not in whitelist:
            offenders.add(rel)
    return offenders, signals


def load_baseline(path=BASELINE):
    entries = set()
    if not os.path.exists(path):
        return entries
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            entries.add(line)
    return entries


def emit_baseline(path=BASELINE):
    whitelist = load_whitelist()
    offenders, _signals = collect_spread(whitelist)
    by_dir = {}
    for rel in sorted(offenders):
        by_dir.setdefault(rel.split("/")[1], []).append(rel)
    lines = [
        "# qgis-spread 基线 —— Qgs 散布符号级现状冻结清单（#472，",
        "# tools/check_qgis_spread.py 生成，勿手写路径）。",
        "# 口径：src/workflow 与 src/ui 中【直接 include 外部 qgs*.h】或",
        "#【代码使用 Qgs 前缀符号（含前向声明）】的文件（逐文件豁免面 =",
        "# tools/layering_vocab.json qgs_audit_whitelist ∪ 脚本内附加清单）。",
        "# 语义（沿用 layering-baseline 只缩不涨）：基线外新散布 → FAIL；",
        "# 基线内文件收敛 → NOTE 可收缩（--strict 红，强制删行）。",
        "# 收敛方向：QGIS 调用收进 src/qgis/ 封装（docs/PALEO_QGIS_PLAN.md",
        "# §2）；与 check_layering.py --qgs-audit（include 级）同批收敛。",
    ]
    lines.append(f"# 条目数：{len(offenders)}"
                 + "".join(f"（{d} {len(v)}）" for d, v in sorted(by_dir.items())))
    lines += sorted(offenders)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")
    print(f"baseline written: {path}（{len(offenders)} 文件"
          + "".join(f"；{d} {len(v)}" for d, v in sorted(by_dir.items())) + "）")
    return 0


def run_check(strict=False, baseline_path=None):
    whitelist = load_whitelist()
    offenders, _signals = collect_spread(whitelist)
    baseline = load_baseline(baseline_path or BASELINE)
    new = sorted(offenders - baseline)
    shrunk = sorted(baseline - offenders)
    residue = sorted(offenders & baseline)
    for p in new:
        print(f"FAIL {p} 新增 Qgs 散布（qgs* 直连 include 或 Qgs 前缀符号"
              f"越过 src/qgis 封装层——docs/PALEO_QGIS_PLAN.md §2："
              f"QGIS 迁移成本只许落在 src/qgis）")
    for p in shrunk:
        print(f"NOTE baseline 可收缩：{p}（该文件已无 qgs* 直连且无 Qgs 符号，"
              f"请删除该行）")
    n_wf = len([p for p in offenders if p.split("/")[1] == "workflow"])
    n_ui = len([p for p in offenders if p.split("/")[1] == "ui"])
    n_other = len(offenders) - n_wf - n_ui
    extra = f" + 非视图其他模块 {n_other}" if n_other else ""
    print(f"qgis-spread：散布文件 {len(offenders)}（workflow {n_wf} + ui {n_ui}{extra}；"
          f"baseline {len(baseline)}，残留 {len(residue)}，新增 {len(new)}，"
          f"可收缩 {len(shrunk)}）——符号级现状冻结、只缩不涨；豁免面见 "
          f"tools/layering_vocab.json qgs_audit_whitelist（+脚本内附加清单）。")
    if new:
        print("\n新增散布须改走 src/qgis/ 封装（或经 qgs_audit_whitelist 显式"
              "裁决），并把对应文件从 baseline 收缩——不许加行。")
        return 1
    if strict and shrunk:
        print(f"\n--strict：qgis-spread baseline 有 {len(shrunk)} 条可收缩——"
              f"收敛后删除对应行再合入。")
        return 1
    return 0


def selftest():
    """临时树夹具：include/符号/注释反例/封装头通道/白名单/基线三态。"""
    import tempfile

    global SRC, BASELINE, VOCAB
    orig_src, orig_baseline, orig_vocab = SRC, BASELINE, VOCAB
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        SRC = tmp
        VOCAB = os.path.join(tmp, "vocab.json")  # 不存在 → 豁免面=脚本内清单
        BASELINE = os.path.join(tmp, "qgis-spread-baseline.txt")

        def put(rel, text):
            p = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, "w", encoding="utf-8").write(text)

        # 正例 1：直接 include 外部 qgs 头。
        put("workflow/inc_direct.cpp",
            '// 层：功能\n#include <qgsmaplayer.h>\n')
        # 正例 2：无 qgs include，但代码用 Qgs 符号（含前向声明）。
        put("workflow/symbol_only.h",
            '// 层：功能\nclass QgsVectorLayer;\nvoid f(QgsVectorLayer *l);\n')
        # 正例 3：ui 侧符号使用（qobject_cast 形态）。
        put("ui/cast.cpp",
            '// 层：视图\n#include <memory>\n'
            'auto v = qobject_cast<QgsVectorLayer *>(p);\n')
        # 反例 1：注释/字符串里的 Qgs 不算。
        put("ui/comment_only.cpp",
            '// 层：视图\n// QgsVectorLayer 仅注释\n'
            'auto s = QStringLiteral("QgsProject");\n')
        # 反例 2：repo qgis/ 封装头通道（文件存在）不算直连，且无符号。
        put("qgis/wrapper.h", '// 层：QGIS 封装\n#pragma once\n')
        put("ui/via_facade.cpp",
            '// 层：视图\n#include "../qgis/wrapper.h"\n')
        # 反例 3：纯 Qt 文件。
        put("ui/plain.cpp", '// 层：视图\n#include <QWidget>\n')

        offenders, _ = collect_spread(set())
        expected = {"src/workflow/inc_direct.cpp",
                    "src/workflow/symbol_only.h",
                    "src/ui/cast.cpp"}
        if offenders != expected:
            print(f"SELFTEST FAIL spread: 实得 {sorted(offenders)}，"
                  f"期望 {sorted(expected)}")
            failures += 1

        # 白名单豁免。
        if collect_spread({"src/ui/cast.cpp"})[0] != expected - {"src/ui/cast.cpp"}:
            print("SELFTEST FAIL spread: 词表豁免条目必须生效")
            failures += 1

        # 基线三态：空基线 + 新散布 → 红；命中 → 绿；收敛 → 非严格绿/严格红。
        open(BASELINE, "w", encoding="utf-8").write("# empty\n")

        def rc(strict=False):
            return run_check(strict=strict, baseline_path=BASELINE)

        if rc() != 1:
            print("SELFTEST FAIL gate: 基线外新散布必须红")
            failures += 1
        open(BASELINE, "w", encoding="utf-8").write(
            "\n".join(sorted(expected)) + "\n")
        if rc() != 0 or rc(strict=True) != 0:
            print("SELFTEST FAIL gate: 命中基线的残留应双绿")
            failures += 1
        # cast.cpp 收敛（改纯 Qt）→ NOTE/strict 红。
        put("ui/cast.cpp", '// 层：视图\n#include <QWidget>\n')
        if rc() != 0:
            print("SELFTEST FAIL gate: 可收缩条目非严格应绿")
            failures += 1
        if rc(strict=True) != 1:
            print("SELFTEST FAIL gate: 可收缩条目严格必须红（强制删行）")
            failures += 1

        # 词表加载：qgs_audit_whitelist 读取 + 坏 JSON 退化为空面（更严）。
        open(VOCAB, "w", encoding="utf-8").write(
            '{"qgs_audit_whitelist": ["src/ui/cast.cpp"]}')
        if "src/ui/cast.cpp" not in load_whitelist():
            print("SELFTEST FAIL vocab: qgs_audit_whitelist 未生效")
            failures += 1
        open(VOCAB, "w", encoding="utf-8").write("{not json")
        if load_whitelist() != QGIS_SPREAD_EXTRA_WHITELIST:
            print("SELFTEST FAIL vocab: 坏词表应退化为脚本内清单（更严）")
            failures += 1

    SRC, BASELINE, VOCAB = orig_src, orig_baseline, orig_vocab
    if failures:
        print(f"selftest：{failures} 处夹具失败")
        return 1
    print("selftest 通过：3 散布正例 + 3 反例 + 1 豁免 + 5 基线三态 + "
          "2 词表夹具全部命中预期。")
    return 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    strict = emit = False
    for a in argv:
        if a == "--strict":
            strict = True
        elif a == "--emit-baseline":
            emit = True
        elif a == "--selftest":
            return selftest()
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            print(f"FAIL 未知参数：{a}\n"
                  f"       用法：check_qgis_spread.py [--strict] "
                  f"| --emit-baseline | --selftest", file=sys.stderr)
            return 2
    if strict and emit:
        print("FAIL --strict 与 --emit-baseline 互斥", file=sys.stderr)
        return 2
    if not os.path.isdir(os.path.join(SRC, "workflow")) or \
            not os.path.isdir(os.path.join(SRC, "ui")):
        print(f"FAIL 扫描根缺失：{SRC}/workflow 或 {SRC}/ui", file=sys.stderr)
        return 2
    if emit:
        return emit_baseline()
    return run_check(strict=strict)


if __name__ == "__main__":
    sys.exit(main())
