#!/usr/bin/env python3
"""#750：英文翻译骨架 translations/paleo_en.ts。

源码文案是中文 tr()，英文界面需要一份 en 目录。本脚本以 paleo_zh_CN.ts 的
<context>/<source> 集合为准生成/补齐 paleo_en.ts：
  - 已有英文译文保留（按 context+source+comment 匹配）；
  - 新条目写 <translation type="unfinished"></translation>（numerus 同理）；
  - zh_CN 中已消失的条目丢弃。
用法：
  python3 tools/sync_en_ts_skeleton.py           # 写出/刷新 paleo_en.ts
  python3 tools/sync_en_ts_skeleton.py --check   # 条目集与 zh_CN 不一致 → 退出 1
  python3 tools/sync_en_ts_skeleton.py --selftest
lupdate 刷新（tools/update_translations.sh 扫 translations/*.ts）后再跑一次本脚本。
"""
import os
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ZH = os.path.join(ROOT, 'translations', 'paleo_zh_CN.ts')
EN = os.path.join(ROOT, 'translations', 'paleo_en.ts')


def _key(ctx, msg):
    src = msg.findtext('source') or ''
    com = msg.findtext('comment') or ''
    return (ctx, src, com)


def _keys(path):
    tree = ET.parse(path)
    out = {}
    for c in tree.getroot().iter('context'):
        name = c.findtext('name') or ''
        for m in c.iter('message'):
            out[_key(name, m)] = m
    return tree, out


def build(zh_path, en_path):
    zh_tree, _ = _keys(zh_path)
    existing = {}
    if os.path.exists(en_path):
        _, existing = _keys(en_path)
    root = zh_tree.getroot()
    root.set('language', 'en')
    root.set('sourcelanguage', 'zh_CN')
    for c in root.iter('context'):
        name = c.findtext('name') or ''
        for m in c.iter('message'):
            old = existing.get(_key(name, m))
            t = m.find('translation')
            if t is None:
                t = ET.SubElement(m, 'translation')
            for child in list(t):
                t.remove(child)
            t.text = None
            t.attrib.clear()
            ot = old.find('translation') if old is not None else None
            if ot is not None and ot.get('type') != 'unfinished' and (
                    (ot.text or '').strip() or len(ot)):
                t.text = ot.text
                for child in ot:
                    t.append(child)
                continue
            t.set('type', 'unfinished')
            if m.get('numerus') == 'yes':
                nf = ET.SubElement(t, 'numerusform')
                nf.text = ''
    ET.indent(zh_tree, space='    ')
    with open(en_path, 'wb') as f:
        f.write(b'<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE TS>\n')
        zh_tree.write(f, encoding='utf-8', xml_declaration=False)
        f.write(b'\n')


def check(zh_path, en_path):
    if not os.path.exists(en_path):
        print('sync_en_ts_skeleton: FAIL paleo_en.ts missing')
        return 1
    _, zk = _keys(zh_path)
    _, ek = _keys(en_path)
    missing = set(zk) - set(ek)
    extra = set(ek) - set(zk)
    print(f'sync_en_ts_skeleton: zh={len(zk)} en={len(ek)} missing={len(missing)} extra={len(extra)}')
    if missing or extra:
        print('sync_en_ts_skeleton: FAIL — run python3 tools/sync_en_ts_skeleton.py')
        return 1
    return 0


def selftest():
    with tempfile.TemporaryDirectory() as t:
        zh = os.path.join(t, 'zh.ts')
        en = os.path.join(t, 'en.ts')
        open(zh, 'w', encoding='utf-8').write(
            '<?xml version="1.0" encoding="utf-8"?><!DOCTYPE TS><TS version="2.1" language="zh_CN">'
            '<context><name>A</name>'
            '<message><source>新建</source><translation>新建</translation></message>'
            '<message numerus="yes"><source>%n 口井</source><translation><numerusform>%n 口井</numerusform></translation></message>'
            '</context></TS>')
        build(zh, en)
        assert check(zh, en) == 0
        _, ek = _keys(en)
        m = ek[('A', '新建', '')]
        assert m.find('translation').get('type') == 'unfinished'
        assert not (m.find('translation').text or '').strip()
        # 保留已有译文
        txt = open(en, encoding='utf-8').read().replace(
            '<translation type="unfinished" />', '<translation>New</translation>', 1)
        open(en, 'w', encoding='utf-8').write(txt)
        build(zh, en)
        _, ek = _keys(en)
        assert ek[('A', '新建', '')].find('translation').text == 'New'
        # zh 增条目 → check 失败
        zh_txt = open(zh, encoding='utf-8').read()
        open(zh, 'w', encoding='utf-8').write(zh_txt.replace(
            '</context>', '<message><source>打开</source><translation>打开</translation></message></context>'))
        assert check(zh, en) == 1
    print('sync_en_ts_skeleton selftest: OK')
    return 0


if __name__ == '__main__':
    if '--selftest' in sys.argv:
        sys.exit(selftest())
    if '--check' in sys.argv:
        sys.exit(check(ZH, EN))
    build(ZH, EN)
    sys.exit(check(ZH, EN))
