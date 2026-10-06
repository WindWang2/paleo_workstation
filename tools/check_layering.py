#!/usr/bin/env python3
# 层边界机械检查（docs/UI_LAYER_PLAN.md W6）。
#
# 用法：
#   check_layering.py [--strict] [--vocab PATH] [--baseline PATH]
#   check_layering.py --selftest
#   check_layering.py --transitive [--qgis-include PATH]   # opt-in 诊断档（方向 49）
#   check_layering.py --symbol-audit                       # opt-in 诊断档（方向 49）
#   未知参数一律 exit 2（历史上 argv 被静默忽略，--strict 假绿——已修）。
#
# 规则面：
#   1. include 一律先规范化为仓库相对路径再匹配——`../io/x.h`、`../../io/x.h`
#      与 `io/x.h` 归一为 `io/x.h`，防 ../ 前缀绕过。
#   2. 正向（视图层出边）：src/ui/** 只允许 include
#        - io/lasdoc.h          （io/* 白名单制，其余 io/* 一律失败）
#        - metadata/{layermanifest,paleoprojectstore,mapversionstore,
#          releasestore,wellsectionstore,faultsetstore}.h（六头，单点事实在
#          tools/layering_vocab.json——此处只示意，勿再单独枚举维护）
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
#   6. 数据层禁令：数据层模块禁止 include qgis/*。
#   7. 数据层禁令：数据层模块禁止 include 功能层（workflow/*, linkage/*, ai/*）。
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
# `--selftest` 跑内置正/反夹具 + strict 语义夹具（不进 ctest 就是哑护栏）；
# 方向 49 起并含 --transitive / --symbol-audit 两档的正反夹具。
#
# opt-in 诊断档（方向 49，默认关；与三档闸门互斥执行，不进 strict 闸、
# 不改变三档任何输出）：
#   --transitive：传递闭包黄牌清单——补「QtWidgets 禁令只在直接 include 级
#      成立」的盲区。功能层文件直接 include 的 repo qgis/ 封装头，若该头
#      经 include 闭包（repo 边 + QGIS 自带头边）传递引入 QtWidgets 字面
#      （`<QtWidgets/…>`、`<QGestureEvent>` 等词表单类头），即黄牌并打印
#      完整链。视图层同类边仅「记录」（视图不禁 QtWidgets）。
#      QGIS 头目录定位：--qgis-include > $QGIS_PREFIX/include/qgis >
#      vendor/superbuild/prefix/include/qgis；未定位则仅按 repo 闭包判定
#      （黄牌面收窄，输出 NOTE 说明）。已知实现边界：norm_include 会把
#      `<qgsx.h>` 归一成不存在的 repo 幽灵路径——repo 边必须存在性校验后
#      再回退外部解析。
#      方向 59 起并含「公共头出闸重头」检测：io/* / algorithms/* 头（ui
#      白名单外的全部重头）出现在非实现 TU 的 repo 传递闭包即黄牌并打链
#      ——豁免三类：同模块 TU（io 头对 io TU / algorithms 头对 algorithms
#      TU = 实现 TU）、TU 自身的直接 include（自有选择，直接边的层违规由
#      三档闸门口径管）、app/selfcheck（组装根/测试壳终端消费 by design）。
#      方向 49 时点「刻意不查」的 previewdoc 类门面扇出（38 处）已由方向
#      59 收口 lasparser/qgisprocessingservice 两族；algorithms/* 经 workflow
#      头进 ui 的存量扇出仍是方向 49 递延债面，本检测如实列示（黄牌清单
#      是诊断面，不进 strict 闸——修复手段是删 include 边，不落 baseline）。
#      护栏语义：selftest 内置正/反夹具（ctest layering_selftest 档把守
#      检测逻辑本身）；--transitive 退出码维持诊断档语义（0）。
#   --symbol-audit：符号级抽检——非视图层 TU 内 new / std::make_unique /
#      std::make_shared 构造 src/ui 头定义的 Q_OBJECT 类即 fail（exit 1）。
#      词表由 src/ui/**/*.h 扫描生成；这是「不带 include 直接 new」盲区的
#      直接护栏（当前树应零报告）。

import json
import os
import re
import sys
from collections import deque

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
                elif (LAYERS.get(layer_dir) == "数据" or layer_dir in {"domain", "catalog", "io", "metadata", "services", "algorithms"}) and dst == "qgis":
                    violations.append(("data-qgis-include", n, line.strip()))
                elif (LAYERS.get(layer_dir) == "数据" or layer_dir in {"domain", "catalog", "io", "metadata", "services", "algorithms"}) and (LAYERS.get(dst) == "功能" or dst in {"workflow", "linkage", "ai"}):
                    violations.append(("data-functional-include", n, line.strip()))

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
        # 统一正斜杠：os.path.join 在 Windows 产出 src\io/x.cpp 混合分隔符，
        # 与 baseline（正斜杠）永不相等——残留匹配与 selftest 全失效。
        rel_src = "src/" + rel
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


# ---------------- opt-in 诊断档（方向 49）：--transitive / --symbol-audit ----------------
# 只在显式传参时执行；无参 / --strict / --selftest 三档的行为与输出零变化。

FUNCTIONAL_DIRS = ("workflow", "linkage", "ai")
QGIS_INCLUDE_FALLBACK = os.path.join(REPO, "vendor", "superbuild",
                                     "prefix", "include", "qgis")
TRANSITIVE_NODE_CAP = 4000

_INC_CACHE = {}


def file_includes(path):
    """[(行号, include 字面)]，带缓存；读不了的文件按无 include 处理。"""
    if path not in _INC_CACHE:
        try:
            with open(path, encoding="utf-8", errors="replace") as fh:
                _INC_CACHE[path] = [(n, m.group(1)) for n, line in enumerate(fh, 1)
                                    for m in [INC_RE.match(line)] if m]
        except OSError:
            _INC_CACHE[path] = []
    return _INC_CACHE[path]


def find_qgis_include_dir(explicit=None):
    """QGIS 自带头目录：--qgis-include > $QGIS_PREFIX/include/qgis > 仓内 vendor 默认。"""
    cands = []
    if explicit:
        cands.append(explicit)
    env = os.environ.get("QGIS_PREFIX")
    if env:
        cands.append(os.path.join(env, "include", "qgis"))
    cands.append(QGIS_INCLUDE_FALLBACK)
    for c in cands:
        if c and os.path.isdir(c):
            return c
    return None


def is_qtwidgets_literal(inc):
    return inc.startswith("QtWidgets") or inc in QTWIDGETS_CLASSES


def widget_chain(wrapper_rel, qgis_inc, cap=TRANSITIVE_NODE_CAP):
    """repo qgis 头起的 include BFS（repo 边 + 外部 qgs 边）。

    返回到 QtWidgets 字面的最短链 [wrapper, …, '<字面>@行']，无则 None。
    repo 边必须文件存在——norm_include 会把 `<qgsx.h>` 归一成不存在的
    repo 幽灵路径（如 qgis/qgsmaptool.h），须回退到外部 qgs 解析。
    """
    def node_path(node):
        return os.path.join(SRC, node[1]) if node[0] == "repo" \
            else os.path.join(qgis_inc, node[1])

    start = ("repo", wrapper_rel)
    seen = {start}
    prev = {}
    queue = deque([start])
    visited = 0
    while queue and visited < cap:
        node = queue.popleft()
        visited += 1
        for n, inc in file_includes(node_path(node)):
            if is_qtwidgets_literal(inc):
                chain = [f"<{inc}>@{n}"]
                cur = node
                while cur != start:
                    chain.append(cur[1] if cur[0] == "repo" else f"[qgs] {cur[1]}")
                    cur = prev[cur]
                return [wrapper_rel] + list(reversed(chain))
            nxt = None
            if node[0] == "repo":
                rel = norm_include(os.path.dirname(node_path(node)), inc)
                if rel is not None and os.path.exists(os.path.join(SRC, rel)):
                    nxt = ("repo", rel)
            if nxt is None and qgis_inc and inc.endswith(".h") \
                    and not inc.startswith(("Qt", "qgis")):
                base = os.path.basename(inc)
                if os.path.exists(os.path.join(qgis_inc, base)):
                    nxt = ("ext", base)
            if nxt and nxt not in seen:
                seen.add(nxt)
                prev[nxt] = node
                queue.append(nxt)
    return None


def iter_files_under(dirs):
    for d in dirs:
        root = os.path.join(SRC, d)
        for dirpath, _dirs, files in os.walk(root):
            for f in sorted(files):
                if f.endswith((".h", ".cpp")):
                    yield os.path.join(dirpath, f)


# ---------------- 方向 59：--transitive 升级「公共头出闸重头」检测 ----------------
# io/* / algorithms/* 头（ui io 白名单外）出现在非实现 TU 的 repo 传递闭包
# 即黄牌。豁免：同模块 TU（实现侧）、TU 自身直接 include、app/selfcheck
# （组装根/测试壳终端消费 by design）。白名单复用 ui_io_whitelist 单点事实
# （当前 io/lasdoc.h——纯类型门面，本身即「轻」）。

HEAVY_ROOTS = ("io/", "algorithms/")
HEAVY_EXEMPT_DIRS = {"app", "selfcheck"}


def repo_closure(start_rel, cap=TRANSITIVE_NODE_CAP):
    """start_rel 起的 repo 内部 include BFS 闭包（边须存在性校验）。

    返回 (seen 集, prev 链)。与 widget_chain 的 repo 边同口径：norm_include
    归一出的幽灵路径（不存在的文件）不算边。
    """
    seen = {start_rel}
    prev = {start_rel: None}
    queue = deque([start_rel])
    visited = 0
    while queue and visited < cap:
        cur = queue.popleft()
        visited += 1
        path = os.path.join(SRC, cur)
        for _n, inc in file_includes(path):
            rel = norm_include(os.path.dirname(path), inc)
            if rel and rel not in seen and os.path.exists(os.path.join(SRC, rel)):
                seen.add(rel)
                prev[rel] = cur
                queue.append(rel)
    return seen, prev


def collect_heavy_fanout():
    """(--transitive 的重头出闸数据面) → [(tu_rel, heavy_rel, chain_str)]。"""
    hits = []
    for root, _dirs, files in os.walk(SRC):
        for f in sorted(files):
            if not f.endswith(".cpp"):
                continue
            tu_rel = os.path.relpath(os.path.join(root, f), SRC).replace(os.sep, "/")
            tu_top = tu_rel.split("/", 1)[0]
            if tu_top in HEAVY_EXEMPT_DIRS:
                continue
            tu_path = os.path.join(SRC, tu_rel)
            direct = {norm_include(os.path.dirname(tu_path), inc)
                      for _n, inc in file_includes(tu_path)}
            direct.discard(None)
            seen, prev = repo_closure(tu_rel)
            for h in sorted(x for x in seen
                            if x.startswith(HEAVY_ROOTS)
                            and x not in UI_IO_WHITELIST):
                if h.split("/", 1)[0] == tu_top or h in direct:
                    continue
                chain = [h]
                cur = prev[h]
                while cur is not None:
                    chain.append(cur)
                    cur = prev[cur]
                chain.reverse()
                hits.append((tu_rel, h, " -> ".join(chain)))
    return hits


def collect_transitive(qgis_inc_arg=None):
    """(--transitive 的数据面) → (inc_dir, func_hits, ui_widget_notes)。"""
    inc_dir = find_qgis_include_dir(qgis_inc_arg)
    chain_cache = {}

    def chain_of(wrapper):
        if wrapper not in chain_cache:
            chain_cache[wrapper] = widget_chain(wrapper, inc_dir)
        return chain_cache[wrapper]

    func_hits, ui_widget_notes = [], []
    for path in iter_files_under(FUNCTIONAL_DIRS + ("ui",)):
        rel = os.path.relpath(path, SRC).replace(os.sep, "/")
        from_ui = rel.startswith("ui/")
        for n, inc in file_includes(path):
            norm = norm_include(os.path.dirname(path), inc)
            if norm is None or not norm.startswith("qgis/") \
                    or not os.path.exists(os.path.join(SRC, norm)):
                continue
            chain = chain_of(norm)
            if chain:
                entry = f"{rel}:{n} → {' -> '.join(chain)}"
                (ui_widget_notes if from_ui else func_hits).append(entry)
    return inc_dir, func_hits, ui_widget_notes


def run_transitive(qgis_inc_arg=None):
    inc_dir, func_hits, ui_widget_notes = collect_transitive(qgis_inc_arg)
    heavy_hits = collect_heavy_fanout()
    print("NOTE --transitive：opt-in 传递闭包黄牌清单（不进 strict 闸；与三档输出互不干扰）")
    if inc_dir:
        print(f"     QGIS 头目录：{inc_dir}")
    else:
        print("     QGIS 头目录未定位（用 --qgis-include 或环境变量 QGIS_PREFIX 指定后"
              "可解析外部 qgs 边）；本次仅按 repo 闭包判定，黄牌面收窄。")
    for e in func_hits:
        print(f"黄牌 {e}")
    for e in ui_widget_notes:
        print(f"记录 {e}（视图层不禁 QtWidgets，仅记录）")
    # 重头出闸按（重头 × 出闸头）聚合展示：出闸头 = 链上直接 include 重头的
    # 那个 repo 头（债的归属点）；逐 TU 明细以计数给出，代表链示形态。
    edges = {}
    for tu, heavy, chain in heavy_hits:
        steps = chain.split(" -> ")
        # 直接 include 的重头已在 collect_heavy_fanout 豁免，链长恒 ≥3；
        # steps[-2] 即链上直接 include 重头的 repo 头（出闸责任人）。
        key = (heavy, steps[-2])
        entry = edges.setdefault(key, {"tus": set(), "chain": steps})
        entry["tus"].add(tu)
    for (heavy, carrier) in sorted(edges):
        e = edges[(heavy, carrier)]
        print(f"黄牌(重头出闸) {heavy} 经 {carrier} 出闸 → {len(e['tus'])} TU；"
              f"代表链：{' -> '.join(e['chain'])}")
    n_edges = len(edges)
    n_tus = len({tu for tu, _h, _c in heavy_hits})
    n_heavy = len({h for _t, h, _c in heavy_hits})
    print(f"--transitive 小计：功能层 QtWidgets 黄牌 {len(func_hits)} 处；"
          f"视图层 QtWidgets 边记录 {len(ui_widget_notes)} 处；"
          f"重头出闸黄牌 {n_edges} 条出闸边（{n_heavy} 个重头 × {n_tus} 个受染 TU）。")
    return 0


UI_CLASS_DECL_RE = re.compile(r"\bclass\s+([A-Za-z_]\w*)")
_CODE_ONLY_RE = re.compile(
    r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/', re.S)


def strip_comments_and_strings(text):
    """注释与字符串字面整体替换为空白（保留换行，行号不漂移）。"""
    return _CODE_ONLY_RE.sub(
        lambda m: re.sub(r"[^\n]", " ", m.group()), text)


def ui_qobject_vocab():
    """src/ui/**/*.h 中带 Q_OBJECT 的类名集合（符号级抽检的目标词表）。"""
    vocab = set()
    for path in iter_files_under(("ui",)):
        if not path.endswith(".h"):
            continue
        try:
            with open(path, encoding="utf-8", errors="replace") as fh:
                lines = fh.read().split("\n")
        except OSError:
            continue
        last = None
        for line in lines:
            m = UI_CLASS_DECL_RE.search(line)
            if m:
                last = m.group(1)
            if "Q_OBJECT" in line and last:
                vocab.add(last)
    return vocab


def collect_symbol_audit():
    """(--symbol-audit 的数据面) → (vocab, hits)；hits = [(rel, 行号, 文本, 构造写法)]。"""
    vocab = ui_qobject_vocab()
    hits = []
    if not vocab:
        return vocab, hits
    name_alt = "|".join(sorted(vocab))
    new_re = re.compile(r"\bnew\s+(" + name_alt + r")\s*[({;]")
    make_re = re.compile(r"\bstd::(?:make_unique|make_shared)<\s*(" + name_alt + r")\s*>")
    for path in iter_files_under(NON_VIEW):
        try:
            with open(path, encoding="utf-8", errors="replace") as fh:
                raw = fh.read()
        except OSError:
            continue
        rel = os.path.relpath(path, SRC).replace(os.sep, "/")
        for n, line in enumerate(strip_comments_and_strings(raw).split("\n"), 1):
            for m in new_re.finditer(line):
                hits.append((rel, n, line.strip(), f"new {m.group(1)}"))
            for m in make_re.finditer(line):
                hits.append((rel, n, line.strip(), f"make_*<{m.group(1)}>"))
    return vocab, hits


def run_symbol_audit():
    vocab, hits = collect_symbol_audit()
    print(f"NOTE --symbol-audit：opt-in 符号级抽检（ui Q_OBJECT 词表 {len(vocab)} 类）")
    if not vocab:
        print("FAIL --symbol-audit：ui 词表为空——扫描面异常，拒绝静默通过")
        return 2
    for rel, n, text, what in hits:
        print(f"FAIL {rel}:{n} [symbol-ui-construct] {what}（非视图层构造 ui 类）：{text}")
    if hits:
        print(f"\n{len(hits)} 处非视图层符号级 ui 构造——include 级护栏挡不住的"
              f"「直接 new」回流，须改意图信号/壳持有。")
        return 1
    print("符号级抽检通过：非视图层零 ui 类构造。")
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
        ("io/x.cpp",
         '// 层：数据\n#include "../qgis/qgislayerservice.h"\n',
         {"data-qgis-include"}),
        ("io/x.cpp",
         '// 层：数据\n#include "../workflow/sectionworkbench.h"\n',
         {"data-functional-include"}),
        ("domain/x.cpp",
         '// 层：数据\n#include "../linkage/linkagewidget.h"\n',
         {"data-functional-include"}),
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

        # ---- 方向 49：--transitive 夹具（黄牌正例/clean 反例/幽灵边/视图双面）----
        def put(rel, text):
            p = os.path.join(tmp2, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, "w", encoding="utf-8").write(text)

        qinc = os.path.join(tmp2, "qgis_inc")
        put("qgis_inc/qgsfake_tool.h", "#include <QGestureEvent>\n")
        put("qgis_inc/qgsok.h", "#include <QString>\n")
        put("qgis_inc/qgsdeep_chain.h", '#include "qgssecond.h"\n')
        put("qgis_inc/qgssecond.h", "#include <QWidget>\n")
        put("qgis/wtool.h", '// 层：QGIS 封装\n#include <qgsfake_tool.h>\n')
        put("qgis/mid.h", '// 层：QGIS 封装\n#include <qgsdeep_chain.h>\n')
        put("qgis/deep.h", '// 层：QGIS 封装\n#include "mid.h"\n')
        put("qgis/direct.h", '// 层：QGIS 封装\n#include <QWidget>\n')
        put("qgis/clean.h", '// 层：QGIS 封装\n#include <qgsok.h>\n')
        put("qgis/ghost.h", '// 层：QGIS 封装\n#include <qgsnotthere.h>\n')
        put("linkage/x.cpp", '// 层：功能\n#include "../qgis/wtool.h"\n')
        put("linkage/deep.cpp", '// 层：功能\n#include "../qgis/deep.h"\n')
        put("linkage/ghost.cpp", '// 层：功能\n#include "../qgis/ghost.h"\n')
        put("linkage/ok.cpp", '// 层：功能\n#include "../qgis/clean.h"\n')
        put("workflow/d.cpp", '// 层：功能\n#include "qgis/direct.h"\n')
        put("ui/w.cpp", '// 层：视图\n#include "../qgis/wtool.h"\n')

        ch = widget_chain("qgis/wtool.h", qinc)
        if not (ch and ch[0] == "qgis/wtool.h" and ch[1] == "[qgs] qgsfake_tool.h"
                and ch[-1].startswith("<QGestureEvent>")):
            print("SELFTEST FAIL transitive: qgs 外部边应链到 QtWidgets 字面")
            failures += 1
        ch = widget_chain("qgis/deep.h", qinc)
        if not (ch and len(ch) == 5 and ch[1] == "qgis/mid.h"
                and ch[3] == "[qgs] qgssecond.h" and ch[-1].startswith("<QWidget>")):
            print("SELFTEST FAIL transitive: repo→repo→qgs→qgs→QtWidgets 多跳链")
            failures += 1
        if widget_chain("qgis/clean.h", qinc) is not None:
            print("SELFTEST FAIL transitive: 纯 QtCore 外部链不应判 widget-bearing")
            failures += 1
        if widget_chain("qgis/ghost.h", qinc) is not None:
            print("SELFTEST FAIL transitive: 幽灵 repo 路径（norm_include 归一出的"
                  "不存在文件）不得崩溃或误报")
            failures += 1
        if widget_chain("qgis/wtool.h", None) is not None:
            print("SELFTEST FAIL transitive: 无 QGIS 头目录时仅 repo 闭包（黄牌面收窄）")
            failures += 1
        _inc, func_hits, ui_widget_notes = collect_transitive(qinc)
        func_set = {e.split(" → ")[0] for e in func_hits}
        if func_set != {"linkage/x.cpp:2", "linkage/deep.cpp:2", "workflow/d.cpp:2"}:
            print(f"SELFTEST FAIL transitive: 功能层黄牌面 {sorted(func_set)} 与预期不符")
            failures += 1
        if len(ui_widget_notes) != 1 or not ui_widget_notes[0].startswith("ui/w.cpp:2"):
            print(f"SELFTEST FAIL transitive: 视图层 QtWidgets 记录面 {ui_widget_notes}")
            failures += 1

        # ---- 方向 59：--transitive 重头出闸夹具（正例 + 四类豁免反例）----
        put("io/heavy.h", "// 层：数据\n")
        put("io/lasdoc.h", "// 层：数据\n")  # 名字即白名单单点事实（ui_io_whitelist）
        put("services/facade.h", '// 层：数据\n#include "../io/heavy.h"\n')
        put("services/facade_light.h", '// 层：数据\n#include "../io/lasdoc.h"\n')
        put("ui/victim.cpp", '// 层：视图\n#include "../services/facade.h"\n')
        put("ui/own_choice.cpp", '// 层：视图\n#include "../io/heavy.h"\n')
        put("io/impl.cpp", '// 层：数据\n#include "../services/facade.h"\n')
        put("ui/light.cpp", '// 层：视图\n#include "../services/facade_light.h"\n')
        put("app/wired.cpp", '// 层：组装根\n#include "../services/facade.h"\n')
        put("selfcheck/probe.cpp", '// 层：测试壳\n#include "../services/facade.h"\n')

        heavy_map = {(tu, h): chain for tu, h, chain in collect_heavy_fanout()}
        if ("ui/victim.cpp", "io/heavy.h") not in heavy_map:
            print("SELFTEST FAIL heavy-fanout: 门面扇出的 ui TU 必须黄牌（io/heavy.h）")
            failures += 1
        else:
            ch = heavy_map[("ui/victim.cpp", "io/heavy.h")]
            if not (ch.startswith("ui/victim.cpp") and "services/facade.h" in ch
                    and ch.endswith("io/heavy.h")):
                print(f"SELFTEST FAIL heavy-fanout: 链形态异常：{ch}")
                failures += 1
        victim_set = {tu for tu, _h in heavy_map}
        if victim_set != {"ui/victim.cpp"}:
            print(f"SELFTEST FAIL heavy-fanout: 命中面 {sorted(victim_set)} 应仅"
                  f" ui/victim.cpp（直接 include/同模块/白名单/app+selfcheck 四类豁免不触发）")
            failures += 1

        # ---- 方向 49：--symbol-audit 夹具（new / make_unique 正例 + 注释反例）----
        put("ui/foopanel.h", '// 层：视图\nclass FooPanel {\n  Q_OBJECT\n};\n')
        put("workflow/n.cpp", '// 层：功能\nauto *p = new FooPanel(this);\n')
        put("workflow/m.cpp", '// 层：功能\nauto q = std::make_unique<FooPanel>();\n')
        put("io/c.cpp", '// 层：数据\n// auto *x = new FooPanel(this);\nnew OtherThing();\n')
        put("qgis/k.cpp", '// 层：QGIS 封装\nFooPanel on_stack;\n')
        vocab, sym_hits = collect_symbol_audit()
        if "FooPanel" not in vocab:
            print("SELFTEST FAIL symbol-audit: ui Q_OBJECT 词表未收 FooPanel")
            failures += 1
        if {h[0] for h in sym_hits} != {"workflow/n.cpp", "workflow/m.cpp"}:
            print(f"SELFTEST FAIL symbol-audit: 命中面 {[h[0] for h in sym_hits]}"
                  f" 应为 n/m 两处（注释/非词表/栈构造不计）")
            failures += 1
    BASELINE = orig_baseline
    SRC = orig_src
    if failures:
        print(f"selftest：{failures} 处夹具失败")
        return 1
    print(f"selftest 通过：{len(cases)} 文件夹具 + 2 词表外置夹具 + "
          f"5 strict 语义夹具 + 7 transitive 夹具 + 3 重头出闸夹具 + "
          f"2 symbol-audit 夹具全部命中预期。")
    return 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    strict = False
    vocab = VOCAB
    baseline = None
    transitive = False
    symbol_audit = False
    qgis_include = None
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
        elif a == "--transitive":
            transitive = True
        elif a == "--symbol-audit":
            symbol_audit = True
        elif a == "--qgis-include":
            qgis_include = next(it, None)
            if not qgis_include:
                print("FAIL --qgis-include 需要路径参数", file=sys.stderr)
                return 2
            if not os.path.isdir(qgis_include):
                # 显式指定即显式意图：路径不存在直接红，不静默回退 vendor 默认
                print(f"FAIL --qgis-include 目录不存在：{qgis_include}", file=sys.stderr)
                return 2
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            # 真实 argv 校验：未知参数一律 exit 2（--strict 假绿的根因是全静默忽略）
            print(f"FAIL 未知参数：{a}\n"
                  f"       用法：check_layering.py [--strict] [--vocab PATH] "
                  f"[--baseline PATH] | --selftest | --transitive "
                  f"[--qgis-include PATH] | --symbol-audit", file=sys.stderr)
            return 2
    if (transitive or symbol_audit) and (strict or baseline is not None):
        print("FAIL 诊断档（--transitive/--symbol-audit）与三档闸门参数"
              "（--strict/--baseline）互斥：诊断档不进闸，请分开跑。",
              file=sys.stderr)
        return 2
    if qgis_include and not transitive:
        print("FAIL --qgis-include 只对 --transitive 有意义", file=sys.stderr)
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
    if transitive or symbol_audit:
        # 诊断档：替代默认扫描执行（三档闸门口径不受影响，另跑即可）
        rc = 0
        if transitive:
            rc = max(rc, run_transitive(qgis_include))
        if symbol_audit:
            rc = max(rc, run_symbol_audit())
        return rc
    return run_check(strict=strict, baseline_path=baseline)


if __name__ == "__main__":
    sys.exit(main())
