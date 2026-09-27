#!/usr/bin/env python3
# 层边界机械检查（docs/UI_LAYER_PLAN.md W6）。
#
# 规则面：
#   1. include 一律先规范化为仓库相对路径再匹配——`../io/x.h`、`../../io/x.h`
#      与 `io/x.h` 归一为 `io/x.h`，防 ../ 前缀绕过。
#   2. 正向（视图层出边）：src/ui/** 只允许 include
#        - io/lasdoc.h          （io/* 白名单制，其余 io/* 一律失败）
#        - metadata/{layermanifest,paleoprojectstore,mapversionstore,releasestore}.h
#      且 algorithms/* 一律失败。
#   3. 反向：src/{domain,catalog,io,metadata,services,workflow,linkage,
#      algorithms,ai,qgis}/** 出现 ui/* include 即失败。
#      src/app、src/selfcheck 不扫（组装根/测试壳按契约允许 ui/ 依赖，
#      by design 豁免）。
#   4. QtWidgets 禁令（domain/catalog/io/metadata/services/workflow/linkage/
#      algorithms/ai；仅 qgis 豁免）：词表制三类写法——
#      `#include <QtWidgets…>`、`#include <QWidget>` 等单类头、
#      `class Q…;` 前向声明。
#   5. 层标记：每个 src/ 文件头三行内必须有 `// 层：<六值词表之一>`。
#
# 合法残留走 tools/layering-baseline.txt（格式：每行 `<path>:<rule>`，
# `#` 开头注释）。命中 baseline 的违规降级为提示；baseline 里已修复的条目
# 提示可收缩。收敛方式：修代码后把该行从 baseline 删掉——baseline 只缩不涨。
#
# `--selftest` 跑内置正/反夹具（不进 ctest 就是哑护栏），单独挂 ctest 项。

import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "src")
BASELINE = os.path.join(REPO, "tools", "layering-baseline.txt")

LAYERS = {
    "domain": "数据", "catalog": "数据", "io": "数据", "metadata": "数据",
    "services": "数据", "algorithms": "数据",
    "workflow": "功能", "linkage": "功能", "ai": "功能",
    "qgis": "QGIS 封装", "ui": "视图", "app": "组装根", "selfcheck": "测试壳",
}
LAYER_VOCAB = set(LAYERS.values())

# 反向扫描与 QtWidgets 禁令覆盖的非视图目录；app/selfcheck 按契约豁免
# （组装根装配 UI、测试壳驱动 UI——见 docs/UI_LAYER_PLAN.md W6.1 by design）。
NON_VIEW = ["domain", "catalog", "io", "metadata", "services", "workflow",
            "linkage", "algorithms", "ai", "qgis"]
QTWIDGETS_BAN = [d for d in NON_VIEW if d != "qgis"]

UI_IO_WHITELIST = {"io/lasdoc.h"}
UI_METADATA_WHITELIST = {
    "metadata/layermanifest.h", "metadata/paleoprojectstore.h",
    "metadata/mapversionstore.h", "metadata/releasestore.h",
}

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
    if inc.startswith(("io/", "domain/", "catalog/", "metadata/", "services/",
                       "algorithms/", "workflow/", "linkage/", "ai/", "qgis/",
                       "ui/", "app/", "selfcheck/")):
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

    # W5c 层标记：头三行必须有 `// 层：<词表值>`
    head = "\n".join(lines[:3])
    m = re.search(r"//\s*层：\s*([^\n]+)", head)
    if m:
        m = m if m.group(1).strip() in LAYER_VOCAB else None
    if not m or m.group(1) not in LAYER_VOCAB:
        violations.append(("layer-marker", 0,
                           "头三行缺 `// 层：<数据|功能|QGIS 封装|视图|组装根|测试壳>`"))

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


def load_baseline():
    entries = {}
    if not os.path.exists(BASELINE):
        return entries
    with open(BASELINE, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            path, _, rule = line.partition(":")
            entries.setdefault(path.strip(), set()).add(rule.strip())
    return entries


def run_check():
    baseline = load_baseline()
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
        print(f"\n检查通过；baseline 有 {len(shrunk)} 条可收缩（非失败）。")
        return 0
    print("检查通过：无层违规。")
    return 0


def selftest():
    """内置夹具：正/反违规 + ../ 前缀 + 前向声明 + 层标记。"""
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
    SRC = orig_src
    if failures:
        print(f"selftest：{failures}/{len(cases)} 夹具失败")
        return 1
    print(f"selftest 通过：{len(cases)} 夹具全部命中预期。")
    return 0


def main():
    if "--selftest" in sys.argv[1:]:
        return selftest()
    return run_check()


if __name__ == "__main__":
    sys.exit(main())
