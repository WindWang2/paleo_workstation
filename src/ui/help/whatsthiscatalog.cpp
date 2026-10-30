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
    {"PaleoMainWindow", "statusCoords", QT_TRANSLATE_NOOP("PaleoWhatsThis", "鼠标所在位置的坐标：由 QGIS 按工程显示设置格式化；未配准工程显示局部米制网格，已配准工程按显示设置格式化，不冒充经纬度。"), nullptr},
    {"PaleoMainWindow", "statusCoordinateToolsButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "QGIS 坐标工具：输入或地图拾取点位，选择来源与目标 CRS，执行坐标转换、定位，或复制地图坐标与 WGS84 经纬度。"), nullptr},
    {"PaleoMainWindow", "statusScale", QT_TRANSLATE_NOOP("PaleoWhatsThis", "当前地图比例尺。"), nullptr},
    {"PaleoMainWindow", "statusCrs", QT_TRANSLATE_NOOP("PaleoWhatsThis", "坐标系：未配准的工程显示「工程坐标 · 米 · 未投影」（局部坐标，不是经纬度）；已配准的工程显示地图坐标系代号（没有代号时显示坐标系名称）。"), nullptr},
    {"PaleoMainWindow", "statusHorizon", QT_TRANSLATE_NOOP("PaleoWhatsThis", "界面编辑模式的当前层位（上方层位条）；绘制约束与井因子仍按此界面。分析请看「分析层段」。"), nullptr},
    {"PaleoMainWindow", "statusUnit", QT_TRANSLATE_NOOP("PaleoWhatsThis", "当前分析层段（父单元/S1–S4）；与层位条独立，切换不重置地图或编辑。"), nullptr},
    {"PaleoMainWindow", "subIntervalSelectorBar", QT_TRANSLATE_NOOP("PaleoWhatsThis", "层段分析选择：先选父层间单元（如 T70–T71），再选 S1–S4 或综合；各分析页共用同一选择。"), nullptr},
    {"PaleoMainWindow", "subIntervalParentCombo", QT_TRANSLATE_NOOP("PaleoWhatsThis", "父层间单元：相邻两条界面构成；展开「状态」可查看四个子层段就绪情况。"), nullptr},
    {"PaleoMainWindow", "subIntervalS1", QT_TRANSLATE_NOOP("PaleoWhatsThis", "子层段 S1（自上而下第一段）。选中后单因素/编图/相分析按稳定子层段 ID 归属。"), nullptr},
    {"PaleoMainWindow", "subIntervalS2", QT_TRANSLATE_NOOP("PaleoWhatsThis", "子层段 S2（自上而下第二段）。选中后单因素/编图/相分析按稳定子层段 ID 归属。"), nullptr},
    {"PaleoMainWindow", "subIntervalS3", QT_TRANSLATE_NOOP("PaleoWhatsThis", "子层段 S3（自上而下第三段）。选中后单因素/编图/相分析按稳定子层段 ID 归属。"), nullptr},
    {"PaleoMainWindow", "subIntervalS4", QT_TRANSLATE_NOOP("PaleoWhatsThis", "子层段 S4（自上而下第四段，含底界）。选中后单因素/编图/相分析按稳定子层段 ID 归属。"), nullptr},
    {"PaleoMainWindow", "subIntervalAggregate", QT_TRANSLATE_NOOP("PaleoWhatsThis", "父单元显式综合（厚度加权）；缺任一子层段成果时综合不可用。"), nullptr},
    {"PaleoMainWindow", "subIntervalExpand", QT_TRANSLATE_NOOP("PaleoWhatsThis", "展开查看当前父单元四个子层段的稳定 ID 与选中状态。"), nullptr},
    {"PaleoMainWindow", "subIntervalModeLabel", QT_TRANSLATE_NOOP("PaleoWhatsThis", "层段分析模式标识；与上方界面编辑层位条分工，互不重置对方状态。"), nullptr},

    {"PaleoMainWindow", "newProjectButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "通过可跳过的引导创建 Paleo 工程，可自动识别井点、井资料、地震和层位；也可创建空白工程。"), nullptr},
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
    {"StratigraphyPage", "stratigraphyWells", QT_TRANSLATE_NOOP("PaleoWhatsThis", "选择当前工区中参与对比的井；输入资料可显式指定或由优选方法选择。"), nullptr},
    {"StratigraphyPage", "stratigraphyStatus", QT_TRANSLATE_NOOP("PaleoWhatsThis", "显示界面计算、模板划分与版本保存的状态，以及需复核的来源和约定问题。"), nullptr},

    // ---- 智能预测页 ----
    {"PredictPage", "predictTypeCombo", QT_TRANSLATE_NOOP("PaleoWhatsThis", "沉积相 / 地震相 / 测井相。地震相切片预测与层段综合分列；测井相走井面板，砂地比等单因素按子层段深度窗。"), nullptr},

    {"PredictPage", "horizonCombo", QT_TRANSLATE_NOOP("PaleoWhatsThis", "历史层位下拉（兼容）。层段分析优先用画布父单元→S1–S4；成果身份为 SubIntervalId。"), nullptr},    {"PredictPage", "algoCombo", QT_TRANSLATE_NOOP("PaleoWhatsThis", "选择预测算法；下方参数区按所选算法重建。"), nullptr},
    {"PredictPage", "runButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "按层段分析选择（父单元/S1–S4）或层位下拉运行；成果登记 Data Service，可供编图证据拉取。"), nullptr},
    {"PredictPage", "cancelRunButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "取消正在运行的预测。"), nullptr},
    {"PredictPage", "historyList", QT_TRANSLATE_NOOP("PaleoWhatsThis", "当前层位的历史预测结果；点行内「显示」可在地图上查看。"), nullptr},
    {"PredictPage", "mamclButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "启动独立的地震多属性智能分析程序（MAMCL）；首次启动会自动准备 Python 环境。"), nullptr},

    // ---- 单因素图页 ----
    {"ConstraintPage", "factorTable", QT_TRANSLATE_NOOP("PaleoWhatsThis", "单因素图清单：勾选一行作为当前要生成或查看的因素，并显示其输入与状态。"), nullptr},
    {"ConstraintPage", "factorMethodCombo", QT_TRANSLATE_NOOP("PaleoWhatsThis", "选择单因素图的成图方法，如本地方向插值或克里金。"), nullptr},
    {"ConstraintPage", "factorOneClickButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "一键成图：先提取并保存井点数据，成功后自动按当前参数生成单因素图；任一步失败或被约束门控拒绝即停下并说明原因。"), nullptr},
    {"ConstraintPage", "extractWellFactorsButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "按当前取值方式提取当前地层的井点数据，保存为可复用的井点版本。"), nullptr},
    {"ConstraintPage", "contourButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "从当前因素已保存的单因素图提取等值线，并保存为新的成果版本。"), nullptr},
    {"ConstraintPage", "generateFactorButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "按所选方法和参数生成勾选的单因素图。"), nullptr},
    {"ConstraintPage", "drawButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "在地图上绘制所选形状的约束。"), nullptr},
    {"ConstraintPage", "constraintList", QT_TRANSLATE_NOOP("PaleoWhatsThis", "已有约束列表；选中后可修改属性或删除。"), nullptr},
    {"ConstraintPage", "directionButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "在地图上绘制当前层位的方向线族约束线；语义由右侧下拉选——方向引导定供源方向（各向异性轴）、解释软边界减弱跨线影响、制图绕行只调整解释性等值线；仅支持约束的插值方法会消费它。"), nullptr},
    {"ConstraintPage", "breakLineButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "在地图上绘制当前层位的间隔线族约束线；语义由右侧下拉选——硬屏障阻断跨线传播、等值线止于该线，等值停止只终止提线、不改变插值数值；仅支持约束的插值方法会消费它。"), nullptr},
    {"ConstraintPage", "barrierImportButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "导入线状矢量作为当前层位的间隔线族约束，按所选硬屏障或等值停止语义使用；此步骤可跳过。"), nullptr},
    {"ConstraintPage", "directionImportButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "导入线状矢量作为当前层位的方向线族约束，按所选方向引导、解释软边界或制图绕行语义使用；此步骤可跳过。"), nullptr},

    // ---- 智能编图页 ----
    {"ComposePage", "thicknessChainButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "按所选层位生成等厚图：等时差（双向 ms）× 层间速度 IDW → 厚度栅格；需先在顶部层位 chip 选择层位。"), nullptr},
    {"ComposePage", "fuseButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "把因素列表中勾选的单因素图合成为编图结果。"), nullptr},
    {"ComposePage", "polygonizeButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "把相栅格转为相多边形，按最小面积和简化容差清理碎片。"), nullptr},
    {"ComposePage", "exportPdfButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "把当前层位图导出为 PDF。"), nullptr},
    {"ComposePage", "openDesignerButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "在图件设计器中打开当前版面做细调。"), nullptr},
    {"ComposePage", "saveVersionButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "把当前编图保存为一个版本。"), nullptr},
    {"ComposePage", "publishButton", QT_TRANSLATE_NOOP("PaleoWhatsThis", "发布当前版本；需先导出 PDF，并且每口井都已有验证残差或不可计算的原因（先在验证页运行验证）。"), nullptr},

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
