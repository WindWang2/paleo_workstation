// 层：功能
#include "navigationtools.h"
#include "domaintools.h"

#include <QJsonArray>

// ai/chat/navigationtools.cpp — I4：六个语义导航原子的工具描述表。
//
// 描述与参数就是真实执行面的轮廓（workflow/agentnavigation 的编排 +
// qgis/agentmapnavigation 与 ui/agentnavigationadapter 的原生适配）；
// 没有一项是本表新发明的执行能力。效果恒 Read：选择/定位/预览都是临时
// 视图态，不落业务库、不改 Presentation 写门。

namespace paleo::agent::navigation {

bool isNavigationTool(const QString &name) {
  return name == QLatin1String(kStateTool) || name == QLatin1String(kSelectWellsTool) ||
         name == QLatin1String(kClearWellSelectionTool) || name == QLatin1String(kMapLocateTool) ||
         name == QLatin1String(kOpenWellViewTool) || name == QLatin1String(kOpenSeismicLineTool);
}

bool isSupportedWellViewAssetType(const QString &assetType) {
  // 基线已存在固定版本/井过滤路径的三类井资料（datapreviewtabs 的
  // wellFilterable 分支）；well_log 等按显式表拒绝，待后续核实后扩表。
  return assetType == QLatin1String("well_head") ||
         assetType == QLatin1String("well_stratification") ||
         assetType == QLatin1String("time_depth");
}

} // namespace paleo::agent::navigation

void appendAgentNavigationTools(QVector<AiToolSpec> &specs) {
  using namespace paleo::agent::navigation;

  AiToolSpec state;
  state.name = QLatin1String(kStateTool);
  state.title = QObject::tr("导航状态查询");
  state.description = QObject::tr(
      "只读查询当前宿主的导航事实：文档绑定、当前业务选井（含来源）、主画布"
      "实际范围与 CRS、已打开原生预览目标及其加载/错误态。不创建视图、不全扫"
      "源数据；未创建的画布/已关闭的页面如实报不可用，不从页面标题猜身份。");
  state.requiresLocalOrt = false;
  state.effects = paleo::host::Read;
  state.resultSchemaJson = QStringLiteral(
      R"({"type":"object","required":["ok","contract"],"properties":{"ok":{"type":"boolean"},"contract":{"const":"paleo.navigation.v1"},"code":{"type":"string"},"document":{"type":"object"},"selection":{"type":"object"},"map":{"type":"object"},"previews":{"type":"array","items":{"type":"object"}}}})");

  AiToolSpec selectWells;
  selectWells.name = QLatin1String(kSelectWellsTool);
  selectWells.title = QObject::tr("选择井");
  selectWells.description = QObject::tr(
      "按稳定井业务 ID 精确替换当前井选择（replace 语义）：先在当前工程目录"
      "核实全部 ID 都是井实体才更新，未知/错类型 ID 整批拒绝且不留部分选中。"
      "重复 ID 去重且保持首次出现顺序。只影响井选择域，不动地震线选择、解释"
      "目标或编辑状态。不收名称匹配或 QGIS FID。");
  selectWells.requiresLocalOrt = false;
  selectWells.effects = paleo::host::Read;
  selectWells.parameters = {
      {QStringLiteral("well_ids"), QStringLiteral("array"),
       QObject::tr("井业务 ID 列表（1–%1 个，每项 ≤%2 字符）；语义为整批替换")
           .arg(kMaxWellIds)
           .arg(kMaxIdLength),
       true},
  };
  selectWells.resultSchemaJson = QStringLiteral(
      R"({"type":"object","required":["ok","contract"],"properties":{"ok":{"type":"boolean"},"contract":{"const":"paleo.navigation.v1"},"code":{"type":"string"},"requested_count":{"type":"integer"},"selected_count":{"type":"integer"},"selected_wells":{"type":"array","items":{"type":"string"}},"mapped_feature_count":{"type":"integer"},"unmapped_wells":{"type":"array","items":{"type":"string"}}}})");

  AiToolSpec clearWells;
  clearWells.name = QLatin1String(kClearWellSelectionTool);
  clearWells.title = QObject::tr("清除井选择");
  clearWells.description = QObject::tr(
      "只清除井选择域（本工具定义的业务选井），不删除数据、不清解释/编辑/"
      "地震线选择。可携带 navigation_state 返回的选择修订 token：token 与"
      "当前选择不一致时报 stale_selection 不动作；当前选择含非井目标时报"
      "foreign_selection 不动作。");
  clearWells.requiresLocalOrt = false;
  clearWells.effects = paleo::host::Read;
  clearWells.parameters = {
      {QStringLiteral("selection_token"), QStringLiteral("string"),
       QObject::tr("可选：navigation_state 返回的选择修订 token（≤%1 字符）；"
                   "缺省时按当前选择内容判定")
           .arg(kMaxTokenLength),
       false},
  };
  clearWells.resultSchemaJson = QStringLiteral(
      R"({"type":"object","required":["ok","contract"],"properties":{"ok":{"type":"boolean"},"contract":{"const":"paleo.navigation.v1"},"code":{"type":"string"},"status":{"type":"string","enum":["cleared","no_change","stale_selection","foreign_selection"]}}})");

  AiToolSpec mapLocate;
  mapLocate.name = QLatin1String(kMapLocateTool);
  mapLocate.title = QObject::tr("地图定位");
  mapLocate.description = QObject::tr(
      "把主画布视口移到业务目标：井 ID 列表的包络或受管图层。坐标/范围/源"
      "CRS 由宿主解析并经工程转换严格校验——非有限坐标、未知 CRS、转换失败"
      "或空范围时拒绝且旧视口不动，不回落原始坐标。center 保留当前比例尺，"
      "fit 匹配目标包络。不改变图层可见性/样式/层序；目标隐藏时如实报"
      "located_not_visible。");
  mapLocate.requiresLocalOrt = false;
  mapLocate.effects = paleo::host::Read;
  mapLocate.parameters = {
      {QStringLiteral("well_ids"), QStringLiteral("array"),
       QObject::tr("井业务 ID 列表（1–%1 个）；与 layer_id 互斥").arg(kMaxWellIds),
       false},
      {QStringLiteral("layer_id"), QStringLiteral("string"),
       QObject::tr("受管图层 ID（manifest id）；与 well_ids 互斥"),
       false},
      {QStringLiteral("mode"), QStringLiteral("string"),
       QObject::tr("center=保持当前比例尺居中；fit=适配目标包络"),
       true,
       {QStringLiteral("center"), QStringLiteral("fit")}},
  };
  mapLocate.resultSchemaJson = QStringLiteral(
      R"({"type":"object","required":["ok","contract"],"properties":{"ok":{"type":"boolean"},"contract":{"const":"paleo.navigation.v1"},"code":{"type":"string"},"mode":{"type":"string"},"target_count":{"type":"integer"},"resolved_count":{"type":"integer"},"extent":{"type":"array","items":{"type":"number"}},"crs":{"type":"string"},"target_visible":{"type":"boolean"}}})");

  AiToolSpec openWellView;
  openWellView.name = QLatin1String(kOpenWellViewTool);
  openWellView.title = QObject::tr("打开井资料");
  openWellView.description = QObject::tr(
      "在原生数据页预览中打开明确版本的井资料并过滤到指定井：well_id+asset_id+"
      "version_id 须在当前工程内核实（版本归属资产、井关联已决、类型受支持："
      "well_head/well_stratification/time_depth）。结果区分页面创建/聚焦/正文"
      "显示三个事实；外链 SHA 失配按只读策略如实失败，不写目录。不打开"
      "Office、外部网址或任意资产。");
  openWellView.requiresLocalOrt = false;
  openWellView.effects = paleo::host::Read;
  openWellView.parameters = {
      {QStringLiteral("well_id"), QStringLiteral("string"),
       QObject::tr("井业务 ID（须是该资产已决关联的井）"), true},
      {QStringLiteral("asset_id"), QStringLiteral("string"),
       QObject::tr("资产业务 ID"), true},
      {QStringLiteral("version_id"), QStringLiteral("string"),
       QObject::tr("固定版本 ID（不回落 currentVersion）"), true},
  };
  openWellView.resultSchemaJson = QStringLiteral(
      R"({"type":"object","required":["ok","contract"],"properties":{"ok":{"type":"boolean"},"contract":{"const":"paleo.navigation.v1"},"code":{"type":"string"},"status":{"type":"string"},"asset_id":{"type":"string"},"version_id":{"type":"string"},"well_id":{"type":"string"},"page_open":{"type":"boolean"},"page_focused":{"type":"boolean"},"detail":{"type":"string"}}})");

  AiToolSpec openSeismic;
  openSeismic.name = QLatin1String(kOpenSeismicLineTool);
  openSeismic.title = QObject::tr("打开地震测线");
  openSeismic.description = QObject::tr(
      "在原生二维测线页面显示明确版本地震资料的指定 inline/crossline：真实"
      "存在的线才解码呈现，越界/不存在的线拒绝（不取最近一条、不吃 SpinBox"
      "钳位）；结果区分振幅已显示与层位仍在读取的部分就绪态。不选层、不改"
      "可见性、不做时间游标或 3D 视角。");
  openSeismic.requiresLocalOrt = false;
  openSeismic.effects = paleo::host::Read;
  openSeismic.parameters = {
      {QStringLiteral("asset_id"), QStringLiteral("string"),
       QObject::tr("地震资产业务 ID"), true},
      {QStringLiteral("version_id"), QStringLiteral("string"),
       QObject::tr("固定版本 ID（不回落 currentVersion）"), true},
      {QStringLiteral("axis"), QStringLiteral("string"),
       QObject::tr("测线轴"), true,
       {QStringLiteral("inline"), QStringLiteral("crossline")}},
      {QStringLiteral("line_number"), QStringLiteral("integer"),
       QObject::tr("测线号（survey 几何范围内的整数；真实存在性由解码结果判定）"),
       true},
  };
  openSeismic.resultSchemaJson = QStringLiteral(
      R"({"type":"object","required":["ok","contract"],"properties":{"ok":{"type":"boolean"},"contract":{"const":"paleo.navigation.v1"},"code":{"type":"string"},"status":{"type":"string"},"asset_id":{"type":"string"},"version_id":{"type":"string"},"axis":{"type":"string"},"line_number":{"type":"integer"},"detail":{"type":"string"}}})");

  specs.append(state);
  specs.append(selectWells);
  specs.append(clearWells);
  specs.append(mapLocate);
  specs.append(openWellView);
  specs.append(openSeismic);
}
