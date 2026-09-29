#!/usr/bin/env python3
# 层边界机械检查（docs/UI_LAYER_PLAN.md W6）。
#
# 用法：
#   check_layering.py [--strict] [--vocab PATH] [--baseline PATH]
#   check_layering.py --selftest
#   未知参数一律 exit 2（历史上 argv 被静默忽略，--strict 假绿——已修）。
#
# 规则面：
#   1. include 一律先规范化为仓库相对路径再匹配——`../io/x.h`、`../../io/x.h`
#      与 `io/x.h` 归一为 `io/x.h`，防 ../ 前缀绕过。
#   2. 正向（视图层出边）：src/ui/** 只允许 include
#        - io/lasdoc.h          （io/* 白名单制，其余 io/* 一律失败）
#        - metadata/{layermanifest,paleoprojectstore,mapversionstore,releasestore}.h
#      且 algorithms/* 一律失败。
#   3. 反向：non_view（见 tools/layering_vocab.json）目录出现 ui/* include 即失败。
#      src/app、src/selfcheck 不扫（组装根/测试壳按契约允许 ui/ 依赖，
#      by design 豁免）。
#   4. QtWidgets 禁令（non_view 去掉 qgis；仅 QGIS 封装豁免）：词表制三类写法——
#      `#include <QtWidgets…>`、`#include <QWidget>` 等单类头、
#      `class Q…;` 前向声明。
#   5. 层标记：每个 src/ 文件头三行内必须有 `// 层：<词表之一>` 且与所属目录对应。
#      词表外置在 tools/layering_vocab.json——新增顶层模块先登记词表
#      （scripts/new_module.sh 会同步），否则全量判违规。
#
# 合法残留走 tools/layering-baseline.txt（格式：每行 `<path>:<rule>`，
# `#` 开头注释）。命中 baseline 的违规降级为提示；baseline 里已修复的条目
# 提示可收缩。收敛方式：修代码后把该行从 baseline 删掉——baseline 只缩不涨。
#
# --strict：闸门语义，防 baseline 回升。以下任一即 fail：
#   - baseline 非空（残留只许收缩，当前已归零，任何新增都是回升）；
#   - baseline 存在可收缩条目（对应违规已修复却仍占着表）。
# 非严格模式维持提示语义（可收缩 NOTE 非 fail）。
#
# `--selftest` 跑内置正/反夹具 + strict 语义夹具（不进 ctest 就是哑护栏）。

import json
import os
import re
import sys

# Windows 控制台缺省 cp1252 无法编码中文输出（CI 上 UnicodeEncodeError
# 即失败）。任何 print 前把三流重配为 UTF-8；非 TTY（ctest 管道）安全。
for _s in (sys.stdout, sys.stderr):
    try:
        if _s and hasattr(_s, "reconfigure"):
            _s.reconfigure(encoding="utf-8", errors="replace")
    except (ValueError, OSError):
        pass

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "src")
VOCAB = os.path.join(REPO, "tools", "layering_vocab.json")
BASELINE = os.path.join(REPO, "tools", "layering-baseline.txt")


def load_vocab(path=VOCAB):
    """词表外置的单点事实；缺失/坏 JSON 直接 exit 2（哑配置不能静默变绿）。"""
    try:
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)
        layers = {k: str(v) for k, v in data["layers"].items()}
        non_view = [str(x) for x in data["non_view"]]
        ui_io = set(data["ui_io_whitelist"])
        ui_meta = set(data["ui_metadata_whitelist"])
    except (OSError, KeyError, TypeError, ValueError) as exc:
        print(f"FAIL 词表 {path} 不可用：{exc}", file=sys.stderr)
        print("       新模块须先登记 tools/layering_vocab.json（scripts/new_module.sh 会同步）。",
              file=sys.stderr)
        sys.exit(2)
    unknown_nv = [d for d in non_view if d not in layers]
    if unknown_nv:
        print(f"FAIL 词表自相矛盾：non_view {unknown_nv} 不在 layers 中", file=sys.stderr)
        sys.exit(2)
    return layers, non_view, ui_io, ui_meta


LAYERS, NON_VIEW, UI_IO_WHITELIST, UI_METADATA_WHITELIST = load_vocab()
LAYER_VOCAB = set(LAYERS.values())
QTWIDGETS_BAN = [d for d in NON_VIEW if d != "qgis"]

# QtWidgets 单类头/前向声明词表（只列控件类；QtCore/QtGui 通用类不在列）。
QTWIDGETS_CLASSES = {
    "QAbstractButton", "QAbstractItemDelegate", "QAbstractItemView",
    "QAbstractScrollArea", "QAbstractSlider", "QAbstractSpinBox",
    "QAction", "QActionGroup", "QButtonGroup", "QCalendarWidget",
    "QCheckBox", "QColumnView", "QComboBox", "QCommandLinkButton",
    "QCompleter", "QDataWidgetMapper", "QDateEdit", "QDateTimeEdit",
    "QDial", "QDialog", "QDialogButtonBox", "QDockWidget",
    "QDoubleSpinBox", "QErrorMessage", "QFileDialog", "QFileSystemModel",
    "QFocusFrame", "QFontComboBox", "QFormLayout", "QFrame",
    "QGesture", "QGestureEvent", "QGraphicsAnchorLayout",
    "QGraphicsEffect", "QGraphicsGridLayout", "QGraphicsItem",
    "QGraphicsLayout", "QGraphicsLayoutItem", "QGraphicsLinearLayout",
    "QGraphicsObject", "QGraphicsPixmapItem", "QGraphicsProxyWidget",
    "QGraphicsRectItem", "QGraphicsScene", "QGraphicsSceneEvent",
    "QGraphicsSimpleTextItem", "QGraphicsTextItem", "QGraphicsView",
    "QGraphicsWidget", "QGridLayout", "QGroupBox", "QHBoxLayout",
    "QHeaderView", "QInputDialog", "QItemDelegate", "QItemEditorFactory",
    "QKeySequenceEdit", "QLCDNumber", "QLabel", "QLayout", "QLayoutItem",
    "QLineEdit", "QListView", "QListWidget", "QListWidgetItem",
    "QMainWindow", "QMdiArea", "QMdiSubWindow", "QMenu", "QMenuBar",
    "QMessageBox", "QOpenGLWidget", "QPanGesture", "QPinchGesture",
    "QPlainTextEdit", "QProgressBar", "QProgressDialog", "QPushButton",
    "QRadioButton", "QRubberBand", "QScrollArea", "QScrollBar",
    "QScroller", "QShortcut", "QSizeGrip", "QSlider", "QSpinBox",
    "QSplashScreen", "QSplitter", "QSplitterHandle", "QStackedLayout",
    "QStackedWidget", "QStatusBar", "QStyleFactory", "QStyleOption",
    "QStyledItemDelegate", "QSwipeGesture", "QSystemTrayIcon",
    "QTabBar", "QTabWidget", "QTableView", "QTableWidget",
    "QTableWidgetItem", "QTapGesture", "QTextBrowser", "QTextEdit",
    "QTimeEdit", "QToolBar", "QToolBox", "QToolButton", "QToolTip",
    "QTreeView", "QTreeWidget", "QTreeWidgetItem", "QUndoCommand",
    "QUndoGroup", "QUndoStack", "QUndoView", "QVBoxLayout", "QWhatsThis",
    "QWidget", "QWidgetAction", "QWidgetItem", "QWizard", "QWizardPage",
}

INC_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')
FWD_RE = re.compile(r'^\s*class\s+(Q[A-Za-z0-9_]+)\s*;')


def norm_include(file_dir, inc):
    """把 include 归一为仓库相对路径（以 src/ 为根）。失败返回 None。"""
    top_prefixes = tuple(d + "/" for d in LAYERS)
    if inc.startswith(top_prefixes):
        return inc
    cand = os.path.normpath(os.path.join(file_dir, inc))
    rel = os.path.relpath(cand, SRC)
    if rel.startswith(".."):
        return None
    return rel.replace(os.sep, "/")


def top_dir(rel):
    return rel.split("/", 1)[0]


def iter_sources():
    for root, _dirs, files in os.walk(SRC):
        for f in sorted(files):
            if f.endswith((".h", ".cpp")):
                yield os.path.join(root, f)


def check_file(path):
    """返回 [(rule, line_no, text)] 违规列表。"""
    rel = os.path.relpath(path, SRC).replace(os.sep, "/")
    layer_dir = top_dir(rel)
    file_dir = os.path.dirname(path)
    violations = []
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            lines = fh.read().split("\n")
    except OSError as exc:
        return [("read-error", 0, str(exc))]

    # W5c 层标记：头三行必须有 `// 层：<词表值>` 且与目录所属层严格一致
    head = "\n".join(lines[:3])
    m = re.search(r"//\s*层：\s*([^\n]+)", head)
    tag = m.group(1).strip() if m else None
    expected_layer = LAYERS.get(layer_dir)
    if not tag or tag not in LAYER_VOCAB:
        violations.append(("layer-marker", 0,
                           f"头三行缺 `// 层：<{'|'.join(sorted(LAYER_VOCAB))}>`"))
    elif tag != expected_layer:
        violations.append(("layer-marker-mismatch", 0,
                           f"层标记不匹配：标注为 `// 层：{tag}`，所属目录 `{layer_dir}` 应为 `// 层：{expected_layer}`"))

    for n, line in enumerate(lines, 1):
        inc = INC_RE.match(line)
        if inc:
            norm = norm_include(file_dir, inc.group(1))
            if norm is not None:
                dst = top_dir(norm)
                if layer_dir == "ui":
                    if dst == "io" and norm not in UI_IO_WHITELIST:
                        violations.append(("ui-io-include", n, line.strip()))
                    elif dst == "metadata" and norm not in UI_METADATA_WHITELIST:
                        violations.append(("ui-metadata-include", n, line.strip()))
                    elif dst == "algorithms":
                        violations.append(("ui-algorithms-include", n, line.strip()))
                elif layer_dir in NON_VIEW and dst == "ui":
                    violations.append(("reverse-ui-include", n, line.strip()))

            if layer_dir in QTWIDGETS_BAN:
                name = inc.group(1)
                if name.startswith("QtWidgets"):
                    violations.append(("qtwidgets-include", n, line.strip()))
                elif name in QTWIDGETS_CLASSES:
                    violations.append(("qtwidgets-include", n, line.strip()))
        else:
            if layer_dir in QTWIDGETS_BAN:
                fwd = FWD_RE.match(line)
                if fwd and fwd.group(1) in QTWIDGETS_CLASSES:
                    violations.append(("qtwidgets-fwd-decl", n, line.strip()))
    return violations


def load_baseline(path=BASELINE):
    entries = {}
    if not os.path.exists(path):
        return entries
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            path_, _, rule = line.partition(":")
            entries.setdefault(path_.strip(), set()).add(rule.strip())
    return entries


def run_check(strict=False, baseline_path=None):
    baseline = load_baseline(baseline_path or BASELINE)
    hits = {}          # baseline 中被命中的条目
    failures = []      # baseline 外的真违规
    for path in iter_sources():
        rel = os.path.relpath(path, SRC).replace(os.sep, "/")
        rel_src = os.path.join("src", rel)
        for rule, lineno, text in check_file(path):
            if rule in baseline.get(rel_src, set()):
                hits.setdefault(rel_src, set()).add(rule)
                continue
            failures.append((rel_src, rule, lineno, text))

    shrunk = []
    for path, rules in baseline.items():
        missing = rules - hits.get(path, set())
        for rule in sorted(missing):
            shrunk.append(f"{path}:{rule}")

    for rel_src, rule, lineno, text in sorted(failures):
        print(f"FAIL {rel_src}:{lineno} [{rule}] {text}")
    for entry in sorted(shrunk):
        print(f"NOTE baseline 可收缩：{entry}（对应违规已修复，请删除该行）")

    if failures:
        print(f"\n{len(failures)} 处层违规。合法残留须逐行列入 "
              f"tools/layering-baseline.txt（格式 <path>:<rule>），"
              f"修代码后删除对应行——baseline 只缩不涨。")
        return 1
    if shrunk:
        # --strict 下可收缩条目也算 fail：残留只许收缩，修完必须删行。
        if strict:
            print(f"\n--strict：baseline 有 {len(shrunk)} 条可收缩——"
                  f"对应违规已修复，删除这些行后再合入。")
            return 1
        print(f"\n检查通过；baseline 有 {len(shrunk)} 条可收缩（非失败）。")
        return 0
    if strict and baseline:
        # baseline 非空即 fail：当前已归零，任何新增条目都是回升。
        n = sum(len(r) for r in baseline.values())
        print(f"\n--strict：baseline 非空（{n} 条）——已归零的残留清单"
              f"禁止回升，请修代码而非加表项。")
        return 1
    print("检查通过：无层违规。")
    return 0


def selftest():
    """内置夹具：正/反违规 + ../ 前缀 + 前向声明 + 层标记 + strict 语义。"""
    import tempfile

    cases = [
        # (path, content, expected rules)
        ("ui/pages/x.cpp",
         '// 层：视图\n#include "../../io/lasparser.h"\n',
         {"ui-io-include"}),
        ("ui/pages/x.cpp",
         '// 层：视图\n#include "../../io/lasdoc.h"\n',
         set()),
        ("ui/pages/x.cpp",
         '// 层：视图\n#include "../../metadata/paleoprojectfile.h"\n',
         {"ui-metadata-include"}),
        ("ui/pages/x.cpp",
         '// 层：视图\n#include "../../metadata/layermanifest.h"\n',
         set()),
        ("ui/pages/x.cpp",
         '// 层：视图\n#include "../../algorithms/faciespolygonize.h"\n',
         {"ui-algorithms-include"}),
        ("ui/pages/x.cpp",
         '// 层：视图\n#include "../../catalog/datacatalog.h"\n',
         set()),
        ("workflow/x.cpp",
         '// 层：功能\n#include "../ui/pages/pagepanels.h"\n',
         {"reverse-ui-include"}),
        ("linkage/x.h",
         '// 层：功能\nclass QTabWidget;\n',
         {"qtwidgets-fwd-decl"}),
        ("linkage/x.h",
         '// 层：功能\nclass QgsMapLayer;\n',
         set()),
        ("io/x.cpp",
         '// 层：数据\n#include <QWidget>\n',
         {"qtwidgets-include"}),
        ("io/x.cpp",
         '// 层：数据\n#include <QtWidgets/QDialog>\n',
         {"qtwidgets-include"}),
        ("qgis/x.cpp",
         '// 层：QGIS 封装\n#include <QWidget>\n',
         set()),  # qgis 豁免 QtWidgets
        ("qgis/x.cpp",
         '// 层：QGIS 封装\n#include "../ui/x.h"\n',
         {"reverse-ui-include"}),  # qgis 不豁免 ui/
        ("app/x.cpp",
         '// 层：组装根\n#include "../ui/pages/pagepanels.h"\n#include <QWidget>\n',
         set()),  # app 豁免（by design）
        ("selfcheck/main.cpp",
         '// 层：测试壳\n#include "../ui/pages/pagepanels.h"\n',
         set()),  # selfcheck 豁免（by design）
        ("ui/pages/x.cpp",
         '#include "pageshared.h"\n',
         {"layer-marker"}),
        ("ui/pages/x.cpp",
         '// 层：视图\n#include "pageshared.h"\n#include <QTabWidget>\n',
         set()),  # ui 自身用 QtWidgets 合法
        ("services/x.cpp",
         '// 层：功能\n#include <QString>\n',
         {"layer-marker-mismatch"}),
    ]

    global SRC
    orig_src = SRC
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        SRC = tmp
        for rel, content, expected in cases:
            path = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            open(path, "w", encoding="utf-8").write(content)
            got = {rule for rule, _n, _t in check_file(path)}
            if got != expected:
                print(f"SELFTEST FAIL {rel}: 期望 {sorted(expected)} "
                      f"实得 {sorted(got)}")
                failures += 1
        # ---- 词表外置回归：新目录登记词表后按登记层检查，未登记会被
        # layer-marker 判违规（new_module.sh 的同步义务就是为此存在）----
        geo = os.path.join(tmp, "geophysics", "x.cpp")
        os.makedirs(os.path.dirname(geo), exist_ok=True)
        open(geo, "w", encoding="utf-8").write('// 层：数据\n#include "../ui/x.h"\n')
        unregistered = {rule for rule, _n, _t in check_file(geo)}
        LAYERS["geophysics"] = "数据"
        NON_VIEW.append("geophysics")
        QTWIDGETS_BAN.append("geophysics")
        registered = {rule for rule, _n, _t in check_file(geo)}
        del LAYERS["geophysics"]
        NON_VIEW.remove("geophysics")
        QTWIDGETS_BAN.remove("geophysics")
        if "layer-marker-mismatch" not in unregistered:
            print("SELFTEST FAIL 词表: 未登记新目录必须被 layer-marker 判违规")
            failures += 1
        if registered != {"reverse-ui-include"}:
            print(f"SELFTEST FAIL 词表: 登记后应按登记层检查，实得 {sorted(registered)}")
            failures += 1

    # ---- strict 语义夹具：需要干净扫描树，与上面的违规夹具分树跑 ----
    global BASELINE
    orig_baseline = BASELINE
    with tempfile.TemporaryDirectory() as tmp2:
        SRC = tmp2
        BASELINE = os.path.join(tmp2, "baseline.txt")

        def write_baseline(text):
            open(BASELINE, "w", encoding="utf-8").write(text)

        def rc_of(strict):
            return run_check(strict=strict, baseline_path=BASELINE)

        # (a) 全净 + baseline 空：非严格/严格都过
        write_baseline("# empty\n")
        if rc_of(False) != 0 or rc_of(True) != 0:
            print("SELFTEST FAIL strict: 全净+空 baseline 应双绿")
            failures += 1
        # (b) 注入违例 + baseline 空：双红
        os.makedirs(os.path.join(tmp2, "io"), exist_ok=True)
        vio = os.path.join(tmp2, "io", "bad.cpp")
        open(vio, "w", encoding="utf-8").write('// 层：数据\n#include <QWidget>\n')
        if rc_of(False) != 1 or rc_of(True) != 1:
            print("SELFTEST FAIL strict: 注入违例应双红")
            failures += 1
        # (c) 违例 + baseline 命中：非严格绿（残留合法），strict 红（baseline 回升）
        write_baseline("src/io/bad.cpp:qtwidgets-include\n")
        if rc_of(False) != 0:
            print("SELFTEST FAIL strict: 命中 baseline 的残留非严格模式应绿")
            failures += 1
        if rc_of(True) != 1:
            print("SELFTEST FAIL strict: baseline 新增条目在 strict 下必须红（防回升）")
            failures += 1
        # (d) 违例已修 + baseline 未删（可收缩）：非严格绿+NOTE，strict 红
        open(vio, "w", encoding="utf-8").write('// 层：数据\n#include <QString>\n')
        if rc_of(False) != 0:
            print("SELFTEST FAIL strict: 可收缩条目非严格模式应绿")
            failures += 1
        if rc_of(True) != 1:
            print("SELFTEST FAIL strict: 可收缩条目在 strict 下必须红（强制删行）")
            failures += 1
    BASELINE = orig_baseline
    SRC = orig_src
    if failures:
        print(f"selftest：{failures}/{len(cases) + 7} 夹具失败")
        return 1
    print(f"selftest 通过：{len(cases)} 文件夹具 + 2 词表外置夹具 + "
          f"5 strict 语义夹具全部命中预期。")
    return 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    strict = False
    vocab = VOCAB
    baseline = None
    it = iter(argv)
    for a in it:
        if a == "--selftest":
            return selftest()
        if a == "--strict":
            strict = True
        elif a == "--vocab":
            vocab = next(it, None)
            if not vocab:
                print("FAIL --vocab 需要路径参数", file=sys.stderr)
                return 2
        elif a == "--baseline":
            baseline = next(it, None)
            if not baseline:
                print("FAIL --baseline 需要路径参数", file=sys.stderr)
                return 2
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            # 真实 argv 校验：未知参数一律 exit 2（--strict 假绿的根因是全静默忽略）
            print(f"FAIL 未知参数：{a}\n"
                  f"       用法：check_layering.py [--strict] [--vocab PATH] "
                  f"[--baseline PATH] | --selftest", file=sys.stderr)
            return 2
    global SRC
    if vocab != VOCAB:
        # 自定义词表：替换模块级单点事实（selftest 也走这条路注入临时词表）
        _layers, _nv, _io, _meta = load_vocab(vocab)
        LAYERS.clear()
        LAYERS.update(_layers)
        NON_VIEW[:] = _nv
        UI_IO_WHITELIST.clear()
        UI_IO_WHITELIST.update(_io)
        UI_METADATA_WHITELIST.clear()
        UI_METADATA_WHITELIST.update(_meta)
        LAYER_VOCAB.clear()
        LAYER_VOCAB.update(set(_layers))
        QTWIDGETS_BAN.clear()
        QTWIDGETS_BAN.extend(d for d in NON_VIEW if d != "qgis")
    return run_check(strict=strict, baseline_path=baseline)


if __name__ == "__main__":
    sys.exit(main())
