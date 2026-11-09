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

// 标题/说明是 PaleoCommands 上下文的源串；lupdate 经 QT_TRANSLATE_NOOP 收录。
#define T(s) QT_TRANSLATE_NOOP("PaleoCommands", s)
const Row kRows[] = {
    // ---- 工程（文件菜单 / 快速访问 / 起始页；U4 Backstage 前的过渡位置）----
    {"project.new", CommandKind::Command, "project", T("新建工程"), T("新建空白工程并设置资料来源"), kMenu},
    {"project.open", CommandKind::Command, "project", T("打开工程"), T("打开 .paleo / QGIS 工程"), kMenu},
    {"project.newFromFolder", CommandKind::Command, "project", T("从工区文件夹新建"),
     T("选择工区目录：已有工程直接打开，否则在目录内建工程并导入"), kMenu},
    {"project.save", CommandKind::Command, "project", T("保存工程"), T("保存当前工程"), kMenu | SurfaceShortcut,
     "stage", "", "", "main.project.save", "project"},
    {"project.sources", CommandKind::Command, "project", T("工程资料设置"), T("补充或调整当前工程的资料来源"), kMenu,
     "stage", "", "", "", "project"},
    {"project.mapSettings", CommandKind::Command, "project", T("工程坐标与底图"), T("设置工程坐标参考与底图"), kMenu,
     "stage", "", "", "", "project"},
    {"project.coordinateTools", CommandKind::View, "project", T("坐标定位与转换"),
     T("QGIS 坐标定位、格式转换与基准操作"), kMenu},
    {"project.basemap.online", CommandKind::Command, "project", T("加入网络底图预设"),
     T("把选定的在线底图加入图层树（参数 presetId）"), SurfaceMenu, "stage", "", "", "", "project"},
    {"project.home", CommandKind::View, "project", T("起始页"), T("回到起始页"), kMenu},
    {"app.quit", CommandKind::Command, "project", T("退出"), T("退出应用"), kMenu},

    // ---- 页签切换（Ctrl+1..N）----
    {"view.stage.data", CommandKind::View, "view", T("切到「数据管理」页签"), "", SurfaceShortcut | SurfaceLocator,
     "ui", "", "", "main.page.data"},
    {"view.stage.correlation", CommandKind::View, "view", T("切到「地层对比」页签"), "",
     SurfaceShortcut | SurfaceLocator, "ui", "", "", "main.page.correlation"},
    {"view.stage.predict", CommandKind::View, "view", T("切到「单井相分析」页签"), "",
     SurfaceShortcut | SurfaceLocator, "ui", "", "", "main.page.predict"},
    {"view.stage.seismic", CommandKind::View, "view", T("切到「地震相分析」页签"), "",
     SurfaceShortcut | SurfaceLocator, "ui", "", "", "main.page.seismic"},
    {"view.stage.compose", CommandKind::View, "view", T("切到「古地理编图」页签"), "",
     SurfaceShortcut | SurfaceLocator, "ui", "", "", "main.page.compose"},

    // ---- 打开任务视图（各页签「视图 / 分析任务」组）----
    {"view.open.data", CommandKind::View, "data", T("打开数据视图"), T("打开当前工程的数据列表与资料预览。"), kUi,
     "ui", "", "数据管理/视图"},
    {"view.open.correlation", CommandKind::View, "strat", T("打开地层对比"),
     T("打开原生连井曲线与分层结果，使用当前工区的数据服务。"), kUi, "ui", "", "地层对比/视图"},
    {"view.open.wellPrediction", CommandKind::View, "well", T("井预测"),
     T("按当前地层预测井的相分类并保存井预测结果。"), kUi, "ui", "", "单井相分析/分析任务"},
    {"view.open.singleFactor", CommandKind::View, "well", T("编制单因素图"),
     T("按当前地层选择单因素方法，编制对应图件。"), kUi, "ui", "", "单井相分析/分析任务"},
    {"view.open.seismicFacies", CommandKind::View, "seismic", T("地震相预测"), T("预测地震相并生成面状结果。"),
     kUi, "ui", "", "地震相分析/分析任务"},
    {"view.open.validate", CommandKind::View, "release", T("送交验证"), T("切到验证页，用井点残差检查当前成果"),
     kUi, "ui", "", "结果"},

    // ---- 窗格 / 视图 ----
    {"view.params", CommandKind::View, "view", T("参数面板"), T("显示/隐藏右侧参数面板"), kUi},
    {"view.attributeTable", CommandKind::View, "view", T("属性表"), T("在底部面板打开图层属性表"), kUi},
    {"view.releases", CommandKind::View, "release", T("发布记录"), T("在底部面板查看已发布的版本"), kUi},
    {"view.wellSection", CommandKind::View, "strat", T("连井剖面"), T("按地层连井：多井对比、分层连线与井间地震"),
     kUi},
    {"view.sectionWorkbench", CommandKind::View, "seismic", T("井震剖面"), T("打开井震剖面工作台"), kUi},
    {"view.seismic3d", CommandKind::View, "seismic", T("三维视口"), T("打开三维地震体立体视口"), kUi},
    {"view.seismic2d", CommandKind::View, "seismic", T("地震剖面"), T("打开二维地震与井震标定剖面"), kUi},
    {"view.errorHistory", CommandKind::View, "view", T("错误历史"), T("打开错误历史面板"),
     SurfaceContextMenu | SurfaceLocator},
    {"view.theme.dark", CommandKind::View, "view", T("深色模式"), T("切换浅色/深色主题"),
     SurfaceContextMenu | SurfaceLocator},
    {"view.density.compact", CommandKind::View, "view", T("紧凑密度"), T("切换舒适/紧凑界面密度"),
     SurfaceContextMenu | SurfaceLocator},
    {"view.more.constraint", CommandKind::View, "well", T("单因素图更多工具"),
     T("连井剖面、井震剖面、地图导航、相图显示、捕捉与撤销"), SurfaceRibbon},

    // ---- 地图导航（QGIS 原生工具）----
    {"map.pan", CommandKind::View, "map", T("平移"), T("拖动平移地图"), kUi},
    {"map.zoomIn", CommandKind::View, "map", T("放大"), T("点击或框选放大"), kUi},
    {"map.zoomOut", CommandKind::View, "map", T("缩小"), T("点击或框选缩小"), kUi},
    {"map.zoomFull", CommandKind::View, "map", T("全图"), T("缩放到全部图层"), kUi},
    {"map.identify", CommandKind::View, "map", T("识别"), T("指针落到要素上时显示悬浮信息，多个命中可切换"), kUi},

    // ---- ① 资料 ----
    {"data.import.paleomap", CommandKind::Command, "data", T("导入现有古地理图"),
     T("按层位管理古地理图，Shapefile 配套文件作为一项"), kUi, "stage", "", "数据管理/数据导入", "", "project"},
    {"data.import.offlineBasemap", CommandKind::Command, "data", T("导入离线底图"),
     T("将 MBTiles、GeoTIFF 等底图及配套文件复制进工程，供地图离线显示"), kUi | SurfaceMenu, "stage", "",
     "数据管理/数据导入", "", "project"},
    {"data.import.paleomapFolder", CommandKind::Command, "data", T("导入图件文件夹"), T("递归导入并识别图件层位"),
     kUi, "stage", "", "数据管理/数据导入", "", "project"},
    {"data.import.files", CommandKind::Command, "data", T("导入文件"), T("选择一个或多个资料文件导入"), kUi, "stage",
     "", "数据管理/数据导入", "", "project"},
    {"data.import.folder", CommandKind::Command, "data", T("导入工区文件夹"),
     T("确认每个文件的类型后整目录入库"), kUi, "stage", "", "数据管理/数据导入", "", "project"},
    {"data.import.wells", CommandKind::Command, "data", T("导入井数据"), T("选择一个或多个井位/分层文件入库"), kUi,
     "stage", "", "数据管理/数据导入", "", "project"},
    {"data.import.wellLogs", CommandKind::Command, "data", T("导入测井数据"),
     T("选择 LAS、DLIS、LIS、XML 或 CSV/TXT/TSV 测井数据"), kUi, "stage", "", "数据管理/数据导入", "", "project"},
    {"data.import.seismic", CommandKind::Command, "data", T("导入地震数据"), T("选择 SEG-Y 等地震数据文件入库"), kUi,
     "stage", "", "数据管理/数据导入", "", "project"},
    {"data.import.boundary", CommandKind::Command, "data", T("导入边界数据"), T("选择边界矢量文件入库"), kUi, "stage",
     "", "数据管理/数据导入", "", "project"},
    {"data.list.unresolved", CommandKind::View, "data", T("只看未决"), T("只显示还有未决关联的数据"), kUi, "ui", "",
     "数据管理/数据列表"},
    {"data.list.refresh", CommandKind::Query, "data", T("刷新列表"), T("按数据目录重建列表"), kUi, "none", "",
     "数据管理/数据列表"},
    {"data.graph.open", CommandKind::View, "data", T("资料图谱"), T("查看文件资料关联、版本与成果来源"), kUi, "ui",
     "", "数据管理/数据列表"},
    {"data.preview.maximize", CommandKind::View, "data", T("最大化预览"), T("预览占满数据面（数据列表与右侧面板隐藏）"),
     kUi, "ui", "", "数据管理/数据预览"},

    // ---- ③ 井分析 / 单因素四步 ----
    {"predict.run", CommandKind::Workflow, "well", T("运行预测"), T("按当前参数运行井相或地震相预测"), kUi, "stage",
     "", "单井相分析/预测运行", "", "project"},
    {"singlefactor.acquire", CommandKind::Workflow, "well", T("获取单因素"), T("从井数据获取当前层段的单因素值"), kUi,
     "stage", "", "单井相分析/1. 获取单因素", "", "project"},
    {"singlefactor.oneclick", CommandKind::Workflow, "well", T("生成等值图"), T("获取单因素并生成等值图"), kUi, "stage",
     "", "单井相分析/3. 生成等值图", "", "project"},
    {"constraint.idw", CommandKind::Compute, "well", T("生成单因素图"), T("约束 IDW 插值生成单因素图"),
     kUi | SurfaceAgent, "stage", "constraint.idw", "单井相分析/3. 生成等值图", "", "project"},
    {"constraint.draw", CommandKind::Command, "well", T("绘制约束"), T("按参数面板当前形状绘制约束线"), kUi, "stage", "",
     "单井相分析/2. 约束线", "", "project"},
    {"constraint.breakLine", CommandKind::Command, "well", T("间隔线"), T("绘制间隔线"), kUi, "stage", "",
     "单井相分析/2. 约束线", "", "project"},
    {"constraint.direction", CommandKind::Command, "well", T("方向线"), T("绘制方向线"), kUi, "stage", "",
     "单井相分析/2. 约束线", "", "project"},
    {"constraint.importBarrier", CommandKind::Command, "well", T("导入间隔线"), T("从矢量文件导入间隔线"), kUi, "stage",
     "", "单井相分析/2. 约束线", "", "project"},
    {"constraint.importDirection", CommandKind::Command, "well", T("导入方向线"), T("从矢量文件导入方向线"), kUi,
     "stage", "", "单井相分析/2. 约束线", "", "project"},
    {"singlefactor.contours", CommandKind::Compute, "well", T("提取等值线"), T("从已发布的单因素栅格提取等值线"),
     kUi | SurfaceAgent, "stage", "singlefactor.contours", "单井相分析/4. 等值线", "", "project"},

    // ---- ⑤ 编图 / ⑥ 检查发布 ----
    {"compose.thicknessChain", CommandKind::Workflow, "compose", T("生成等厚图"), T("按当前层位运行编图厚度链"), kUi,
     "stage", "", "古地理编图/编图链", "", "project"},
    {"compose.fuse", CommandKind::Workflow, "compose", T("合成编图"), T("融合依据生成相图"), kUi, "stage", "",
     "古地理编图/编图链", "", "project"},
    {"compose.polygonize", CommandKind::Workflow, "compose", T("转为相多边形"), T("把相栅格转为相多边形"), kUi,
     "stage", "", "古地理编图/编图链", "", "project"},
    {"compose.export.pdf", CommandKind::Command, "compose", T("导出地质图"), T("把当前层位图导出为 PDF。"),
     kUi | SurfaceMenu, "export", "", "古地理编图/图件输出", "", "project"},
    {"compose.designer", CommandKind::View, "compose", T("图件设计器"), T("打开图件设计器"), kUi, "ui", "",
     "古地理编图/图件输出"},
    {"compose.mapbook", CommandKind::View, "compose", T("地图册"), T("打开地图册批量导出"), kUi, "ui", "",
     "古地理编图/图件输出"},
    {"compose.saveVersion", CommandKind::Command, "compose", T("保存版本"), T("把当前图件登记为新版本"), kUi, "stage",
     "", "古地理编图/图件输出", "", "project"},
    {"compose.publish", CommandKind::Command, "release", T("发布"), T("发布当前图件版本"), kUi, "publish", "",
     "古地理编图/图件输出", "", "project"},
    {"compose.editBasis", CommandKind::View, "compose", T("编辑依据"), T("打开编图依据编辑"), kUi, "ui", "",
     "古地理编图/编辑依据"},
    {"compose.references", CommandKind::View, "compose", T("参考资料"), T("打开编图参考资料面板"), kUi, "ui", "",
     "古地理编图/编辑依据"},
    {"validate.run", CommandKind::Workflow, "release", T("运行验证"), T("用井点残差检查当前成果"), kUi, "stage", "",
     "验证", "", "project"},
    {"validate.openSection", CommandKind::View, "release", T("看这条剖面"), T("打开验证问题所在的地震剖面"), kUi, "ui",
     "", "联动定位"},

    // ---- 矢量编辑（PaleoEditingToolbar 共享动作；QAction 本身即实现）----
    {"edit.select", CommandKind::Command, "edit", T("选择要素"), "", kUi, "ui", "", "矢量编辑"},
    {"edit.addFeature", CommandKind::Command, "edit", T("添加要素"), "", kUi, "stage", "", "矢量编辑"},
    {"edit.addPoint", CommandKind::Command, "edit", T("添加点"), "", kUi, "stage", "", "矢量编辑"},
    {"edit.addLine", CommandKind::Command, "edit", T("添加线"), "", kUi, "stage", "", "矢量编辑"},
    {"edit.addPolygon", CommandKind::Command, "edit", T("添加面"), "", kUi, "stage", "", "矢量编辑"},
    {"edit.reshape", CommandKind::Command, "edit", T("整形"), "", kUi, "stage", "", "矢量编辑"},
    {"edit.move", CommandKind::Command, "edit", T("移动要素"), "", kUi, "stage", "", "矢量编辑"},
    {"edit.deleteFeatures", CommandKind::Command, "edit", T("删除要素"), "", kUi, "stage", "", "矢量编辑"},
    {"edit.vertex", CommandKind::Command, "edit", T("节点编辑"), "", kUi, "stage", "", "矢量编辑"},
    {"edit.faciesOperations", CommandKind::Command, "edit", T("相面分区操作"), "", kUi, "stage", "", "相面分区"},
    {"edit.facies.split", CommandKind::Command, "edit", T("分割所选相面"), "", kUi, "stage", "", "相面分区"},
    {"edit.facies.paint", CommandKind::Command, "edit", T("新建 / 覆盖相面"), "", kUi, "stage", "", "相面分区"},
    {"edit.facies.merge", CommandKind::Command, "edit", T("合并所选相面"), "", kUi, "stage", "", "相面分区"},
    {"edit.topological", CommandKind::Command, "edit", T("拓扑编辑"), "", kUi, "ui", "", "编辑会话"},
    {"edit.crossLayerTopo", CommandKind::Command, "edit", T("跨图层拓扑"), "", kUi, "ui", "", "编辑会话"},
    {"edit.mode", CommandKind::Command, "edit", T("编辑模式"), "", kUi, "ui", "", "编辑会话"},
    {"edit.save", CommandKind::Command, "edit", T("保存编辑"), "", kUi, "stage", "", "编辑会话"},
    {"edit.cancel", CommandKind::Command, "edit", T("取消编辑"), "", kUi, "stage", "", "编辑会话"},
    {"edit.undo", CommandKind::Command, "edit", T("撤销"), "", kUi | SurfaceMenu, "stage", "", "吸附与撤销"},
    {"edit.redo", CommandKind::Command, "edit", T("重做"), "", kUi | SurfaceMenu, "stage", "", "吸附与撤销"},

    // ---- 只给 Agent / 批处理的 Callable 能力（无 ribbon 入口；U3 起由检查器生成表单）----
    {"depth.convert_batch", CommandKind::Compute, "well", T("时深批量转换"), T("按速度模型批量转换井样点深度"),
     SurfaceAgent, "stage", "depth.convert_batch"},
    {"seismic.attributes.compute", CommandKind::Compute, "seismic", T("地震属性计算"),
     T("RMS、包络与相干属性计算"), SurfaceAgent, "stage", "seismic.attributes.compute"},
};
#undef T

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
