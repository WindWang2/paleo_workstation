// 层：视图
#include "whatsthiscatalog.h"

#include "../shortcuts/shortcutcatalog.h"

#include <QCoreApplication>
#include <QWidget>

namespace paleo::help
{

namespace
{
constexpr char kTrContext[] = "PaleoWhatsThis";

bool insideScope(const QWidget *w, const char *scopeClass)
{
  for (const QWidget *p = w; p; p = p->parentWidget())
    if (p->inherits(scopeClass))
      return true;
  return false;
}
} // namespace

// clang-format off
QList<WhatsThisEntry> whatsThisEntries()
{
  return {
    // ---- 主窗口外壳（六页共用的 chrome 与起始页）----
    {"PaleoMainWindow", "workflowTabs", QT_TRANSLATE_NOOP("PaleoWhatsThis", "工作流页签：按数据管理、地层对比、智能预测、单因素图、智能编图、验证的顺序切换六个工作页。"), nullptr},
    {"PaleoMainWindow", "paleoLocator", QT_TRANSLATE_NOOP("PaleoWhatsThis", "顶栏搜索：输入井名或层位名快速定位；选中层位会切换当前层位。"), "main.locator.focus"},
    {"PaleoMainWindow", "panelsMenuButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "布局菜单：显示或隐藏各停靠面板、保存与恢复布局，并可切换深色模式和紧凑密度。"), nullptr},
    {"PaleoMainWindow", "processingButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "处理算法：按提供者分组列出可用算法，点击后打开该算法的参数对话框。"), nullptr},
    {"PaleoMainWindow", "webServiceButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "显示或隐藏「Web 服务」面板，在应用内打开网页服务地址。"), nullptr},
    {"PaleoMainWindow", "helpMenuButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "帮助菜单：快捷键总表、「这是什么？」模式与关于信息。"), "main.help.shortcuts"},
    {"PaleoMainWindow", "horizonChips", QT_TRANSLATE_NOOP("PaleoWhatsThis", "层位切换条：点击层位切换当前层位，地图与各页随之刷新；有图层正在编辑时会拒绝切换。"), nullptr},
    {"PaleoMainWindow", "stopMapToolButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "结束当前地图操作（绘制、编辑、拾取等）；尚未保存的图层编辑会保留。"), nullptr},
    {"PaleoMainWindow", "statusCoords", QT_TRANSLATE_NOOP("PaleoWhatsThis", "鼠标所在位置的工程坐标，单位米。"), nullptr},
    {"PaleoMainWindow", "statusScale", QT_TRANSLATE_NOOP("PaleoWhatsThis", "当前地图比例尺。"), nullptr},
    {"PaleoMainWindow", "statusCrs", QT_TRANSLATE_NOOP("PaleoWhatsThis", "坐标系说明：局部工程坐标、单位米、未投影，不是经纬度。"), nullptr},
    {"PaleoMainWindow", "statusHorizon", QT_TRANSLATE_NOOP("PaleoWhatsThis", "当前层位；各页的编图与预测都针对这个层位。"), nullptr},
    {"PaleoMainWindow", "newProjectButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "新建一个空的 Paleo 工程文件（.qgz）。"), nullptr},
    {"PaleoMainWindow", "openProjectButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "打开已有的 Paleo 工程。"), nullptr},
    {"PaleoMainWindow", "importFromFolderButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "选择工区文件夹：已有工程直接打开，否则在该文件夹内新建工程并导入其中的数据。"), nullptr},
    {"PaleoMainWindow", "recentProjectsList", QT_TRANSLATE_NOOP("PaleoWhatsThis", "最近打开的工程，双击或回车即可打开。"), nullptr},

    // ---- 数据管理页（数据列表）----
    {"DataListPanel", "assetSearchEdit", QT_TRANSLATE_NOOP("PaleoWhatsThis", "按名称、类型或关联井搜索资产，列表即时过滤。"), "data.list.focusSearch"},
    {"DataListPanel", "assetTypeFilter", QT_TRANSLATE_NOOP("PaleoWhatsThis", "只显示所选类型的资产。"), nullptr},
    {"DataListPanel", "dataTree", QT_TRANSLATE_NOOP("PaleoWhatsThis", "数据导航树：按层级浏览工区资产与井等实体；选中即可预览，也可把资产拖到井上建立关联。"), nullptr},
    {"DataListPanel", "treeViewButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "以树形层级显示资产。"), nullptr},
    {"DataListPanel", "listViewButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "以表格列表显示资产，便于排序和批量选择。"), nullptr},
    {"DataListPanel", "dataUndoButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "撤销上一步数据列表操作。"), "data.list.undo"},
    {"DataListPanel", "dataRedoButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "重做刚撤销的数据列表操作。"), "data.list.redo"},
    {"DataListPanel", "dataListOptionsButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "展开或收起筛选、视图、排序与标签等低频选项。"), nullptr},
    {"DataListPanel", "healthCheckButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "工区体检：检查缺失文件、文件校验值不一致、未决链接与孤立实体等问题。"), nullptr},
    {"DataListPanel", "importLedgerButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "导入台账：查看最近导入批次中每个文件的结局（入库、未决、失败或跳过）。"), nullptr},
    {"DataListPanel", "pendingResolveButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "按文件名或备注，把能唯一匹配到某口井的未决链接批量挂接上去。"), nullptr},
    {"DataListPanel", "storageGovernanceButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "存储治理台：汇总占用体积、扫描未引用文件、预览可回收的过时版本。"), nullptr},

    // ---- 地层对比页 ----
    {"StratigraphicWebPage", "correlationWebAddress", QT_TRANSLATE_NOOP("PaleoWhatsThis", "地层对比网页工作台的服务地址。"), nullptr},
    {"StratigraphicWebPage", "correlationWebStatus", QT_TRANSLATE_NOOP("PaleoWhatsThis", "地层对比服务的连接状态与下一步提示。"), nullptr},

    // ---- 智能预测页 ----
    {"PredictPage", "predictTypeCombo", QT_TRANSLATE_NOOP("PaleoWhatsThis", "选择预测类型：沉积相、地震相或测井相。"), nullptr},
    {"PredictPage", "algoCombo", QT_TRANSLATE_NOOP("PaleoWhatsThis", "选择预测算法；下方参数区按所选算法重建。"), nullptr},
    {"PredictPage", "runButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "按当前类型、算法与参数运行预测。"), nullptr},
    {"PredictPage", "cancelRunButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "取消正在运行的预测。"), nullptr},
    {"PredictPage", "historyList", QT_TRANSLATE_NOOP("PaleoWhatsThis", "当前层位的历史预测结果；点行内「显示」可在地图上查看。"), nullptr},
    {"PredictPage", "mamclButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "启动独立的地震多属性智能分析程序（MAMCL）；首次启动会自动准备 Python 环境。"), nullptr},

    // ---- 单因素图页 ----
    {"ConstraintPage", "factorTable", QT_TRANSLATE_NOOP("PaleoWhatsThis", "单因素图清单：勾选一行作为当前要生成或查看的因素，并显示其输入与状态。"), nullptr},
    {"ConstraintPage", "factorMethodCombo", QT_TRANSLATE_NOOP("PaleoWhatsThis", "选择单因素图的成图方法，如本地方向插值或克里金。"), nullptr},
    {"ConstraintPage", "generateFactorButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "按所选方法和参数生成勾选的单因素图。"), nullptr},
    {"ConstraintPage", "drawButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "在地图上绘制所选形状的约束。"), nullptr},
    {"ConstraintPage", "constraintList", QT_TRANSLATE_NOOP("PaleoWhatsThis", "已有约束列表；选中后可修改属性或删除。"), nullptr},

    // ---- 智能编图页 ----
    {"ComposePage", "fuseButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "把因素列表中勾选的单因素图合成为编图结果。"), nullptr},
    {"ComposePage", "polygonizeButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "把相栅格转为相多边形，按最小面积和简化容差清理碎片。"), nullptr},
    {"ComposePage", "exportPdfButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "把当前层位图导出为 PDF。"), nullptr},
    {"ComposePage", "openDesignerButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "在图件设计器中打开当前版面做细调。"), nullptr},
    {"ComposePage", "saveVersionButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "把当前编图保存为一个版本。"), nullptr},
    {"ComposePage", "publishButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "发布当前版本；需先导出 PDF。"), nullptr},

    // ---- 验证页 ----
    {"ValidatePage", "runButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "运行验证，刷新井点残差表与问题列表。"), nullptr},
    {"ValidatePage", "residualTable", QT_TRANSLATE_NOOP("PaleoWhatsThis", "每口井一行：时间残差（或无法计算的原因）与阈值。"), nullptr},
    {"ValidatePage", "issueTable", QT_TRANSLATE_NOOP("PaleoWhatsThis", "验证发现的问题：级别、代码、说明与涉及的图层。"), nullptr},
    {"ValidatePage", "openSeismicSectionButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "在数据页打开所选问题或残差行对应的剖面。"), nullptr},

    // ---- 剖面停靠面板（地震剖面 / 井震综合）----
    {"seismic::SeismicSectionDockWidget", "cboSectionMode", QT_TRANSLATE_NOOP("PaleoWhatsThis", "剖面类型：纵测线、横测线、时间切片或任意测线/井剖面。"), nullptr},
    {"seismic::SeismicSectionDockWidget", "sliderSectionSlice", QT_TRANSLATE_NOOP("PaleoWhatsThis", "拖动选择当前测线号或时间切片。"), nullptr},
    {"seismic::SeismicSectionDockWidget", "btnSectionFit", QT_TRANSLATE_NOOP("PaleoWhatsThis", "把剖面缩放到适应窗口。"), nullptr},
    {"seismic::SeismicSectionDockWidget", "btnSectionUnitToggle", QT_TRANSLATE_NOOP("PaleoWhatsThis", "切换纵轴：双程时间或常速参考深度；井段仍按本井时深表对齐。"), nullptr},
    {"seismic::SeismicSectionDockWidget", "cboSectionColorMap", QT_TRANSLATE_NOOP("PaleoWhatsThis", "选择剖面色标。"), nullptr},
    {"seismic::SeismicSectionDockWidget", "sliderSectionGain", QT_TRANSLATE_NOOP("PaleoWhatsThis", "调整振幅增益（0.1 至 5 倍）。"), nullptr},
    {"seismic::SeismicSectionDockWidget", "btnSectionCurtain", QT_TRANSLATE_NOOP("PaleoWhatsThis", "卷帘对比：帘左为当前线、帘右为相邻线，在画布内拖动分割线。"), nullptr},
    {"seismic::SeismicSectionDockWidget", "btnSectionExport", QT_TRANSLATE_NOOP("PaleoWhatsThis", "把当前剖面导出为图件。"), nullptr},

    // ---- 综合柱状图 ----
    {"WellComposite::WellCompositePanel", "scaleCombo", QT_TRANSLATE_NOOP("PaleoWhatsThis", "选择纵向比例尺，或自适应窗口高度。"), nullptr},
    {"WellComposite::WellCompositePanel", "btnCompResetZoom", QT_TRANSLATE_NOOP("PaleoWhatsThis", "恢复显示全井段；也可双击道内任意位置。"), nullptr},
    {"WellComposite::WellCompositePanel", "btnCompGoto", QT_TRANSLATE_NOOP("PaleoWhatsThis", "跳转到指定深度。"), "wellcomposite.gotoDepth"},
    {"WellComposite::WellCompositePanel", "btnCompBookmarks", QT_TRANSLATE_NOOP("PaleoWhatsThis", "深度书签：添加、跳转或删除。"), nullptr},
    {"WellComposite::WellCompositePanel", "btnCompSnap", QT_TRANSLATE_NOOP("PaleoWhatsThis", "开启后，深度标尺吸附到整刻度和标志层线。"), nullptr},
    {"WellComposite::WellCompositePanel", "btnConfigCurves", QT_TRANSLATE_NOOP("PaleoWhatsThis", "测井道配置：调整各道顺序，合并或拆分曲线道。"), nullptr},
    {"WellComposite::WellCompositePanel", "btnCompExport", QT_TRANSLATE_NOOP("PaleoWhatsThis", "导出 PDF、PNG、SVG 或打印，并管理导出预设。"), nullptr},
    {"WellComposite::WellCompositePanel", "btnCompEdit", QT_TRANSLATE_NOOP("PaleoWhatsThis", "进入标志层/区间编辑模式：拖动标志层线可修改顶深，区间可编辑。"), nullptr},
    {"WellComposite::WellCompositePanel", "btnCompSaveDerived", QT_TRANSLATE_NOOP("PaleoWhatsThis", "把当前编辑保存为派生版本，原始井数据保持不变。"), nullptr},
    {"WellComposite::WellCompositePanel", "lblCompReadout", QT_TRANSLATE_NOOP("PaleoWhatsThis", "当前深度读数与最近的标志层名。"), nullptr},
  };
}
// clang-format on

QString whatsThisText(const WhatsThisEntry &entry)
{
  QString text = QCoreApplication::translate(kTrContext, entry.text);
  if (entry.shortcutId)
  {
    const QKeySequence key =
        shortcuts::registry().key(QString::fromLatin1(entry.shortcutId));
    if (!key.isEmpty())
      text += QCoreApplication::translate("PaleoWhatsThis", "（快捷键：%1）")
                  .arg(shortcuts::displayKey(key));
  }
  return text;
}

int applyWhatsThis(QWidget *root)
{
  if (!root)
    return 0;
  int applied = 0;
  for (const WhatsThisEntry &e : whatsThisEntries())
  {
    const QString name = QString::fromLatin1(e.objectName);
    QList<QWidget *> candidates = root->findChildren<QWidget *>(name);
    if (root->objectName() == name)
      candidates.prepend(root);
    for (QWidget *w : candidates)
    {
      if (!w->whatsThis().isEmpty() || !insideScope(w, e.scopeClass))
        continue;
      w->setWhatsThis(whatsThisText(e));
      ++applied;
    }
  }
  return applied;
}

} // namespace paleo::help
