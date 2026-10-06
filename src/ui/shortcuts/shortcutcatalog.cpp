// 层：视图
#include "shortcutcatalog.h"

#include "../pages/pageshared.h"

#include <QAction>
#include <QCoreApplication>
#include <QHash>
#include <QLoggingCategory>
#include <QShortcut>
#include <QWidget>

Q_LOGGING_CATEGORY(lcPaleoShortcuts, "paleo.shortcuts")

namespace paleo::shortcuts
{

namespace
{
constexpr char kTrContext[] = "PaleoShortcuts";

// ---- 分组 taxonomy（定案记 .goal-loop-ledger-shortcuts-help.md）----
// 按「用户在哪个面上」分组，顺序 = 总表显示顺序。
struct GroupRow
{
  const char *id;
  const char *title;
};
const GroupRow kGroups[] = {
    {"navigation", QT_TRANSLATE_NOOP("PaleoShortcuts", "全局导航")},
    {"project", QT_TRANSLATE_NOOP("PaleoShortcuts", "工程与文件")},
    {"help", QT_TRANSLATE_NOOP("PaleoShortcuts", "帮助")},
    {"data", QT_TRANSLATE_NOOP("PaleoShortcuts", "数据管理")},
    {"wellcomposite", QT_TRANSLATE_NOOP("PaleoShortcuts", "综合柱状图")},
    {"map", QT_TRANSLATE_NOOP("PaleoShortcuts", "地图与图层")},
    {"sections", QT_TRANSLATE_NOOP("PaleoShortcuts", "剖面与三维")},
    {"layout", QT_TRANSLATE_NOOP("PaleoShortcuts", "图件设计器")},
    {"dialogs", QT_TRANSLATE_NOOP("PaleoShortcuts", "停靠与对话框")},
};

// 主窗六页的页名（页序/键序由 pagesinternal::kPageIds 决定，与
// PaleoMainWindow::buildRibbon 的旧 "Ctrl+%1" 循环逐位一致）。
const char *pageDescription(const QString &pageId)
{
  static const QHash<QString, const char *> descriptions = {
      {QStringLiteral("data"), QT_TRANSLATE_NOOP("PaleoShortcuts", "切换到「数据管理」页")},
      {QStringLiteral("correlation"), QT_TRANSLATE_NOOP("PaleoShortcuts", "切换到「地层对比」页")},
      {QStringLiteral("predict"), QT_TRANSLATE_NOOP("PaleoShortcuts", "切换到「预测编图」页")},
      {QStringLiteral("constraint"), QT_TRANSLATE_NOOP("PaleoShortcuts", "切换到「单因素图」页")},
      {QStringLiteral("compose"), QT_TRANSLATE_NOOP("PaleoShortcuts", "切换到「智能编图」页")},
      {QStringLiteral("validate"), QT_TRANSLATE_NOOP("PaleoShortcuts", "切换到「验证」页")},
  };
  return descriptions.value(pageId, QT_TRANSLATE_NOOP("PaleoShortcuts", "切换工作流页"));
}

ShortcutEntry make(const char *id, const QKeySequence &key, const char *context, Binding binding,
                   const char *group, const char *description, const char *sourcePanel,
                   const char *origin, Qt::ShortcutContext qtContext = Qt::WindowShortcut)
{
  ShortcutEntry e;
  e.id = QString::fromLatin1(id);
  e.key = key;
  e.context = QString::fromLatin1(context);
  e.qtContext = qtContext;
  e.binding = binding;
  e.group = QString::fromLatin1(group);
  e.description = QString::fromUtf8(description);
  e.sourcePanel = QString::fromUtf8(sourcePanel);
  e.origin = QString::fromLatin1(origin);
  return e;
}

constexpr Binding S = Binding::Shortcut;
constexpr Binding K = Binding::KeyHandler;

struct BuiltRegistry
{
  ShortcutRegistry registry;
  QStringList errors;
};

const BuiltRegistry &built()
{
  static const BuiltRegistry instance = [] {
    BuiltRegistry b;
    for (const ShortcutEntry &e : catalogEntries())
    {
      QString error;
      if (!b.registry.registerEntry(e, &error))
        b.errors.append(error);
    }
    return b;
  }();
  return instance;
}
} // namespace

// clang-format off
QList<ShortcutEntry> catalogEntries()
{
  QList<ShortcutEntry> out;
  const char *mainPanel = QT_TRANSLATE_NOOP("PaleoShortcuts", "主窗口");

  // ===== 全局导航（paleomainwindow.cpp buildRibbon / attach_shell）=====
  // W5：Ctrl+1..6 直切六页——id 尾段 = 页 id，键序 = 页序 + 1。
  for (int i = 0; i < pagesinternal::kPageIds.size(); ++i)
  {
    const QString pageId = pagesinternal::kPageIds.at(i);
    ShortcutEntry e = make("", QKeySequence(QStringLiteral("Ctrl+%1").arg(i + 1)), "main", S,
                           "navigation", pageDescription(pageId), mainPanel,
                           "src/ui/paleomainwindow.cpp");
    e.id = QStringLiteral("main.page.") + pageId;
    out.append(e);
  }
  out << make("main.page.next", QKeySequence(QStringLiteral("Ctrl+Tab")), "main", S, "navigation",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "循环切换到下一个工作流页"), mainPanel,
              "src/ui/paleomainwindow.cpp")
      << make("main.page.prev", QKeySequence(QStringLiteral("Ctrl+Shift+Tab")), "main", S, "navigation",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "循环切换到上一个工作流页"), mainPanel,
              "src/ui/paleomainwindow.cpp")
      << make("main.locator.focus", QKeySequence(QStringLiteral("Ctrl+K")), "main", S, "navigation",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "聚焦顶栏搜索框，查找井位或层位"), mainPanel,
              "src/ui/paleomainwindow_attach_shell.cpp")
      << make("main.crossplot.open", QKeySequence(QStringLiteral("Ctrl+Alt+X")), "main", S, "navigation",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "打开底部「交会相分类」面板"), mainPanel,
              "src/app/crossplotcontroller.cpp");

  // ===== 工程与文件 =====
  out << make("main.project.save", QKeySequence(QKeySequence::Save), "main", S, "project",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "保存工程（「地层对比」页让给网页工作台自己的保存）"),
              mainPanel, "src/ui/paleomainwindow_attach_shell.cpp");

  // ===== 帮助（方向 63 新增）=====
  out << make("main.help.shortcuts", QKeySequence(QKeySequence::HelpContents), "main", S, "help",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "打开快捷键总表"), mainPanel,
              "src/ui/help/helpsurface.cpp")
      << make("main.help.whatsThis", QKeySequence(QKeySequence::WhatsThis), "main", S, "help",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "进入「这是什么？」模式，再点击控件查看说明"), mainPanel,
              "src/ui/help/helpsurface.cpp")
      << make("sheet.copy", QKeySequence(QKeySequence::Copy), "dialog/shortcut-sheet", S, "help",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "复制总表中所选的快捷键条目"),
              QT_TRANSLATE_NOOP("PaleoShortcuts", "快捷键总表"),
              "src/ui/shortcuts/shortcutsheetdialog.cpp");

  // ===== 数据管理 =====
  const char *dataPage = QT_TRANSLATE_NOOP("PaleoShortcuts", "数据管理页");
  const char *dataList = QT_TRANSLATE_NOOP("PaleoShortcuts", "数据列表");
  const char *assetViews = QT_TRANSLATE_NOOP("PaleoShortcuts", "资产列表（焦点内）");
  const char *palette = QT_TRANSLATE_NOOP("PaleoShortcuts", "数据页命令面板");
  out << make("data.palette.open", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P), "main/data", S, "data",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "打开数据页命令面板，搜索资产、实体、动作与过滤器"),
              dataPage, "src/ui/pages/datapage.cpp")
      << make("data.list.undo", QKeySequence(Qt::CTRL | Qt::Key_Z), "main/data", S, "data",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "撤销上一步数据列表操作"), dataList,
              "src/ui/pages/datalist.cpp")
      << make("data.list.redo", QKeySequence(Qt::CTRL | Qt::Key_Y), "main/data", S, "data",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "重做数据列表操作"), dataList,
              "src/ui/pages/datalist.cpp")
      << make("data.list.invert", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A), "main/data", S, "data",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "反选资产"), dataList, "src/ui/pages/datalist.cpp")
      << make("data.list.selectFiltered", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F), "main/data", S,
              "data", QT_TRANSLATE_NOOP("PaleoShortcuts", "选中当前过滤结果中的全部资产"), dataList,
              "src/ui/pages/datalist.cpp")
      << make("data.list.rename", QKeySequence(Qt::Key_F2), "main/data", S, "data",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "重命名树中所选的实体"), dataList,
              "src/ui/pages/datalist.cpp")
      << make("data.list.shortcuts", QKeySequence(Qt::Key_Question), "main/data", S, "data",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "打开数据页快捷键表"), dataList,
              "src/ui/pages/datalist.cpp")
      << make("data.list.focusSearch", QKeySequence(Qt::CTRL | Qt::Key_F), "main/data", S, "data",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "聚焦资产搜索框"), dataList, "src/ui/pages/datalist.cpp")
      << make("data.list.vimToggle", QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_V), "main/data", S, "data",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "开关 Vim 风导航（j/k/g/G//）"), dataList,
              "src/ui/pages/datalist.cpp")
      << make("data.assets.toggleSelect", QKeySequence(Qt::Key_Space), "main/data/assets", K, "data",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "切换当前行的选中状态"), assetViews,
              "src/ui/pages/datalist.cpp")
      << make("data.assets.remove", QKeySequence(Qt::Key_Delete), "main/data/assets", K, "data",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "把所选资产移入可回收清单（确认后执行，可撤销）"),
              assetViews, "src/ui/pages/datalist.cpp")
      << make("data.assets.removeBackspace", QKeySequence(Qt::Key_Backspace), "main/data/assets", K,
              "data", QT_TRANSLATE_NOOP("PaleoShortcuts", "同 Delete：把所选资产移入可回收清单"),
              assetViews, "src/ui/pages/datalist.cpp")
      << make("data.vim.down", QKeySequence(Qt::Key_J), "main/data/assets", K, "data",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "Vim 风导航开启时：下移一行"), assetViews,
              "src/ui/pages/datanavtree.h")
      << make("data.vim.up", QKeySequence(Qt::Key_K), "main/data/assets", K, "data",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "Vim 风导航开启时：上移一行"), assetViews,
              "src/ui/pages/datanavtree.h")
      << make("data.vim.top", QKeySequence(Qt::Key_G), "main/data/assets", K, "data",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "Vim 风导航开启时：跳到第一行"), assetViews,
              "src/ui/pages/datanavtree.h")
      << make("data.vim.bottom", QKeySequence(Qt::SHIFT | Qt::Key_G), "main/data/assets", K, "data",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "Vim 风导航开启时：跳到最后一行"), assetViews,
              "src/ui/pages/datanavtree.h")
      << make("data.vim.search", QKeySequence(Qt::Key_Slash), "main/data/assets", K, "data",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "Vim 风导航开启时：聚焦搜索框"), assetViews,
              "src/ui/pages/datanavtree.h")
      << make("data.palette.up", QKeySequence(Qt::Key_Up), "dialog/data-palette", S, "data",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "在命令面板结果中上移选择"), palette,
              "src/ui/pages/dataopspalette.h")
      << make("data.palette.down", QKeySequence(Qt::Key_Down), "dialog/data-palette", S, "data",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "在命令面板结果中下移选择"), palette,
              "src/ui/pages/dataopspalette.h");
  const char *graph = QT_TRANSLATE_NOOP("PaleoShortcuts", "派生关系图（焦点内）");
  out << make("data.graph.activate", QKeySequence(Qt::Key_Return), "main/data/derivation-graph", K,
              "data", QT_TRANSLATE_NOOP("PaleoShortcuts", "选中焦点所在的版本节点（同单击）"), graph,
              "src/ui/pages/derivationgraph.cpp")
      << make("data.graph.activateSpace", QKeySequence(Qt::Key_Space), "main/data/derivation-graph", K,
              "data", QT_TRANSLATE_NOOP("PaleoShortcuts", "同回车：选中焦点所在的版本节点"), graph,
              "src/ui/pages/derivationgraph.cpp");

  // ===== 综合柱状图 =====
  const char *composite = QT_TRANSLATE_NOOP("PaleoShortcuts", "综合柱状图");
  const char *compositeCanvas = QT_TRANSLATE_NOOP("PaleoShortcuts", "柱状图画布（焦点内）");
  out << make("wellcomposite.gotoDepth", QKeySequence(QStringLiteral("Ctrl+G")), "main/data/wellcomposite",
              S, "wellcomposite", QT_TRANSLATE_NOOP("PaleoShortcuts", "跳转到指定深度"), composite,
              "src/ui/wellcomposite/wellcompositepanel.cpp")
      << make("wellcomposite.canvas.cancel", QKeySequence(Qt::Key_Escape), "main/data/wellcomposite/canvas",
              K, "wellcomposite",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "取消进行中的道头拖动、框选或标志层拖动"),
              compositeCanvas, "src/ui/wellcomposite/wellcompositecanvas.cpp")
      << make("wellcomposite.canvas.scrollUp", QKeySequence(Qt::Key_Up), "main/data/wellcomposite/canvas",
              K, "wellcomposite", QT_TRANSLATE_NOOP("PaleoShortcuts", "向浅处滚动可见井段的十分之一"),
              compositeCanvas, "src/ui/wellcomposite/wellcompositecanvas.cpp")
      << make("wellcomposite.canvas.scrollDown", QKeySequence(Qt::Key_Down), "main/data/wellcomposite/canvas",
              K, "wellcomposite", QT_TRANSLATE_NOOP("PaleoShortcuts", "向深处滚动可见井段的十分之一"),
              compositeCanvas, "src/ui/wellcomposite/wellcompositecanvas.cpp")
      << make("wellcomposite.canvas.zoomIn", QKeySequence(Qt::Key_Plus), "main/data/wellcomposite/canvas",
              K, "wellcomposite", QT_TRANSLATE_NOOP("PaleoShortcuts", "放大深度比例（= 键同效）"),
              compositeCanvas, "src/ui/wellcomposite/wellcompositecanvas.cpp")
      << make("wellcomposite.canvas.zoomOut", QKeySequence(Qt::Key_Minus), "main/data/wellcomposite/canvas",
              K, "wellcomposite", QT_TRANSLATE_NOOP("PaleoShortcuts", "缩小深度比例"), compositeCanvas,
              "src/ui/wellcomposite/wellcompositecanvas.cpp")
      << make("wellcomposite.canvas.configTrack", QKeySequence(Qt::Key_Return),
              "main/data/wellcomposite/canvas", K, "wellcomposite",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "打开键盘焦点所在道的配置"), compositeCanvas,
              "src/ui/wellcomposite/wellcompositecanvas.cpp")
      << make("wellcomposite.canvas.nextTrack", QKeySequence(Qt::Key_Tab), "main/data/wellcomposite/canvas",
              K, "wellcomposite", QT_TRANSLATE_NOOP("PaleoShortcuts", "把键盘焦点移到下一道"),
              compositeCanvas, "src/ui/wellcomposite/wellcompositecanvas.cpp");

  // ===== 地图与图层 =====
  const char *drawTool = QT_TRANSLATE_NOOP("PaleoShortcuts", "地图绘制工具（焦点内）");
  const char *editTool = QT_TRANSLATE_NOOP("PaleoShortcuts", "地图编辑工具（焦点内）");
  const char *vertexTool = QT_TRANSLATE_NOOP("PaleoShortcuts", "节点编辑工具（焦点内）");
  out << make("layers.remove", QKeySequence(Qt::Key_Delete), "main", S, "map",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "删除图层树中所选的图层或组（正在编辑的图层会被拒绝）"),
              QT_TRANSLATE_NOOP("PaleoShortcuts", "图层树"), "src/ui/layers/layertreepanel.cpp")
      << make("map.draw.finish", QKeySequence(Qt::Key_Return), "main/map/draw-tool", K, "map",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "完成当前绘制"), drawTool,
              "src/ui/maptools/paleomaptools.cpp")
      << make("map.draw.cancel", QKeySequence(Qt::Key_Escape), "main/map/draw-tool", K, "map",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "放弃当前绘制并退出工具"), drawTool,
              "src/ui/maptools/paleomaptools.cpp")
      << make("map.edit.cancel", QKeySequence(Qt::Key_Escape), "main/map/edit-tool", K, "map",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "取消当前要素编辑并退出工具（移动工具先撤回拖动预览）"),
              editTool, "src/ui/edittools/editingtools.cpp")
      << make("map.vertex.delete", QKeySequence(Qt::Key_Delete), "main/map/vertex-tool", K, "map",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "删除光标处（按吸附结果）的节点"), vertexTool,
              "src/ui/edittools/vertexeditortools.cpp")
      << make("map.vertex.deleteBackspace", QKeySequence(Qt::Key_Backspace), "main/map/vertex-tool", K,
              "map", QT_TRANSLATE_NOOP("PaleoShortcuts", "同 Delete：删除光标处的节点"), vertexTool,
              "src/ui/edittools/vertexeditortools.cpp")
      << make("map.vertex.cancel", QKeySequence(Qt::Key_Escape), "main/map/vertex-tool", K, "map",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "拖动节点时放回原位；未拖动时退出工具"), vertexTool,
              "src/ui/edittools/vertexeditortools.cpp")
      << make("map.siting.cancel", QKeySequence(Qt::Key_Escape), "main/map/siting-tool", K, "map",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "取消布井拾取并退出工具"),
              QT_TRANSLATE_NOOP("PaleoShortcuts", "布井拾取工具（焦点内）"),
              "src/ui/maptools/sitingpicktool.cpp");

  // ===== 剖面与三维 =====
  const char *sectionCanvas = QT_TRANSLATE_NOOP("PaleoShortcuts", "地震剖面画布（焦点内）");
  const char *viewport3d = QT_TRANSLATE_NOOP("PaleoShortcuts", "三维视口（焦点内）");
  const char *sectionSrc = "src/ui/seismicsection/seismicsectioncanvas.cpp";
  const char *viewportSrc = "src/ui/seismic3d/seismic3dviewportwidget.cpp";
  out << make("section.pan.left", QKeySequence(Qt::Key_Left), "main/seismic-section/canvas", K, "sections",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "向左平移剖面视图"), sectionCanvas, sectionSrc)
      << make("section.pan.right", QKeySequence(Qt::Key_Right), "main/seismic-section/canvas", K, "sections",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "向右平移剖面视图"), sectionCanvas, sectionSrc)
      << make("section.pan.up", QKeySequence(Qt::Key_Up), "main/seismic-section/canvas", K, "sections",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "向上平移剖面视图"), sectionCanvas, sectionSrc)
      << make("section.pan.down", QKeySequence(Qt::Key_Down), "main/seismic-section/canvas", K, "sections",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "向下平移剖面视图"), sectionCanvas, sectionSrc)
      << make("section.zoomIn", QKeySequence(Qt::Key_Plus), "main/seismic-section/canvas", K, "sections",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "放大剖面（= 键同效）"), sectionCanvas, sectionSrc)
      << make("section.zoomOut", QKeySequence(Qt::Key_Minus), "main/seismic-section/canvas", K, "sections",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "缩小剖面"), sectionCanvas, sectionSrc)
      << make("section.slicePrev", QKeySequence(Qt::Key_PageUp), "main/seismic-section/canvas", K,
              "sections", QT_TRANSLATE_NOOP("PaleoShortcuts", "切片号后退一步"), sectionCanvas, sectionSrc)
      << make("section.sliceNext", QKeySequence(Qt::Key_PageDown), "main/seismic-section/canvas", K,
              "sections", QT_TRANSLATE_NOOP("PaleoShortcuts", "切片号前进一步"), sectionCanvas, sectionSrc)
      << make("viewport3d.rotate.left", QKeySequence(Qt::Key_Left), "main/seismic3d/viewport", K, "sections",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "向左旋转三维视角"), viewport3d, viewportSrc)
      << make("viewport3d.rotate.right", QKeySequence(Qt::Key_Right), "main/seismic3d/viewport", K,
              "sections", QT_TRANSLATE_NOOP("PaleoShortcuts", "向右旋转三维视角"), viewport3d, viewportSrc)
      << make("viewport3d.rotate.up", QKeySequence(Qt::Key_Up), "main/seismic3d/viewport", K, "sections",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "向上俯仰三维视角"), viewport3d, viewportSrc)
      << make("viewport3d.rotate.down", QKeySequence(Qt::Key_Down), "main/seismic3d/viewport", K, "sections",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "向下俯仰三维视角"), viewport3d, viewportSrc)
      << make("viewport3d.zoomIn", QKeySequence(Qt::Key_Plus), "main/seismic3d/viewport", K, "sections",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "拉近三维视角（= 键同效）"), viewport3d, viewportSrc)
      << make("viewport3d.zoomOut", QKeySequence(Qt::Key_Minus), "main/seismic3d/viewport", K, "sections",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "推远三维视角"), viewport3d, viewportSrc)
      << make("viewport3d.pick.commit", QKeySequence(Qt::Key_Return), "main/seismic3d/viewport/section-pick",
              K, "sections", QT_TRANSLATE_NOOP("PaleoShortcuts", "剖面拾取中：提交折线（至少两点）"),
              viewport3d, viewportSrc)
      << make("viewport3d.pick.cancel", QKeySequence(Qt::Key_Escape), "main/seismic3d/viewport/section-pick",
              K, "sections", QT_TRANSLATE_NOOP("PaleoShortcuts", "剖面拾取中：取消拾取"), viewport3d,
              viewportSrc);

  // ===== 图件设计器（独立窗口）=====
  const char *designer = QT_TRANSLATE_NOOP("PaleoShortcuts", "图件设计器");
  out << make("layout.undo", QKeySequence(QKeySequence::StandardKey::Undo), "layout-designer", S, "layout",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "撤销版面编辑"), designer, "src/ui/layout/layoutundostack.cpp")
      << make("layout.redo", QKeySequence(QKeySequence::StandardKey::Redo), "layout-designer", S, "layout",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "重做版面编辑"), designer, "src/ui/layout/layoutundostack.cpp")
      << make("layout.delete", QKeySequence(Qt::Key_Delete), "layout-designer", S, "layout",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "删除所选版面元素"), designer, "src/ui/layoutdesignershell.cpp");

  // ===== 停靠与对话框 =====
  out << make("dock.drag.cancel", QKeySequence(Qt::Key_Escape), "main/dock-drag", K, "dialogs",
              QT_TRANSLATE_NOOP("PaleoShortcuts", "拖动停靠面板时取消，面板留在原处"),
              QT_TRANSLATE_NOOP("PaleoShortcuts", "停靠面板拖动中"), "src/ui/paleodockmanager.cpp");
  return out;
}
// clang-format on

const ShortcutRegistry &registry()
{
  return built().registry;
}

QStringList catalogErrors()
{
  return built().errors;
}

QKeySequence keyFor(const QString &id)
{
  const ShortcutEntry e = registry().entry(id);
  if (e.id.isEmpty())
    qCWarning(lcPaleoShortcuts) << "unregistered shortcut id:" << id;
  return e.key;
}

QShortcut *bindShortcut(const QString &id, QWidget *parent)
{
  const ShortcutEntry e = registry().entry(id);
  if (e.id.isEmpty())
    qCWarning(lcPaleoShortcuts) << "bindShortcut: unregistered id" << id;
  else if (e.binding != Binding::Shortcut)
    qCWarning(lcPaleoShortcuts) << "bindShortcut: id is a key-handler entry" << id;
  auto *sc = new QShortcut(e.key, parent);
  sc->setContext(e.qtContext);
  sc->setProperty(kShortcutIdProperty, id);
  return sc;
}

void bindAction(const QString &id, QAction *action)
{
  setActionShortcutActive(id, action, true);
}

void setActionShortcutActive(const QString &id, QAction *action, bool active)
{
  if (!action)
    return;
  const ShortcutEntry e = registry().entry(id);
  if (e.id.isEmpty())
    qCWarning(lcPaleoShortcuts) << "bindAction: unregistered id" << id;
  action->setShortcut(active ? e.key : QKeySequence());
  action->setShortcutContext(e.qtContext);
  action->setProperty(kShortcutIdProperty, id);
}

QString displayDescription(const ShortcutEntry &entry)
{
  return QCoreApplication::translate(kTrContext, entry.description.toUtf8().constData());
}

QString displaySourcePanel(const ShortcutEntry &entry)
{
  return QCoreApplication::translate(kTrContext, entry.sourcePanel.toUtf8().constData());
}

QString displayGroup(const QString &groupId)
{
  for (const GroupRow &g : kGroups)
    if (groupId == QLatin1String(g.id))
      return QCoreApplication::translate(kTrContext, g.title);
  return groupId;
}

QString displayKey(const QKeySequence &key)
{
  return key.toString(QKeySequence::NativeText);
}

QString contextForWidget(const QWidget *widget)
{
  // 面板类 → 作用域路径（深者在前无所谓：自下而上找最近的祖先）。
  static const QList<QPair<const char *, const char *>> panels = {
      {"WellComposite::WellCompositeCanvas", "main/data/wellcomposite/canvas"},
      {"WellComposite::WellCompositePanel", "main/data/wellcomposite"},
      {"paleo::dataops::DataNavTree", "main/data/assets"},
      {"DerivationGraph", "main/data/derivation-graph"},
      {"DataListPanel", "main/data"},
      {"DataPage", "main/data"},
      {"seismic::SeismicSectionCanvas", "main/seismic-section/canvas"},
      {"seismic::SeismicSectionDockWidget", "main/seismic-section"},
      {"seismic::Seismic3DViewportWidget", "main/seismic3d/viewport"},
      {"QgsMapCanvas", "main/map"},
      {"PaleoLayoutDesignerShell", "layout-designer"},
      {"PaleoMainWindow", "main"},
  };
  for (const QWidget *w = widget; w; w = w->parentWidget())
    for (const auto &p : panels)
      if (w->inherits(p.first))
        return QString::fromLatin1(p.second);
  return QString();
}

int logConflictsOnce()
{
  static const int conflicts = [] {
    const ShortcutRegistry &reg = registry();
    for (const QString &err : catalogErrors())
      qCWarning(lcPaleoShortcuts).noquote() << "rejected catalog entry:" << err;
    const QList<ShortcutConflict> found = reg.conflicts();
    const QList<ShortcutConflict> shadowed = reg.shadows();
    qCInfo(lcPaleoShortcuts).noquote()
        << QStringLiteral("shortcut registry: %1 entries (%2 bound, %3 key-handler), %4 conflicts, %5 shadows")
               .arg(reg.size())
               .arg(reg.entriesIn(Binding::Shortcut).size())
               .arg(reg.entriesIn(Binding::KeyHandler).size())
               .arg(found.size())
               .arg(shadowed.size());
    for (const ShortcutConflict &c : found)
      qCWarning(lcPaleoShortcuts).noquote() << "conflict" << c.describe();
    for (const ShortcutConflict &c : shadowed)
      qCInfo(lcPaleoShortcuts).noquote() << "shadow" << c.describe();
    return int(found.size());
  }();
  return conflicts;
}

} // namespace paleo::shortcuts
