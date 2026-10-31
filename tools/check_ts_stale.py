#!/usr/bin/env python3
# 层：测试壳
"""PW-044：translations/*.ts 陈旧度棘轮。

背景：paleo_zh_CN.ts 有 6341 条消息、0 unfinished，但 src/ 中约 2500 个不同
的 tr("...") 字面量根本不在 .ts 里（自上次 lupdate 以来新增的字符串，英文源串
会以英文显示给中文用户）。check_i18n/check_l10n 只查裸字面量，不查陈旧。

本门禁统计「src/ 中 tr()/QT_TR_NOOP/translate 的字面量不在 .ts <source> 中」的
不同字面量数，与 tools/ts_stale_baseline.json 比对：只许降不许升。修复路径：
运行 lupdate 更新 .ts（并补译文），然后 `--update-baseline` 把基线降下来。

用法：
  check_ts_stale.py [--root PATH] [--update-baseline] [--verbose]
  check_ts_stale.py --selftest
未知参数 exit 2。
"""
import json
import os
import re
import sys
import tempfile
import xml.etree.ElementTree as ET

TR_RE = re.compile(
    r'\b(?:tr|QT_TR_NOOP|QT_TRANSLATE_NOOP\s*\(\s*"[^"]*"\s*,|translate\s*\(\s*"[^"]*"\s*,)\s*\(?\s*'
    r'((?:"(?:[^"\\]|\\.)*"\s*)+)')
LIT_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
ESC = {'n': '\n', 't': '\t', '"': '"', '\\': '\\', "'": "'"}


def unescape(s):
    return re.sub(r'\\(.)', lambda m: ESC.get(m.group(1), '\\' + m.group(1)), s)


# lupdate 看不见的调用面（#866）：文件内自定义的自由函数 tr() 包装、以及 obj->tr()/obj.tr()。
# 这些字面量无论跑多少次 lupdate 都进不了 .ts，计入棘轮会让「跑 lupdate」这一修复路径失效，
# 故单独计数仅作提示（--verbose 列出），不入门禁。
FREE_TR_RE = re.compile(r'^\s*(?:static\s+)?(?:inline\s+)?QString\s+tr\s*\(', re.M)
MEMBER_TR_RE = re.compile(r'(?:->|\.)\s*$')


def src_literals(root, invisible=None):
    out = set()
    for d, _, files in os.walk(os.path.join(root, 'src')):
        for f in files:
            if not f.endswith(('.cpp', '.h', '.hpp', '.cc')):
                continue
            try:
                text = open(os.path.join(d, f), encoding='utf-8', errors='replace').read()
            except OSError:
                continue
            free_wrapper = bool(FREE_TR_RE.search(text))
            for m in TR_RE.finditer(text):
                lit = ''.join(unescape(x) for x in LIT_RE.findall(m.group(1)))
                if not lit:
                    continue
                line_start = text.rfind('\n', 0, m.start()) + 1
                if '//' in text[line_start:m.start()]:
                    continue  # 行注释里的 tr("…") 不是调用
                bare_tr = text.startswith('tr', m.start())
                if bare_tr and (free_wrapper or MEMBER_TR_RE.search(text[max(0, m.start() - 8):m.start()])):
                    if invisible is not None:
                        invisible.add(lit)
                    continue
                out.add(lit)
    return out


def ts_sources(root):
    out = set()
    tdir = os.path.join(root, 'translations')
    if not os.path.isdir(tdir):
        return out
    for f in sorted(os.listdir(tdir)):
        if f.endswith('.ts'):
            for s in ET.parse(os.path.join(tdir, f)).getroot().iter('source'):
                out.add(s.text or '')
    return out


def missing_count(root, invisible=None):
    return len(src_literals(root, invisible) - ts_sources(root))


def run(root, update, verbose=False):
    inv = set()
    n = missing_count(root, inv)
    inv -= ts_sources(root)
    if inv:
        print(f'check_ts_stale: note {len(inv)} literal(s) via free tr() wrapper / obj->tr() are invisible to lupdate (not gated)')
        if verbose:
            for s in sorted(inv):
                print(f'  invisible: {s!r}')
    bpath = os.path.join(root, 'tools', 'ts_stale_baseline.json')
    if update:
        with open(bpath, 'w', encoding='utf-8') as fh:
            json.dump({'missing_from_ts': n}, fh, indent=2)
            fh.write('\n')
        print(f'check_ts_stale: baseline updated -> {n}')
        return 0
    base = json.load(open(bpath, encoding='utf-8'))['missing_from_ts'] if os.path.exists(bpath) else None
    print(f'check_ts_stale: {n} distinct tr() literals missing from translations/*.ts (baseline {base})')
    if base is None:
        print('check_ts_stale: no baseline; run --update-baseline')
        return 1
    if n > base:
        print(f'check_ts_stale: FAIL {n} > baseline {base} — run lupdate on translations/*.ts for new strings')
        return 1
    if n < base:
        print(f'::notice::check_ts_stale: {n} < baseline {base}; ratchet down with --update-baseline')
    return 0


def selftest():
    with tempfile.TemporaryDirectory() as t:
        os.makedirs(os.path.join(t, 'src'))
        os.makedirs(os.path.join(t, 'translations'))
        os.makedirs(os.path.join(t, 'tools'))
        open(os.path.join(t, 'src', 'a.cpp'), 'w').write(
            'x = tr("Hello"); y = tr("multi " "part"); z = QCoreApplication::translate("Ctx", "Ctx str");\n')
        open(os.path.join(t, 'translations', 'x.ts'), 'w').write(
            '<TS><context><message><source>Hello</source></message></context></TS>')
        assert missing_count(t) == 2, missing_count(t)
        assert run(t, True) == 0 and run(t, False) == 0
        open(os.path.join(t, 'src', 'b.cpp'), 'w').write('tr("New");')
        assert run(t, False) == 1
        # #866：lupdate 看不见的调用面不入门禁
        open(os.path.join(t, 'src', 'b.cpp'), 'w').write(
            'namespace {\nQString tr(const char *s) { return QCoreApplication::translate("C", s); } }\n'
            'f(tr("Wrapped"));')
        open(os.path.join(t, 'src', 'c.cpp'), 'w').write('g(guard->tr("Member")); h(w.tr("Dot"));\n// old tr("Comment")\n')
        inv = set()
        assert missing_count(t, inv) == 2 and inv == {'Wrapped', 'Member', 'Dot'}, inv
        assert run(t, False) == 0
    print('check_ts_stale selftest OK')
    return 0


def main(argv):
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    update = False
    verbose = False
    it = iter(argv)
    for a in it:
        if a == '--selftest':
            return selftest()
        if a == '--update-baseline':
            update = True
        elif a == '--verbose':
            verbose = True
        elif a == '--root':
            root = next(it)
        else:
            print(f'unknown arg {a}', file=sys.stderr)
            return 2
    return run(root, update, verbose)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
