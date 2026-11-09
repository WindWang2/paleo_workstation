// 层：数据
// services/commandspec.cpp — 见 commandspec.h。条目即 U0 命令盘点：主窗 ribbon 面板、
// 文件菜单/快速访问、Ctrl+1..N 页快捷键、布局右键菜单的全部命令，外加只开放给
// Agent/批处理的 Callable 能力。tst_commandshell 在真实主窗上校验「每个 ribbon/菜单
// 动作都带登记过的 commandId」，新增入口必须先在此登记。
#include "commandspec.h"

#include "capabilityspec.h"

#include <QCoreApplication>
#include <QSet>

namespace paleo::command {
namespace {

constexpr int kUi = SurfaceRibbon | SurfaceLocator;
constexpr int kMenu = SurfaceMenu | SurfaceLocator;

struct Row {
  const char *id;
  CommandKind kind;
  const char *stage;
  const char *title;
  const char *tooltip;
  int surfaces;
  const char *effect = "ui";
  const char *capability = "";
  const char *group = "";
  const char *shortcut = "";
  const char *context = ""; // 逗号分隔
};

// 标题/说明是 PaleoCommands 上下文的源串：逐条直写 QT_TRANSLATE_NOOP（lupdate 不展开宏，
// 包一层自定义宏会让 lupdate / check_ts_stale 都看不见这些串）。
const Row kRows[] = {
    // ---- 工程（文件菜单 / 快速访问 / 起始页；U4 Backstage 前的过渡位置）----
    {"project.new", CommandKind::Command, "project", QT_TRANSLATE_NOOP("PaleoCommands", "新建工程"), QT_TRANSLATE_NOOP("PaleoCommands", "新建空白工程并设置资料来源"), kMenu},
    {"project.open", CommandKind::Command, "project", QT_TRANSLATE_NOOP("PaleoCommands", "打开工程"), QT_TRANSLATE_NOOP("PaleoCommands", "打开 .paleo / QGIS 工程"), kMenu},
    {"project.newFromFolder", CommandKind::Command, "project", QT_TRANSLATE_NOOP("PaleoCommands", "从工区文件夹新建"),
     QT_TRANSLATE_NOOP("PaleoCommands", "选择工区目录：已有工程直接打开，否则在目录内建工程并导入"), kMenu},
    {"project.save", CommandKind::Command, "project", QT_TRANSLATE_NOOP("PaleoCommands", "保存工程"), QT_TRANSLATE_NOOP("PaleoCommands", "保存当前工程"), kMenu | SurfaceShortcut,
     "stage", "", "", "main.project.save", "project"},
    {"project.sources", CommandKind::Command, "project", QT_TRANSLATE_NOOP("PaleoCommands", "工程资料设置"), QT_TRANSLATE_NOOP("PaleoCommands", "补充或调整当前工程的资料来源"), kMenu,
     "stage", "", "", "", "project"},
    {"project.mapSettings", CommandKind::Command, "project", QT_TRANSLATE_NOOP("PaleoCommands", "工程坐标与底图"), QT_TRANSLATE_NOOP("PaleoCommands", "设置工程坐标参考与底图"), kMenu,
     "stage", "", "", "", "project"},
    {"project.coordinateTools", CommandKind::View, "project", QT_TRANSLATE_NOOP("PaleoCommands", "坐标定位与转换"),
     QT_TRANSLATE_NOOP("PaleoCommands", "QGIS 坐标定位、格式转换与基准操作"), kMenu},
    {"project.basemap.online", CommandKind::Command, "project", QT_TRANSLATE_NOOP("PaleoCommands", "加入网络底图预设"),
     QT_TRANSLATE_NOOP("PaleoCommands", "把选定的在线底图加入图层树（参数 presetId）"), SurfaceMenu, "stage", "", "", "", "project"},
    {"project.home", CommandKind::View, "project", QT_TRANSLATE_NOOP("PaleoCommands", "起始页"), QT_TRANSLATE_NOOP("PaleoCommands", "回到起始页"), kMenu},
    {"app.quit", CommandKind::Command, "project", QT_TRANSLATE_NOOP("PaleoCommands", "退出"), QT_TRANSLATE_NOOP("PaleoCommands", "退出应用"), kMenu},

    // ---- 页签切换（Ctrl+1..N）----
    {"view.stage.data", CommandKind::View, "view", QT_TRANSLATE_NOOP("PaleoCommands", "切到「数据管理」页签"), "", SurfaceShortcut | SurfaceLocator,
     "ui", "", "", "main.page.data"},
    {"view.stage.correlation", CommandKind::View, "view", QT_TRANSLATE_NOOP("PaleoCommands", "切到「地层对比」页签"), "",
     SurfaceShortcut | SurfaceLocator, "ui", "", "", "main.page.correlation"},
    {"view.stage.predict", CommandKind::View, "view", QT_TRANSLATE_NOOP("PaleoCommands", "切到「单井相分析」页签"), "",
     SurfaceShortcut | SurfaceLocator, "ui", "", "", "main.page.predict"},
    {"view.stage.seismic", CommandKind::View, "view", QT_TRANSLATE_NOOP("PaleoCommands", "切到「地震相分析」页签"), "",
     SurfaceShortcut | SurfaceLocator, "ui", "", "", "main.page.seismic"},
    {"view.stage.compose", CommandKind::View, "view", QT_TRANSLATE_NOOP("PaleoCommands", "切到「古地理编图」页签"), "",
     SurfaceShortcut | SurfaceLocator, "ui", "", "", "main.page.compose"},

    // ---- 打开任务视图（各页签「视图 / 分析任务」组）----
    {"view.open.data", CommandKind::View, "data", QT_TRANSLATE_NOOP("PaleoCommands", "打开数据视图"), QT_TRANSLATE_NOOP("PaleoCommands", "打开当前工程的数据列表与资料预览。"), kUi,
     "ui", "", "数据管理/视图"},
    {"view.open.correlation", CommandKind::View, "strat", QT_TRANSLATE_NOOP("PaleoCommands", "打开地层对比"),
     QT_TRANSLATE_NOOP("PaleoCommands", "打开原生连井曲线与分层结果，使用当前工区的数据服务。"), kUi, "ui", "", "地层对比/视图"},
    {"view.open.wellPrediction", CommandKind::View, "well", QT_TRANSLATE_NOOP("PaleoCommands", "井预测"),
     QT_TRANSLATE_NOOP("PaleoCommands", "按当前地层预测井的相分类并保存井预测结果。"), kUi, "ui", "", "单井相分析/分析任务"},
    {"view.open.singleFactor", CommandKind::View, "well", QT_TRANSLATE_NOOP("PaleoCommands", "编制单因素图"),
     QT_TRANSLATE_NOOP("PaleoCommands", "按当前地层选择单因素方法，编制对应图件。"), kUi, "ui", "", "单井相分析/分析任务"},
    {"view.open.seismicFacies", CommandKind::View, "seismic", QT_TRANSLATE_NOOP("PaleoCommands", "地震相预测"), QT_TRANSLATE_NOOP("PaleoCommands", "预测地震相并生成面状结果。"),
     kUi, "ui", "", "地震相分析/分析任务"},
    {"view.open.validate", CommandKind::View, "release", QT_TRANSLATE_NOOP("PaleoCommands", "送交验证"), QT_TRANSLATE_NOOP("PaleoCommands", "切到验证页，用井点残差检查当前成果"),
     kUi, "ui", "", "结果"},

    // ---- 窗格 / 视图 ----
    {"view.params", CommandKind::View, "view", QT_TRANSLATE_NOOP("PaleoCommands", "参数面板"), QT_TRANSLATE_NOOP("PaleoCommands", "显示/隐藏右侧参数面板"), kUi},
    {"view.attributeTable", CommandKind::View, "view", QT_TRANSLATE_NOOP("PaleoCommands", "属性表"), QT_TRANSLATE_NOOP("PaleoCommands", "在底部面板打开图层属性表"), kUi},
    {"view.releases", CommandKind::View, "release", QT_TRANSLATE_NOOP("PaleoCommands", "发布记录"), QT_TRANSLATE_NOOP("PaleoCommands", "在底部面板查看已发布的版本"), kUi},
    {"view.wellSection", CommandKind::View, "strat", QT_TRANSLATE_NOOP("PaleoCommands", "连井剖面"), QT_TRANSLATE_NOOP("PaleoCommands", "按地层连井：多井对比、分层连线与井间地震"),
     kUi},
    {"view.sectionWorkbench", CommandKind::View, "seismic", QT_TRANSLATE_NOOP("PaleoCommands", "井震剖面"), QT_TRANSLATE_NOOP("PaleoCommands", "打开井震剖面工作台"), kUi},
    {"view.seismic3d", CommandKind::View, "seismic", QT_TRANSLATE_NOOP("PaleoCommands", "三维视口"), QT_TRANSLATE_NOOP("PaleoCommands", "打开三维地震体立体视口"), kUi},
    {"view.seismic2d", CommandKind::View, "seismic", QT_TRANSLATE_NOOP("PaleoCommands", "地震剖面"), QT_TRANSLATE_NOOP("PaleoCommands", "打开二维地震与井震标定剖面"), kUi},
    {"view.errorHistory", CommandKind::View, "view", QT_TRANSLATE_NOOP("PaleoCommands", "错误历史"), QT_TRANSLATE_NOOP("PaleoCommands", "打开错误历史面板"),
     SurfaceContextMenu | SurfaceLocator},
    {"view.theme.dark", CommandKind::View, "view", QT_TRANSLATE_NOOP("PaleoCommands", "深色模式"), QT_TRANSLATE_NOOP("PaleoCommands", "切换浅色/深色主题"),
     SurfaceContextMenu | SurfaceLocator},
    {"view.density.compact", CommandKind::View, "view", QT_TRANSLATE_NOOP("PaleoCommands", "紧凑密度"), QT_TRANSLATE_NOOP("PaleoCommands", "切换舒适/紧凑界面密度"),
     SurfaceContextMenu | SurfaceLocator},
    {"view.more.constraint", CommandKind::View, "well", QT_TRANSLATE_NOOP("PaleoCommands", "单因素图更多工具"),
     QT_TRANSLATE_NOOP("PaleoCommands", "连井剖面、井震剖面、地图导航、相图显示、捕捉与撤销"), SurfaceRibbon},

    // ---- 地图导航（QGIS 原生工具）----
    {"map.pan", CommandKind::View, "map", QT_TRANSLATE_NOOP("PaleoCommands", "平移"), QT_TRANSLATE_NOOP("PaleoCommands", "拖动平移地图"), kUi},
    {"map.zoomIn", CommandKind::View, "map", QT_TRANSLATE_NOOP("PaleoCommands", "放大"), QT_TRANSLATE_NOOP("PaleoCommands", "点击或框选放大"), kUi},
    {"map.zoomOut", CommandKind::View, "map", QT_TRANSLATE_NOOP("PaleoCommands", "缩小"), QT_TRANSLATE_NOOP("PaleoCommands", "点击或框选缩小"), kUi},
    {"map.zoomFull", CommandKind::View, "map", QT_TRANSLATE_NOOP("PaleoCommands", "全图"), QT_TRANSLATE_NOOP("PaleoCommands", "缩放到全部图层"), kUi},
    {"map.identify", CommandKind::View, "map", QT_TRANSLATE_NOOP("PaleoCommands", "识别"), QT_TRANSLATE_NOOP("PaleoCommands", "指针落到要素上时显示悬浮信息，多个命中可切换"), kUi},

    // ---- ① 资料 ----
    {"data.import.paleomap", CommandKind::Command, "data", QT_TRANSLATE_NOOP("PaleoCommands", "导入现有古地理图"),
     QT_TRANSLATE_NOOP("PaleoCommands", "按层位管理古地理图，Shapefile 配套文件作为一项"), kUi, "stage", "", "数据管理/数据导入", "", "project"},
    {"data.import.offlineBasemap", CommandKind::Command, "data", QT_TRANSLATE_NOOP("PaleoCommands", "导入离线底图"),
     QT_TRANSLATE_NOOP("PaleoCommands", "将 MBTiles、GeoTIFF 等底图及配套文件复制进工程，供地图离线显示"), kUi | SurfaceMenu, "stage", "",
     "数据管理/数据导入", "", "project"},
    {"data.import.paleomapFolder", CommandKind::Command, "data", QT_TRANSLATE_NOOP("PaleoCommands", "导入图件文件夹"), QT_TRANSLATE_NOOP("PaleoCommands", "递归导入并识别图件层位"),
     kUi, "stage", "", "数据管理/数据导入", "", "project"},
    {"data.import.files", CommandKind::Command, "data", QT_TRANSLATE_NOOP("PaleoCommands", "导入文件"), QT_TRANSLATE_NOOP("PaleoCommands", "选择一个或多个资料文件导入"), kUi, "stage",
     "", "数据管理/数据导入", "", "project"},
    {"data.import.folder", CommandKind::Command, "data", QT_TRANSLATE_NOOP("PaleoCommands", "导入工区文件夹"),
     QT_TRANSLATE_NOOP("PaleoCommands", "确认每个文件的类型后整目录入库"), kUi, "stage", "", "数据管理/数据导入", "", "project"},
    {"data.import.wells", CommandKind::Command, "data", QT_TRANSLATE_NOOP("PaleoCommands", "导入井数据"), QT_TRANSLATE_NOOP("PaleoCommands", "选择一个或多个井位/分层文件入库"), kUi,
     "stage", "", "数据管理/数据导入", "", "project"},
    {"data.import.wellLogs", CommandKind::Command, "data", QT_TRANSLATE_NOOP("PaleoCommands", "导入测井数据"),
     QT_TRANSLATE_NOOP("PaleoCommands", "选择 LAS、DLIS、LIS、XML 或 CSV/TXT/TSV 测井数据"), kUi, "stage", "", "数据管理/数据导入", "", "project"},
    {"data.import.seismic", CommandKind::Command, "data", QT_TRANSLATE_NOOP("PaleoCommands", "导入地震数据"), QT_TRANSLATE_NOOP("PaleoCommands", "选择 SEG-Y 等地震数据文件入库"), kUi,
     "stage", "", "数据管理/数据导入", "", "project"},
    {"data.import.boundary", CommandKind::Command, "data", QT_TRANSLATE_NOOP("PaleoCommands", "导入边界数据"), QT_TRANSLATE_NOOP("PaleoCommands", "选择边界矢量文件入库"), kUi, "stage",
     "", "数据管理/数据导入", "", "project"},
    {"data.list.unresolved", CommandKind::View, "data", QT_TRANSLATE_NOOP("PaleoCommands", "只看未决"), QT_TRANSLATE_NOOP("PaleoCommands", "只显示还有未决关联的数据"), kUi, "ui", "",
     "数据管理/数据列表"},
    {"data.list.refresh", CommandKind::Query, "data", QT_TRANSLATE_NOOP("PaleoCommands", "刷新列表"), QT_TRANSLATE_NOOP("PaleoCommands", "按数据目录重建列表"), kUi, "none", "",
     "数据管理/数据列表"},
    {"data.graph.open", CommandKind::View, "data", QT_TRANSLATE_NOOP("PaleoCommands", "资料图谱"), QT_TRANSLATE_NOOP("PaleoCommands", "查看文件资料关联、版本与成果来源"), kUi, "ui",
     "", "数据管理/数据列表"},
    {"data.preview.maximize", CommandKind::View, "data", QT_TRANSLATE_NOOP("PaleoCommands", "最大化预览"), QT_TRANSLATE_NOOP("PaleoCommands", "预览占满数据面（数据列表与右侧面板隐藏）"),
     kUi, "ui", "", "数据管理/数据预览"},

    // ---- ③ 井分析 / 单因素四步 ----
    {"predict.run", CommandKind::Workflow, "well", QT_TRANSLATE_NOOP("PaleoCommands", "运行预测"), QT_TRANSLATE_NOOP("PaleoCommands", "按当前参数运行井相或地震相预测"), kUi, "stage",
     "", "单井相分析/预测运行", "", "project"},
    {"singlefactor.acquire", CommandKind::Workflow, "well", QT_TRANSLATE_NOOP("PaleoCommands", "获取单因素"), QT_TRANSLATE_NOOP("PaleoCommands", "从井数据获取当前层段的单因素值"), kUi,
     "stage", "", "单井相分析/1. 获取单因素", "", "project"},
    {"singlefactor.oneclick", CommandKind::Workflow, "well", QT_TRANSLATE_NOOP("PaleoCommands", "生成等值图"), QT_TRANSLATE_NOOP("PaleoCommands", "获取单因素并生成等值图"), kUi, "stage",
     "", "单井相分析/3. 生成等值图", "", "project"},
    {"constraint.idw", CommandKind::Compute, "well", QT_TRANSLATE_NOOP("PaleoCommands", "生成单因素图"), QT_TRANSLATE_NOOP("PaleoCommands", "约束 IDW 插值生成单因素图"),
     kUi | SurfaceAgent, "stage", "constraint.idw", "单井相分析/3. 生成等值图", "", "project"},
    {"constraint.draw", CommandKind::Command, "well", QT_TRANSLATE_NOOP("PaleoCommands", "绘制约束"), QT_TRANSLATE_NOOP("PaleoCommands", "按参数面板当前形状绘制约束线"), kUi, "stage", "",
     "单井相分析/2. 约束线", "", "project"},
    {"constraint.breakLine", CommandKind::Command, "well", QT_TRANSLATE_NOOP("PaleoCommands", "间隔线"), QT_TRANSLATE_NOOP("PaleoCommands", "绘制间隔线"), kUi, "stage", "",
     "单井相分析/2. 约束线", "", "project"},
    {"constraint.direction", CommandKind::Command, "well", QT_TRANSLATE_NOOP("PaleoCommands", "方向线"), QT_TRANSLATE_NOOP("PaleoCommands", "绘制方向线"), kUi, "stage", "",
     "单井相分析/2. 约束线", "", "project"},
    {"constraint.importBarrier", CommandKind::Command, "well", QT_TRANSLATE_NOOP("PaleoCommands", "导入间隔线"), QT_TRANSLATE_NOOP("PaleoCommands", "从矢量文件导入间隔线"), kUi, "stage",
     "", "单井相分析/2. 约束线", "", "project"},
    {"constraint.importDirection", CommandKind::Command, "well", QT_TRANSLATE_NOOP("PaleoCommands", "导入方向线"), QT_TRANSLATE_NOOP("PaleoCommands", "从矢量文件导入方向线"), kUi,
     "stage", "", "单井相分析/2. 约束线", "", "project"},
    {"singlefactor.contours", CommandKind::Compute, "well", QT_TRANSLATE_NOOP("PaleoCommands", "提取等值线"), QT_TRANSLATE_NOOP("PaleoCommands", "从已发布的单因素栅格提取等值线"),
     kUi | SurfaceAgent, "stage", "singlefactor.contours", "单井相分析/4. 等值线", "", "project"},

    // ---- ⑤ 编图 / ⑥ 检查发布 ----
    {"compose.thicknessChain", CommandKind::Workflow, "compose", QT_TRANSLATE_NOOP("PaleoCommands", "生成等厚图"), QT_TRANSLATE_NOOP("PaleoCommands", "按当前层位运行编图厚度链"), kUi,
     "stage", "", "古地理编图/编图链", "", "project"},
    {"compose.fuse", CommandKind::Workflow, "compose", QT_TRANSLATE_NOOP("PaleoCommands", "合成编图"), QT_TRANSLATE_NOOP("PaleoCommands", "融合依据生成相图"), kUi, "stage", "",
     "古地理编图/编图链", "", "project"},
    {"compose.polygonize", CommandKind::Workflow, "compose", QT_TRANSLATE_NOOP("PaleoCommands", "转为相多边形"), QT_TRANSLATE_NOOP("PaleoCommands", "把相栅格转为相多边形"), kUi,
     "stage", "", "古地理编图/编图链", "", "project"},
    {"compose.export.pdf", CommandKind::Command, "compose", QT_TRANSLATE_NOOP("PaleoCommands", "导出地质图"), QT_TRANSLATE_NOOP("PaleoCommands", "把当前层位图导出为 PDF。"),
     kUi | SurfaceMenu, "export", "", "古地理编图/图件输出", "", "project"},
    {"compose.designer", CommandKind::View, "compose", QT_TRANSLATE_NOOP("PaleoCommands", "图件设计器"), QT_TRANSLATE_NOOP("PaleoCommands", "打开图件设计器"), kUi, "ui", "",
     "古地理编图/图件输出"},
    {"compose.mapbook", CommandKind::View, "compose", QT_TRANSLATE_NOOP("PaleoCommands", "地图册"), QT_TRANSLATE_NOOP("PaleoCommands", "打开地图册批量导出"), kUi, "ui", "",
     "古地理编图/图件输出"},
    {"compose.saveVersion", CommandKind::Command, "compose", QT_TRANSLATE_NOOP("PaleoCommands", "保存版本"), QT_TRANSLATE_NOOP("PaleoCommands", "把当前图件登记为新版本"), kUi, "stage",
     "", "古地理编图/图件输出", "", "project"},
    {"compose.publish", CommandKind::Command, "release", QT_TRANSLATE_NOOP("PaleoCommands", "发布"), QT_TRANSLATE_NOOP("PaleoCommands", "发布当前图件版本"), kUi, "publish", "",
     "古地理编图/图件输出", "", "project"},
    {"compose.editBasis", CommandKind::View, "compose", QT_TRANSLATE_NOOP("PaleoCommands", "编辑依据"), QT_TRANSLATE_NOOP("PaleoCommands", "打开编图依据编辑"), kUi, "ui", "",
     "古地理编图/编辑依据"},
    {"compose.references", CommandKind::View, "compose", QT_TRANSLATE_NOOP("PaleoCommands", "参考资料"), QT_TRANSLATE_NOOP("PaleoCommands", "打开编图参考资料面板"), kUi, "ui", "",
     "古地理编图/编辑依据"},
    {"validate.run", CommandKind::Workflow, "release", QT_TRANSLATE_NOOP("PaleoCommands", "运行验证"), QT_TRANSLATE_NOOP("PaleoCommands", "用井点残差检查当前成果"), kUi, "stage", "",
     "验证", "", "project"},
    {"validate.openSection", CommandKind::View, "release", QT_TRANSLATE_NOOP("PaleoCommands", "看这条剖面"), QT_TRANSLATE_NOOP("PaleoCommands", "打开验证问题所在的地震剖面"), kUi, "ui",
     "", "联动定位"},

    // ---- 矢量编辑（PaleoEditingToolbar 共享动作；QAction 本身即实现）----
    {"edit.select", CommandKind::Command, "edit", QT_TRANSLATE_NOOP("PaleoCommands", "选择要素"), "", kUi, "ui", "", "矢量编辑"},
    {"edit.addFeature", CommandKind::Command, "edit", QT_TRANSLATE_NOOP("PaleoCommands", "添加要素"), "", kUi, "stage", "", "矢量编辑"},
    {"edit.addPoint", CommandKind::Command, "edit", QT_TRANSLATE_NOOP("PaleoCommands", "添加点"), "", kUi, "stage", "", "矢量编辑"},
    {"edit.addLine", CommandKind::Command, "edit", QT_TRANSLATE_NOOP("PaleoCommands", "添加线"), "", kUi, "stage", "", "矢量编辑"},
    {"edit.addPolygon", CommandKind::Command, "edit", QT_TRANSLATE_NOOP("PaleoCommands", "添加面"), "", kUi, "stage", "", "矢量编辑"},
    {"edit.reshape", CommandKind::Command, "edit", QT_TRANSLATE_NOOP("PaleoCommands", "整形"), "", kUi, "stage", "", "矢量编辑"},
    {"edit.move", CommandKind::Command, "edit", QT_TRANSLATE_NOOP("PaleoCommands", "移动要素"), "", kUi, "stage", "", "矢量编辑"},
    {"edit.deleteFeatures", CommandKind::Command, "edit", QT_TRANSLATE_NOOP("PaleoCommands", "删除要素"), "", kUi, "stage", "", "矢量编辑"},
    {"edit.vertex", CommandKind::Command, "edit", QT_TRANSLATE_NOOP("PaleoCommands", "节点编辑"), "", kUi, "stage", "", "矢量编辑"},
    {"edit.faciesOperations", CommandKind::Command, "edit", QT_TRANSLATE_NOOP("PaleoCommands", "相面分区操作"), "", kUi, "stage", "", "相面分区"},
    {"edit.facies.split", CommandKind::Command, "edit", QT_TRANSLATE_NOOP("PaleoCommands", "分割所选相面"), "", kUi, "stage", "", "相面分区"},
    {"edit.facies.paint", CommandKind::Command, "edit", QT_TRANSLATE_NOOP("PaleoCommands", "新建 / 覆盖相面"), "", kUi, "stage", "", "相面分区"},
    {"edit.facies.merge", CommandKind::Command, "edit", QT_TRANSLATE_NOOP("PaleoCommands", "合并所选相面"), "", kUi, "stage", "", "相面分区"},
    {"edit.topological", CommandKind::Command, "edit", QT_TRANSLATE_NOOP("PaleoCommands", "拓扑编辑"), "", kUi, "ui", "", "编辑会话"},
    {"edit.crossLayerTopo", CommandKind::Command, "edit", QT_TRANSLATE_NOOP("PaleoCommands", "跨图层拓扑"), "", kUi, "ui", "", "编辑会话"},
    {"edit.mode", CommandKind::Command, "edit", QT_TRANSLATE_NOOP("PaleoCommands", "编辑模式"), "", kUi, "ui", "", "编辑会话"},
    {"edit.save", CommandKind::Command, "edit", QT_TRANSLATE_NOOP("PaleoCommands", "保存编辑"), "", kUi, "stage", "", "编辑会话"},
    {"edit.cancel", CommandKind::Command, "edit", QT_TRANSLATE_NOOP("PaleoCommands", "取消编辑"), "", kUi, "stage", "", "编辑会话"},
    {"edit.undo", CommandKind::Command, "edit", QT_TRANSLATE_NOOP("PaleoCommands", "撤销"), "", kUi | SurfaceMenu, "stage", "", "吸附与撤销"},
    {"edit.redo", CommandKind::Command, "edit", QT_TRANSLATE_NOOP("PaleoCommands", "重做"), "", kUi | SurfaceMenu, "stage", "", "吸附与撤销"},

    // ---- 只给 Agent / 批处理的 Callable 能力（无 ribbon 入口；U3 起由检查器生成表单）----
    {"depth.convert_batch", CommandKind::Compute, "well", QT_TRANSLATE_NOOP("PaleoCommands", "时深批量转换"), QT_TRANSLATE_NOOP("PaleoCommands", "按速度模型批量转换井样点深度"),
     SurfaceAgent, "stage", "depth.convert_batch"},
    {"seismic.attributes.compute", CommandKind::Compute, "seismic", QT_TRANSLATE_NOOP("PaleoCommands", "地震属性计算"),
     QT_TRANSLATE_NOOP("PaleoCommands", "RMS、包络与相干属性计算"), SurfaceAgent, "stage", "seismic.attributes.compute"},
};

QStringList splitList(const char *s) {
  const QString text = QString::fromLatin1(s);
  return text.isEmpty() ? QStringList() : text.split(QLatin1Char(','), Qt::SkipEmptyParts);
}

const QVector<CommandSpec> &table() {
  static const QVector<CommandSpec> specs = [] {
    QVector<CommandSpec> out;
    out.reserve(int(sizeof(kRows) / sizeof(kRows[0])));
    for (const Row &r : kRows) {
      CommandSpec s;
      s.commandId = QString::fromLatin1(r.id);
      s.kind = r.kind;
      s.stageId = QString::fromLatin1(r.stage);
      s.title = r.title;
      s.tooltip = r.tooltip;
      s.surfaces = r.surfaces;
      s.effect = QString::fromLatin1(r.effect);
      s.capabilityId = QString::fromLatin1(r.capability);
      s.ribbonGroup = QString::fromUtf8(r.group);
      s.shortcutId = QString::fromLatin1(r.shortcut);
      s.requiredContext = splitList(r.context);
      out << s;
    }
    return out;
  }();
  return specs;
}

} // namespace

QString commandKindName(CommandKind kind) {
  switch (kind) {
  case CommandKind::Query:
    return QStringLiteral("query");
  case CommandKind::Command:
    return QStringLiteral("command");
  case CommandKind::Compute:
    return QStringLiteral("compute");
  case CommandKind::Workflow:
    return QStringLiteral("workflow");
  case CommandKind::View:
    return QStringLiteral("view");
  }
  return QStringLiteral("command");
}

QVector<CommandSpec> CommandRegistry::all() { return table(); }

CommandSpec CommandRegistry::spec(const QString &commandId, int contractVersion) {
  for (const CommandSpec &s : table())
    if (s.commandId == commandId && s.contractVersion == contractVersion)
      return s;
  return {};
}

bool CommandRegistry::contains(const QString &commandId) {
  for (const CommandSpec &s : table())
    if (s.commandId == commandId)
      return true;
  return false;
}

QVector<CommandSpec> CommandRegistry::forStage(const QString &stageId) {
  QVector<CommandSpec> out;
  for (const CommandSpec &s : table())
    if (s.stageId == stageId)
      out << s;
  return out;
}

QVector<CommandSpec> CommandRegistry::forDocKind(const QString &docKind) {
  QVector<CommandSpec> out;
  for (const CommandSpec &s : table())
    if (s.docKinds.contains(docKind))
      out << s;
  return out;
}

QVector<CommandSpec> CommandRegistry::forSurface(CommandSurface surface) {
  QVector<CommandSpec> out;
  for (const CommandSpec &s : table())
    if (s.hasSurface(surface))
      out << s;
  return out;
}

QStringList CommandRegistry::agentCommandIds() {
  QStringList out;
  for (const CommandSpec &s : table())
    if (s.hasSurface(SurfaceAgent))
      out << s.commandId;
  return out;
}

QStringList CommandRegistry::knownStages() {
  return {QStringLiteral("project"), QStringLiteral("data"),    QStringLiteral("strat"),
          QStringLiteral("well"),    QStringLiteral("seismic"), QStringLiteral("compose"),
          QStringLiteral("release"), QStringLiteral("view"),    QStringLiteral("map"),
          QStringLiteral("edit")};
}

QStringList CommandRegistry::validate() {
  QStringList errors;
  QSet<QString> ids, shortcuts;
  const QStringList stages = knownStages();
  const QStringList effects = {QStringLiteral("none"), QStringLiteral("stage"), QStringLiteral("publish"),
                               QStringLiteral("export"), QStringLiteral("ui")};
  for (const CommandSpec &s : table()) {
    const QString id = s.commandId;
    if (id.isEmpty() || id.contains(QLatin1Char(' ')))
      errors << QStringLiteral("invalid command id '%1'").arg(id);
    if (ids.contains(id))
      errors << QStringLiteral("%1: duplicate command id").arg(id);
    ids.insert(id);
    if (!s.title || !*s.title)
      errors << QStringLiteral("%1: empty title").arg(id);
    if (!stages.contains(s.stageId))
      errors << QStringLiteral("%1: unknown stage '%2'").arg(id, s.stageId);
    if (!effects.contains(s.effect))
      errors << QStringLiteral("%1: unknown effect '%2'").arg(id, s.effect);
    if (s.surfaces == SurfaceNone)
      errors << QStringLiteral("%1: no entry surface").arg(id);
    if (!s.shortcutId.isEmpty()) {
      if (shortcuts.contains(s.shortcutId))
        errors << QStringLiteral("%1: shortcut '%2' bound twice").arg(id, s.shortcutId);
      shortcuts.insert(s.shortcutId);
      if (!s.hasSurface(SurfaceShortcut))
        errors << QStringLiteral("%1: shortcutId without Shortcut surface").arg(id);
    }
    if (s.kind == CommandKind::View && !s.capabilityId.isEmpty())
      errors << QStringLiteral("%1: view command must not reference a capability").arg(id);
    if (s.kind == CommandKind::Compute && s.capabilityId.isEmpty())
      errors << QStringLiteral("%1: compute command without capabilityId").arg(id);
    CapabilitySpec cap;
    if (!s.capabilityId.isEmpty()) {
      cap = CapabilityRegistry::spec(s.capabilityId, s.contractVersion);
      if (cap.capabilityId.isEmpty())
        errors << QStringLiteral("%1: capability '%2' v%3 not in CapabilityRegistry")
                      .arg(id, s.capabilityId)
                      .arg(s.contractVersion);
      else if (s.kind == CommandKind::Compute && s.commandId != s.capabilityId)
        errors << QStringLiteral("%1: compute command id must equal its capability id").arg(id);
    }
    if (s.hasSurface(SurfaceAgent)) {
      if (s.capabilityId.isEmpty())
        errors << QStringLiteral("%1: agent surface needs a capability").arg(id);
      else if (cap.exposure != CapabilityExposure::Callable)
        errors << QStringLiteral("%1: agent surface needs a Callable capability (is %2)")
                      .arg(id, capabilityExposureName(cap.exposure));
    }
  }
  return errors;
}

} // namespace paleo::command
