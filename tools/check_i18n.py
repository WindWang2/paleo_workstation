#!/usr/bin/env python3
"""i18n 裸字面量门禁（WP4 / #44「无 CI 门禁」收口）。

扫描 src/ 下用户可见文案入口（setter/常见控件构造/QMenu addAction 等），
第一个参数是裸字符串字面量（"..." / QStringLiteral / u8"...")而非 tr() /
translate() / 变量 / QCoreApplication::translate 的即报违规。

纯符号/数字串（"→"、"…"、"" 等，无字母数字 CJK）不算文案，放行。
词表外的调用模式不扫——门禁只挡「新增裸字面量」的回归，存量清算归
属主方向（与 clang-tidy 门禁只认增量的口径一致）。

用法：tools/check_i18n.py [--src DIR] [--selftest]
退出码：0 = 无违规；1 = 有违规；2 = 用法/自检失败。
"""

import argparse
import re
import sys
from pathlib import Path

# 用户可见文案入口：方法名后第一个字符串参数位置（0 = 第一参）。
SETTER_PATTERNS = [
    r"setText\s*\(", r"setToolTip\s*\(", r"setWindowModality\s*$",
    r"setWindowTitle\s*\(", r"setPlaceholderText\s*\(", r"setWhatsThis\s*\(",
    r"setStatusTip\s*\(", r"setAccessibleName\s*\(", r"setAccessibleDescription\s*\(",
    r"setLabelText\s*\(", r"setTitle\s*\(", r"setSubject\s*\(",
    r"setButtonText\s*\(", r"setInformativeText\s*\(", r"setTextVisible\s*$",
]
# 构造函数：类名( "literal", ... ) 的首参文案（后续参数多为 objectName/parent，
# 只查首参）。
CTOR_PATTERNS = [
    r"\bQLabel\s*\(", r"\bQPushButton\s*\(", r"\bQToolButton\s*\(",
    r"\bQCheckBox\s*\(", r"\bQRadioButton\s*\(", r"\bQAction\s*\(",
    r"\bQGroupBox\s*\(", r"\bQMenu\s*\(", r"\bQCommandLinkButton\s*\(",
]
# 动作入口：addAction("text"...)（QMenu/QToolBar 的文案重载）。
ACTION_PATTERNS = [r"\baddAction\s*\(", r"\baddTab\s*\(", r"\baddSubmenu\s*\("]

LITERAL_RE = re.compile(
    r'^(?:QStringLiteral|QLatin1String|QString::fromLatin1)\s*\(\s*"((?:[^"\\]|\\.)*)"'
    r'|^(?:u8)?"((?:[^"\\]|\\.)*)"'
)

TR_OK_RE = re.compile(
    r"^(?:tr\s*\(|QCoreApplication\s*::\s*translate|QObject\s*::\s*tr|qApp\s*->\s*translate)"
)

# 首参里出现这些开头即视为已走翻译机制或非字面量（变量/表达式/富文本拼接）。
def first_string_arg(call_open_match, line, src_lines, lineno):
    """返回 (kind, literal)——kind: 'literal' | 'tr' | 'other'。跨行取首参。"""
    pos = call_open_match.end()
    # 收集从当前行 pos 起最多 3 行拼成候选首参文本。
    chunk = line[pos:]
    depth = 1
    for extra in range(1, 4):
        if lineno + extra >= len(src_lines):
            break
        chunk += "\n" + src_lines[lineno + extra]
    arg = chunk.lstrip()
    if TR_OK_RE.match(arg):
        return ("tr", None)
    m = LITERAL_RE.match(arg)
    if m:
        return ("literal", m.group(1) or m.group(2))
    # 首参不是字面量也不是 tr：变量/表达式（含 tr(...).arg( 拼接——起头已是
    # TR_OK_RE 覆盖不到的 QStringLiteral("...").arg(tr(...)) 也按字面量报）。
    return ("other", None)


def literal_is_text(literal):
    """只把「真文案」计为违规：含 CJK，或含 ≥3 连续字母的 ASCII 词。

    本仓翻译源语言为中文（DESIGN.md 2026-09-29 决策）。纯占位模板
    （"%1 ms"、"%1, %2"、"1:500" 等——占位符 + 标点 + 单双字母单位）不
    计：参数侧已是翻译产物/数值，模板无词序可翻。英文词 ≥3 字母（如
    "Loading"）仍算文案；品牌名走 setCreator 等非扫描入口。
    """
    if any(ord(ch) > 0x2E7F for ch in literal):
        return True
    return re.search(r"[A-Za-z]{3,}", literal) is not None


def scan_file(path):
    violations = []
    try:
        text = path.read_text(encoding="utf-8")
    except (UnicodeDecodeError, OSError):
        return violations
    lines = text.splitlines()
    all_patterns = SETTER_PATTERNS + CTOR_PATTERNS + ACTION_PATTERNS
    combined = re.compile("|".join(f"(?:{p})" for p in all_patterns))
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("#"):
            continue
        for m in combined.finditer(line):
            kind, lit = first_string_arg(m, line, lines, i)
            if kind == "literal" and literal_is_text(lit):
                violations.append((path, i + 1, m.group(0).rstrip("(").strip(), lit))
    return violations


def run(src_dir):
    violations = []
    for path in sorted(Path(src_dir).rglob("*")):
        if path.suffix not in (".cpp", ".h", ".hpp"):
            continue
        if "vendor" in path.parts:
            continue
        violations.extend(scan_file(path))
    return violations


def selftest():
    import tempfile

    cases = [
        ('l->setText(tr("保存"));', False),
        ('l->setText(QStringLiteral("未翻译"));', True),
        ('l->setText("raw");', True),
        ('b->setToolTip(QStringLiteral("→"));', False),  # 纯符号
        ('auto *l = new QLabel(QStringLiteral("标题"));', True),
        ('menu->addAction(tr("打开"), this, &C::onOpen);', False),
        ('obj->setTitle(id());', False),
        ('// setText("comment only")', False),
    ]
    with tempfile.TemporaryDirectory() as td:
        f = Path(td) / "t.cpp"
        f.write_text("\n".join(c for c, _ in cases), encoding="utf-8")
        found = {(v[2], v[3]) for v in scan_file(f)}
    expected = {("setText", "未翻译"), ("setText", "raw"), ("QLabel", "标题")}
    ok = found == expected
    if not ok:
        print(f"selftest mismatch: found={found} expected={expected}", file=sys.stderr)
    return ok


def main():
    ap = argparse.ArgumentParser(description="i18n 裸字面量门禁")
    ap.add_argument("--src", default="src")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        sys.exit(0 if selftest() else 2)

    violations = run(args.src)
    for path, lineno, call, lit in violations:
        print(f"{path}:{lineno}: {call}( \"{lit}\" ) —— 用户可见文案未走 tr()")
    if violations:
        print(f"\n共 {len(violations)} 处裸字面量。文案入口必须经 tr()/translate()。")
        return 1
    print("i18n 检查通过：文案入口无裸字面量。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
