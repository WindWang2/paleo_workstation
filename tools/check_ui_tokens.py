#!/usr/bin/env python3
"""Inventory UI literals against DESIGN.md; exceptions are exact, counted sources."""

import argparse
from collections import Counter
import json
from pathlib import Path
import re
import tempfile

PATTERNS = {
    "hex": re.compile(r"#[0-9a-fA-F]{8}\b|#[0-9a-fA-F]{6}\b|#[0-9a-fA-F]{4}\b|#[0-9a-fA-F]{3}\b"),
    "packed-color": re.compile(r"QColor\(\s*0x[0-9a-fA-F]{6,8}\s*\)"),
    "rgb": re.compile(r"QColor\(\s*\d+\s*,\s*\d+\s*,\s*\d+(?:\s*,\s*\d+)?\s*\)"),
    "qt-color": re.compile(r"Qt::(?:white|black|gray|darkGray|lightGray)\b"),
    "font": re.compile(r"set(?:PointSizeF?|PixelSize)\(\s*\d+(?:\.\d+)?"),
    "font-constructor": re.compile(r'QFont(?:\s+\w+)?\(\s*QStringLiteral\(\s*"[^"]+"\s*\)\s*,\s*\d+'),
    "font-factory": re.compile(r"\b(?:bodyFont|monoFont|pointFont)\(\s*\d+(?:\.\d+)?"),
    "painter-radius": re.compile(r"drawRoundedRect\([^\n]*,\s*\d+(?:\.\d+)?\s*,\s*\d+(?:\.\d+)?\s*\)"),
    "qss": re.compile(r"(?:padding(?:-[a-z]+)?|margin(?:-[a-z]+)?|border(?:-[a-z-]+)?-radius|font-size):[^;\"\n]*\d+(?:px|pt)"),
    "layout": re.compile(r"(?:setContentsMargins\(\s*\d+(?:\s*,\s*\d+){3}\s*\)|(?:set(?:Horizontal|Vertical)?Spacing|addSpacing)\(\s*[1-9]\d*\s*\)|FlowLayout\([^\n]*?\b(?:spacing\s*=\s*[1-9]\d*|,\s*[1-9]\d*\s*\)))"),
}
TOKEN_SOURCES = {"src/ui/paleotheme.h", "src/ui/paleotheme.cpp"}


def without_comments(text):
    # Keep string/character literals intact, including escaped quotes, while
    # replacing comments with whitespace to preserve source line numbers.
    pattern = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/', re.S)
    return pattern.sub(lambda m: re.sub(r"[^\n]", " ", m.group())
                       if m.group().startswith(("//", "/*")) else m.group(), text)


def scan(root):
    ui = root / "src/ui"
    if not ui.is_dir():
        raise ValueError(f"missing scan root: {ui}")
    found = []
    for path in sorted(ui.rglob("*")):
        if path.suffix not in (".cpp", ".h"):
            continue
        relative = path.relative_to(root).as_posix()
        if relative in TOKEN_SOURCES:
            continue
        original = path.read_text().splitlines()
        for number, code in enumerate(without_comments(path.read_text()).splitlines(), 1):
            for rule, pattern in PATTERNS.items():
                if rule == "layout" and re.search(r"setContentsMargins\(\s*0,\s*0,\s*0,\s*0\s*\)", code):
                    continue
                for match in pattern.finditer(code):
                    if rule == "qss" and not re.search(r"(?<![%\d])\b\d+(?:px|pt)\b", match.group()):
                        continue
                    found.append({"file": relative, "line": number, "rule": rule,
                                  "literal": match.group(), "source": code.strip(),
                                  "original": original[number - 1].strip()})
    return found


def classify(found, exceptions):
    allowed = {}
    for item in exceptions:
        if not item.get("reason") or item.get("count", 0) < 1:
            raise ValueError("exceptions need a concrete reason and positive count")
        key = (item["file"], item["rule"], item["source"])
        if key in allowed:
            raise ValueError(f"duplicate exception: {key}")
        allowed[key] = item
    used = Counter()
    violations = []
    for item in found:
        key = (item["file"], item["rule"], item["source"])
        exception = allowed.get(key)
        if exception and used[key] < exception["count"]:
            item["exception"] = exception["reason"]
            used[key] += 1
        else:
            violations.append(item)
    stale = [list(key) for key, item in allowed.items() if used[key] != item["count"]]
    return violations, stale


def selftest():
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        ui = root / "src/ui"
        ui.mkdir(parents=True)
        (ui / "paleotheme.cpp").write_text('auto color = "#123456";')
        (ui / "example.cpp").write_text(
            '// ignored #123456 setPointSize(7)\n'
            'auto color = "#123456"; /* ignored #ffffff */\n'
            'auto alpha = QColor(1, 2, 3, 4);\n'
            'auto packed = QColor(0x123456);\n'
            'auto white = Qt::white;\n'
            'font.setPointSize(7);\n'
            'auto mono = PaleoTheme::monoFont(7);\n'
            'lay->setContentsMargins(0, 0, 0, 0);\n'
            'lay->setContentsMargins(0, 4, 0, 4);\n'
            'lay->setContentsMargins(0, t.spacingXs, 0, t.spacingXs);\n'
            'grid->setHorizontalSpacing(10); grid->setVerticalSpacing(6);\n'
            'lay->addSpacing(16); lay->addSpacing(t.spacingMd);\n'
            'auto flow = new FlowLayout(host, 0, 4);\n'
            'explicit FlowLayout(QWidget *parent, int margin = 0, int spacing = 4);\n'
            'p.drawRoundedRect(QRectF(x, y, w, h), 2.0, 2.0);\n'
            'auto sheet = "padding: 4px; border-radius: %8px;";\n'
            'auto url = "http://example/#abcdef";\n')
        found = scan(root)
        assert Counter(m["rule"] for m in found) == {
            "hex": 2, "rgb": 1, "packed-color": 1, "qt-color": 1, "font": 1, "layout": 6, "qss": 1, "painter-radius": 1, "font-factory": 1}
        item = found[0]
        exception = dict(file=item["file"], rule=item["rule"], source=item["source"],
                         count=1, reason="Fixture data symbol")
        violations, stale = classify(found, [exception])
        assert len(violations) == len(found) - 1 and not stale
        _, stale = classify([], [exception])
        assert stale
        duplicated = found + [item.copy()]
        violations, _ = classify(duplicated, [exception])
        assert len(violations) == len(found)  # extra identical literal is rejected
        try:
            classify(found, [exception, exception])
        except ValueError:
            pass
        else:
            raise AssertionError("duplicate whitelist entry accepted")
    print("UI token scanner selftest passed")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--report", type=Path)
    parser.add_argument("--inventory", action="store_true", help="record candidates without claiming a clean scan")
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    root = args.root.resolve()
    exceptions_path = root / "tools/ui-token-exceptions.json"
    exceptions = json.loads(exceptions_path.read_text()) if exceptions_path.exists() else []
    found = scan(root)
    violations, stale = classify(found, exceptions)
    report = {"candidates": len(found), "byRule": dict(Counter(item["rule"] for item in found)),
              "exceptions": len(found) - len(violations), "violations": len(violations),
              "staleExceptions": stale, "matches": found}
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({k: v for k, v in report.items() if k != "matches"}, ensure_ascii=False))
    if not args.inventory:
        for item in violations:
            print(f"FAIL {item['file']}:{item['line']} {item['rule']}: {item['literal']}")
        return int(bool(violations or stale))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
