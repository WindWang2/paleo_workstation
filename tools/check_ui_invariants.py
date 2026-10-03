#!/usr/bin/env python3
# UI 不变量机械检查（goal/ui-experience-polish；DESIGN.md 权威）。
#
# 用法：
#   check_ui_invariants.py [--strict] [--src ROOT] [--baseline PATH]
#   check_ui_invariants.py --selftest
#   未知参数 exit 2（同 check_layering 口径——argv 被静默忽略即假绿）。
#
# 规则面：
#   1. motion（DESIGN.md Motion「minimal-functional：即时切换为主；无编排
#      动画」）：src/ui/** 禁止 QPropertyAnimation / QVariantAnimation /
#      QEasingCurve / setAnimated(true)。
#   2. qss-hex（token 单一真源 = src/ui/paleotheme.{h,cpp}）：src/ui/** 的
#      setStyleSheet 调用里禁止裸 hex 色值（#RRGGBB / #RGB）。
#      QColor 构造 / QPainter pen-brush 是数据符号色出口（DESIGN.md「地图
#      域配色不属 UI token」），不在扫描面；paleotheme.cpp 是 token 字面量
#      落地处、整文件豁免。
#      合法残留走 baseline（同 layering-baseline 惯例：只缩不涨）。
#
# baseline 格式：每行 `<relpath>:<rule>`；`#` 开头注释/空行忽略。
# --strict 下 baseline 非空即红（防回升）；非 strict 命中 baseline 降级提示。

import re
import shutil
import tempfile
import sys
from pathlib import Path

MOTION_TOKENS = ("QPropertyAnimation", "QVariantAnimation", "QEasingCurve")
HEX_RE = re.compile(r"#[0-9A-Fa-f]{6}\b|#[0-9A-Fa-f]{3}\b")
SETSTYLE_RE = re.compile(r"setStyleSheet\s*\(")

EXEMPT_FILES = ("src/ui/paleotheme.cpp", "src/ui/paleotheme.h")


def scan(root: Path):
    """返回 (violations, baseline_keys)：violations 带行号供定位，
    baseline_keys 形如 `<relpath>:<rule>` 与 baseline 文件对齐。"""
    violations = []
    ui_root = root / "src" / "ui"
    if not ui_root.is_dir():
        # 扫描根不存在 = 定位错误（ctest 的 cwd 是构建目录），不是「零违规」。
        raise FileNotFoundError(f"scan root not found: {ui_root}")
    files = sorted(ui_root.rglob("*.?pp"))
    if not files:
        raise FileNotFoundError(f"scan root has no sources: {ui_root}")
    for path in files:
        rel = path.relative_to(root).as_posix()
        text = path.read_text(encoding="utf-8", errors="replace")
        # setStyleSheet(...) 实参可能跨行——先按语句聚合到调用起始行。
        for m in re.finditer(r"setStyleSheet\s*\(", text):
            start = text.count("\n", 0, m.start()) + 1
            window = text[m.end():m.end() + 400]
            # 截到平衡右括号（近似：下一个未配对的 ')'）。
            depth, cut = 1, 0
            for ch in window:
                cut += 1
                if ch == "(":
                    depth += 1
                elif ch == ")":
                    depth -= 1
                    if depth == 0:
                        break
            if rel not in EXEMPT_FILES:
                hx = HEX_RE.search(window[:cut])
                if hx:
                    violations.append((f"{rel}:qss-hex",
                                       f"{rel}:{start}: {hx.group()}"))
        for i, line in enumerate(text.splitlines(), 1):
            if "setAnimated(true)" in line or any(t in line for t in MOTION_TOKENS):
                violations.append((f"{rel}:motion", f"{rel}:{i}"))
    return violations


def selftest():
    # 规则自检：正例/反例各一，扫描器自身坏掉不能静默变绿。
    # Windows CI：无 /tmp、默认编码 cp1252——用系统临时目录 + 显式 utf-8。
    fake = Path(tempfile.gettempdir()) / "uipolish_selftest" / "src" / "ui"
    if fake.parent.parent.exists():
        shutil.rmtree(fake.parent.parent)
    fake.mkdir(parents=True)
    (fake / "ok.cpp").write_text(
        'l->setStyleSheet(PaleoTheme::mutedCaptionStyleSheet());\n'
        'c.setPen(QPen(QColor("#DFE5EC"), 1.0));  // 数据符号色不在规则面\n',
        encoding="utf-8")
    (fake / "bad.cpp").write_text(
        'w->setStyleSheet(\n    QStringLiteral("color: #5D6E80;"));\n'
        'auto *a = new QPropertyAnimation(w, "pos");\n'
        'tree->setAnimated(true);\n', encoding="utf-8")
    v = scan(fake.parent.parent)
    keys = sorted(k for k, _ in v)
    ok = keys == ["src/ui/bad.cpp:motion", "src/ui/bad.cpp:motion",
                  "src/ui/bad.cpp:qss-hex"]
    shutil.rmtree(fake.parent.parent)
    print("selftest:", "PASS" if ok else "FAIL", keys)
    return 0 if ok else 1


def main():
    args = sys.argv[1:]
    if "--selftest" in args:
        return selftest()
    strict = "--strict" in args
    # 用脚本自身位置定位仓库根（同 check_layering.py）——不依赖 cwd，
    # ctest 在构建目录下运行也扫真源码树（#77）。
    root = Path(__file__).resolve().parents[1]
    baseline_path = root / "tools" / "ui-invariants-baseline.txt"
    i = 0
    rest = []
    while i < len(args):
        if args[i] == "--src" and i + 1 < len(args):
            root = Path(args[i + 1]).resolve()
            i += 2
        elif args[i] == "--baseline" and i + 1 < len(args):
            baseline_path = Path(args[i + 1]).resolve()
            i += 2
        elif args[i] == "--strict":
            i += 1
        else:
            rest.append(args[i])
            i += 1
    if rest:
        print("unknown argument(s):", " ".join(rest))
        return 2

    baseline = set()
    if baseline_path.exists():
        for ln in baseline_path.read_text(encoding="utf-8").splitlines():
            ln = ln.strip()
            if ln and not ln.startswith("#"):
                baseline.add(ln)

    try:
        violations = scan(root)
    except FileNotFoundError as e:
        print("ERROR", e)
        return 2
    keys = {k for k, _ in violations}
    fresh = [(k, d) for k, d in violations if k not in baseline]
    stale = sorted(baseline - keys)
    rc = 0
    for _, detail in sorted(set(fresh)):
        print("FAIL", detail)
        rc = 1
    for b in stale:
        print("NOTE baseline entry already clean (shrink it):", b)
    if strict and baseline:
        print(f"FAIL strict mode: baseline must be empty (has {len(baseline)})")
        rc = 1
    if rc == 0:
        print(f"ui invariants: clean (violations={len(fresh)}, baseline={len(baseline)})")
    return rc


if __name__ == "__main__":
    sys.exit(main())
