#!/usr/bin/env python3
"""Record source text/wiring guards and contrast from the existing theme tokens."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / 'docs/visual-polish'


def source_calls(source, name):
    # Preserve literals while removing comments. This is a textual guard; the
    # full behavioral/structural diff still requires human review.
    import check_ui_tokens
    source = check_ui_tokens.without_comments(source)
    calls = []
    pattern = (r'\b(?:QCoreApplication|QApplication|QObject)::translate\s*\('
               if name == 'translate' else r'\b' + name + r'\s*\(')
    for match in re.finditer(pattern, source):
        start = match.end()
        depth, quote, escaped = 1, None, False
        for end in range(start, len(source)):
            c = source[end]
            if quote:
                if escaped:
                    escaped = False
                elif c == '\\':
                    escaped = True
                elif c == quote:
                    quote = None
            elif c in ('"', "'"):
                quote = c
            elif c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
                if depth == 0:
                    call = source[start:end]
                    if name == 'connect':
                        call = call.split('{', 1)[0]  # signature before lambda body
                    calls.append(''.join(re.findall(r'"(?:\\.|[^"\\])*"|\S', call)))
                    break
    return calls


def luminance(color):
    channels = [int(color[i:i+2], 16) / 255 for i in (1, 3, 5)]
    values = [v / 12.92 if v <= .04045 else ((v + .055) / 1.055) ** 2.4 for v in channels]
    return sum(v * w for v, w in zip(values, (.2126, .7152, .0722)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default="HEAD", help="Git baseline for the production source review")
    args = parser.parse_args()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    files = subprocess.check_output(['git', 'diff', '--name-only', args.base, '--', 'src/ui'], cwd=ROOT, text=True).splitlines()
    checks = []
    for file in files:
        before = subprocess.check_output(['git', 'show', args.base + ':' + file], cwd=ROOT, text=True)
        after = (ROOT / file).read_text()
        checks.append(dict(file=file,
            translationSourceStringsUnchanged=all(source_calls(before, name) == source_calls(after, name) for name in ('tr', 'translate')),
            connectionSignaturesUnchanged=source_calls(before, 'connect') == source_calls(after, 'connect'),
            widgetLayoutConstructorCallsUnchanged=all(source_calls(before, name) == source_calls(after, name)
                for name in ('QWidget', 'QHBoxLayout', 'QVBoxLayout', 'QFormLayout', 'QGridLayout',
                             'QSplitter', 'QStackedWidget', 'QTabWidget', 'QTableWidget',
                             'QTreeWidget', 'QListWidget', 'QLineEdit', 'QComboBox'))))
    diff = subprocess.check_output(['git', 'diff', args.base, '--', 'src/ui'], cwd=ROOT)
    semantic = dict(baseGitCommit=subprocess.check_output(['git', 'rev-parse', args.base], cwd=ROOT, text=True).strip(),
                    productionDiffSha256=hashlib.sha256(diff).hexdigest(), checks=checks,
                    limitation='Textual guards only; full diff review recorded in ledger.')
    (OUTPUT / 'semantic-diff.json').write_text(json.dumps(semantic, ensure_ascii=False, indent=2) + '\n')
    source = (ROOT / 'src/ui/paleotheme.cpp').read_text()
    contrast = []
    pairs = [('text', 'surface'), ('text', 'surfaceAlt'), ('textMuted', 'surface'),
             ('textMuted', 'surfaceAlt'), ('primaryText', 'surface'),
             ('successText', 'successBg'), ('warningText', 'warningBg'),
             ('errorText', 'errorBg'), ('warningText', 'surface'), ('errorText', 'surface')]
    for theme in ('Light', 'Dark'):
        block = source.split('ThemeTokens k' + theme + ' = {', 1)[1].split('};', 1)[0]
        colors = dict(re.findall(r'/\*\.(\w+)\s*=\*/hx\("(#[A-Fa-f0-9]{6})"\)', block))
        for fg, bg in pairs + [('textDisabled', 'surface')]:
            a, b = luminance(colors[fg]), luminance(colors[bg])
            ratio = (max(a, b) + .05) / (min(a, b) + .05)
            exempt = fg == 'textDisabled'
            contrast.append(dict(theme=theme.lower(), foreground=fg, background=bg,
                                 ratio=round(ratio, 3), threshold=None if exempt else 4.5,
                                 status='inactive control exempt (DESIGN.md)' if exempt else 'pass' if ratio >= 4.5 else 'fail'))
    (OUTPUT / 'contrast.json').write_text(json.dumps(dict(source='src/ui/paleotheme.cpp kLight/kDark literal definitions', checks=contrast), indent=2) + '\n')
    failed = [c for c in checks if not c['translationSourceStringsUnchanged'] or not c['connectionSignaturesUnchanged']
              or not c['widgetLayoutConstructorCallsUnchanged']]
    failed += [c for c in contrast if c['status'] == 'fail']
    print(f'{len(checks)} UI source guards; {len(contrast)} contrast samples; {len(failed)} failures')
    for failure in failed:
        print(failure)
    return int(bool(failed))


if __name__ == '__main__':
    raise SystemExit(main())
