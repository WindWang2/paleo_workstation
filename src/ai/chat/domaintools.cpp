// 层：功能
#include "domaintools.h"

#include <QJsonArray>

#include "../../catalog/catalogroles.h"

// ai/chat — 领域工具表实现（方向51）。
//
// 工具对应 src/ai 的推理能力与 catalog 只读查询面：
//   1. paleo.tile_classification        → tileinference.h + PaleoOnnxService
//   2. paleo.horizon_tracking_suggestion → horizonsuggest.h:suggestHorizonTracking
//   3. paleo.well_facies_prediction      → wellfaciesservice.h:WellFaciesService
//   4. paleo.query_project               → catalog/datacatalog.h 只读查询（方向77）
//   5. paleo.asset_lineage               → services/derivationgraph.h 血缘闭包（方向77）
// 描述的名字/入参/出参就是它们的真实签名轮廓；没有一项是本方向新发明的能力。

namespace {
bool jsonTypeMatches(const QString &expected, const QJsonValue &value) {
  if (expected == QLatin1String("string"))
    return value.isString();
  if (expected == QLatin1String("number"))
    return value.isDouble();
  if (expected == QLatin1String("integer"))
    return value.isDouble() || value.isString(); // 允许 "12" 这种文本整数
  if (expected == QLatin1String("boolean"))
    return value.isBool();
  if (expected == QLatin1String("object"))
    return value.isObject();
  if (expected == QLatin1String("array"))
    return value.isArray();
  return true; // 未知类型：不挡（表由本目录维护，类型词有限）
}
} // namespace

QJsonObject AiToolParamSpec::toJsonSchema() const {
  QJsonObject schema;
  schema.insert(QStringLiteral("type"), type);
  if (!description.isEmpty())
    schema.insert(QStringLiteral("description"), description);
  if (!enumValues.isEmpty()) {
    QJsonArray allowed;
    for (const QString &value : enumValues)
      allowed.append(value);
    schema.insert(QStringLiteral("enum"), allowed);
  }
  return schema;
}

QJsonObject AiToolSpec::toJsonSchema() const {
  QJsonObject schema;
  schema.insert(QStringLiteral("type"), QStringLiteral("object"));
  QJsonObject properties;
  QJsonArray required;
  for (const AiToolParamSpec &parameter : parameters) {
    properties.insert(parameter.name, parameter.toJsonSchema());
    if (parameter.required)
      required.append(parameter.name);
  }
  schema.insert(QStringLiteral("properties"), properties);
  if (!required.isEmpty())
    schema.insert(QStringLiteral("required"), required);
  schema.insert(QStringLiteral("additionalProperties"), false);
  return schema;
}

QJsonObject AiToolSpec::toChatFunction() const {
  QJsonObject function;
  function.insert(QStringLiteral("name"), name);
  function.insert(QStringLiteral("description"), description);
  function.insert(QStringLiteral("parameters"), toJsonSchema());
  return function;
}

QString AiToolSpec::validateParameters(const QJsonObject &arguments) const {
  for (const AiToolParamSpec &parameter : parameters) {
    const bool present = arguments.contains(parameter.name);
    if (parameter.required && !present)
      return QObject::tr("缺少必填参数 %1").arg(parameter.name);
    if (!present)
      continue;
    if (!jsonTypeMatches(parameter.type, arguments.value(parameter.name)))
      return QObject::tr("参数 %1 类型应为 %2").arg(parameter.name,
                                                    parameter.type);
    // 封闭词表（方向77）：词表外取值在本地就拒，省一轮工具往返。
    if (parameter.type == QLatin1String("string") &&
        !parameter.enumValues.isEmpty() &&
        !parameter.enumValues.contains(arguments.value(parameter.name).toString()))
      return QObject::tr("参数 %1 取值须为：%2")
        .arg(parameter.name, parameter.enumValues.join(QLatin1String("、")));
  }
  for (auto it = arguments.begin(); it != arguments.end(); ++it) {
    bool known = false;
    for (const AiToolParamSpec &parameter : parameters)
      if (parameter.name == it.key()) {
        known = true;
        break;
      }
    if (!known)
      return QObject::tr("未知参数 %1").arg(it.key());
  }
  return QString();
}

QVector<AiToolSpec> builtinAiToolSpecs() {
  // 1) tile 分类：existing capability (tileinference + PaleoOnnxService)
  AiToolSpec tile;
  tile.name = QStringLiteral("paleo.tile_classification");
  tile.title = QObject::tr("地震相 tile 分类");
  tile.description = QObject::tr(
    "对给定网格做逐像素/tile 相分类：按 tile 切片读域 → 本地 ONNX 模型推理 → "
    "缝合出等尺寸与请求网格一致的相码图（255 = 无数据）与置信度掩膜。");
  tile.requiresLocalOrt = true;
  tile.parameters = {
    {QStringLiteral("model"), QStringLiteral("string"),
     QObject::tr("模型名（PaleoOnnxService 命名，位于工程 models/ 目录）"), true},
    {QStringLiteral("grid_columns"), QStringLiteral("integer"),
     QObject::tr("整图网格列数"), true},
    {QStringLiteral("grid_rows"), QStringLiteral("integer"),
     QObject::tr("整图网格行数"), true},
    {QStringLiteral("tile_rows"), QStringLiteral("integer"),
     QObject::tr("tile 行数（缺省 128）"), false},
    {QStringLiteral("tile_columns"), QStringLiteral("integer"),
     QObject::tr("tile 列数（缺省 128）"), false},
    {QStringLiteral("halo"), QStringLiteral("integer"),
     QObject::tr("tile 四周冗余像素（缺省 8，用于消缝合接缝）"), false},
  };
  tile.resultSchemaJson = QStringLiteral(
    R"({"type":"object","properties":{"rows":{"type":"integer"},"columns":{"type":"integer"},"classes":{"type":"integer"},"argmax":{"type":"array","items":{"type":"integer"}},"confidence":{"type":"array","items":{"type":"number"}}}})");

  // 2) 层位自动追踪建议
  AiToolSpec horizon;
  horizon.name = QStringLiteral("paleo.horizon_tracking_suggestion");
  horizon.title = QObject::tr("层位追踪建议");
  horizon.description = QObject::tr(
    "从种子点（井位/人工拾取）出发逐道扩张给出层位拾取建议与置信度；"
    "输出的是建议，须逐条接受/否决后才可能落库——不会自动写解释。");
  horizon.requiresLocalOrt = true;
  horizon.parameters = {
    {QStringLiteral("model"), QStringLiteral("string"),
     QObject::tr("道窗 scorer 模型名"), true},
    {QStringLiteral("window_samples"), QStringLiteral("integer"),
     QObject::tr("每道的取窗样本数"), true},
    {QStringLiteral("radius"), QStringLiteral("integer"),
     QObject::tr("种子向 inline/xline 双向扩张的道数半径"), true},
    {QStringLiteral("seeds"), QStringLiteral("array"),
     QObject::tr("种子点列表，元素形如 {\"inline\":0,\"xline\":0,\"sample_index\":0}"),
     true},
  };
  horizon.resultSchemaJson = QStringLiteral(
    R"({"type":"object","properties":{"suggestions":{"type":"array","items":{"type":"object","properties":{"inline":{"type":"integer"},"xline":{"type":"integer"},"sample_index":{"type":"integer"},"score":{"type":"number"},"confidence":{"type":"number"},"is_seed":{"type":"boolean"}}}}}})");

  // 3) 测井相预测（HTTP 远端，需用户配置的端点 + 密钥）
  AiToolSpec facies;
  facies.name = QStringLiteral("paleo.well_facies_prediction");
  facies.title = QObject::tr("测井相预测");
  facies.description = QObject::tr(
    "把一口井的曲线段送到用户自己配置的测井相服务（https 或 loopback http，"
    "密钥在系统钥匙串里）拿回相分段。端点未配置时不可用——不替抄第三方地址。");
  facies.requiresLocalOrt = false;
  facies.parameters = {
    {QStringLiteral("model"), QStringLiteral("string"),
     QObject::tr("服务端模型 id（取 WellFaciesService::fetchModels 的结果）"), true},
    {QStringLiteral("well_name"), QStringLiteral("string"),
     QObject::tr("井名"), true},
    {QStringLiteral("depth_from"), QStringLiteral("number"),
     QObject::tr("起始深度"), false},
    {QStringLiteral("depth_to"), QStringLiteral("number"),
     QObject::tr("终止深度"), false},
  };
  facies.resultSchemaJson = QStringLiteral(
    R"({"type":"object","properties":{"well":{"type":"string"},"intervals":{"type":"array","items":{"type":"object","properties":{"top":{"type":"number"},"bottom":{"type":"number"},"code":{"type":"integer"}}}}}})");

  // 4) 工程数据问答（方向77：只读——catalog 实体/资产/角色/版本摘要，
  // 不做任何推理，不写任何库）。
  AiToolSpec query;
  query.name = QStringLiteral("paleo.query_project");
  query.title = QObject::tr("工程数据查询");
  query.description = QObject::tr(
    "只读查询当前工程的目录事实：井清单（含计划井）、层位清单、资产清单、"
    "单井关联明细或计数摘要。回答「工程里有哪些井/层位/资产、某井有什么"
    "曲线」类问题用这只工具；结果带版本路径（可追溯），不写任何数据。");
  query.requiresLocalOrt = false;
  query.parameters = {
    {QStringLiteral("topic"), QStringLiteral("string"),
     QObject::tr("查询主题：summary=计数摘要；wells=井清单；horizons=层位"
                 "资产清单；assets=资产清单；well_details=单井关联明细"),
     true,
     {QStringLiteral("summary"), QStringLiteral("wells"), QStringLiteral("horizons"),
      QStringLiteral("assets"), QStringLiteral("well_details")}},
    {QStringLiteral("well_name"), QStringLiteral("string"),
     QObject::tr("井名（topic=well_details 时必填；按目录规范化匹配）"), false},
    {QStringLiteral("role"), QStringLiteral("string"),
     // 角色清单单源：catalogRoles() 生成（分组名+清单），不再手写串。
     // 「等」字保留——工程 project_area.json 自定义角色不在词表内。
     QObject::tr("关联角色过滤（词表角色按分组：%1 等），作用于 well_details")
         .arg(catalogRoleListForAi()),
     false},
    {QStringLiteral("asset_type"), QStringLiteral("string"),
     QObject::tr("资产类型过滤（如 well_log/horizon/seismic），作用于 assets"),
     false},
  };
  query.resultSchemaJson = QStringLiteral(
    R"({"type":"object","properties":{"tool":{"type":"string"},"topic":{"type":"string"},)"
    R"("count":{"type":"integer"},"wells":{"type":"array","items":{"type":"object"}},)"
    R"("horizons":{"type":"array","items":{"type":"object"}},"assets":{"type":"array","items":{"type":"object"}},)"
    R"("links":{"type":"array","items":{"type":"object"}},"entities":{"type":"object"},)"
    R"("assets_by_type":{"type":"object"},"revision":{"type":"integer"}}})");

  // 5) 派生血缘闭包（方向77：只读——「这个图件是从哪些源来的」）。
  AiToolSpec lineage;
  lineage.name = QStringLiteral("paleo.asset_lineage");
  lineage.title = QObject::tr("资产血缘查询");
  lineage.description = QObject::tr(
    "只读查询某资产的派生血缘：上游版本链（这个产物从哪些井点/约束/中间"
      "产品派生）与下游版本，及选中闭包集合。入参给资产显示名或 id；节点"
      "带版本路径与阶段（可追溯）。");
  lineage.requiresLocalOrt = false;
  lineage.parameters = {
    {QStringLiteral("asset"), QStringLiteral("string"),
     QObject::tr("资产名或 id（versionId / assetId / 显示名，按此序解析）"),
     true},
    {QStringLiteral("upstream_depth"), QStringLiteral("integer"),
     QObject::tr("上游追溯深度（缺省 3，0-12）"), false},
    {QStringLiteral("downstream_depth"), QStringLiteral("integer"),
     QObject::tr("下游追溯深度（缺省 1，0-12）"), false},
  };
  lineage.resultSchemaJson = QStringLiteral(
    R"({"type":"object","properties":{"tool":{"type":"string"},"asset":{"type":"string"},)"
    R"("version_id":{"type":"string"},"available":{"type":"boolean"},)"
    R"("nodes":{"type":"array","items":{"type":"object"}},"edges":{"type":"array","items":{"type":"object"}},)"
    R"("selection":{"type":"object","properties":{"count":{"type":"integer"},)"
    R"("version_ids":{"type":"array","items":{"type":"string"}}}}}})");

  return {tile, horizon, facies, query, lineage};
}

AiToolSpec aiToolSpec(const QString &name) {
  for (const AiToolSpec &spec : builtinAiToolSpecs())
    if (spec.name == name)
      return spec;
  return AiToolSpec();
}

QString aiToolDispatchLabel(AiToolDispatchStatus status) {
  switch (status) {
  case AiToolDispatchStatus::Routed:
    return QObject::tr("已路由");
  case AiToolDispatchStatus::NotFound:
    return QObject::tr("未登记的工具");
  case AiToolDispatchStatus::Disabled:
    return QObject::tr("本构建不可用");
  case AiToolDispatchStatus::NotImplemented:
    return QObject::tr("尚未接线");
  }
  return QString();
}

AiToolDispatch dispatchAiTool(const QString &name,
                              const QJsonObject &arguments) {
  AiToolDispatch out;
  out.spec = aiToolSpec(name);
  if (out.spec.name.isEmpty())
    return out; // NotFound（默认态）
  const QString invalid = out.spec.validateParameters(arguments);
  if (!invalid.isEmpty()) {
    // 参数不合格 = 工具名知道但调用不成：报 Disabled 并把原因写进 note。
    out.status = AiToolDispatchStatus::Disabled;
    out.note = invalid;
    return out;
  }
  if (name == QStringLiteral("paleo.tile_classification")) {
    out.target = QStringLiteral("ai/tileinference.h:runTileInference + "
                                "ai/onnxpredictionservice.h:PaleoOnnxService");
#if PALEO_HAVE_ORT
    // 方向61：执行回路已接线（workflow/aichattoolrunner 经 startClassification
    // 异步执行 + 协作取消）；本函数只答「能否路由」，不执行。
    out.status = AiToolDispatchStatus::Routed;
    out.note = QObject::tr(
      "入口已接线：由聊天工具执行器异步驱动（须绑定工程上下文与模型）");
#else
    out.status = AiToolDispatchStatus::Disabled;
    out.note = QObject::tr("本构建未编译 ONNX Runtime，入口不可达");
#endif
    return out;
  }
  if (name == QStringLiteral("paleo.horizon_tracking_suggestion")) {
    out.target =
      QStringLiteral("ai/horizonsuggest.h:suggestHorizonTracking");
#if PALEO_HAVE_ORT
    out.status = AiToolDispatchStatus::Routed;
    out.note = QObject::tr(
      "入口已接线：由聊天工具执行器异步驱动（须绑定道窗取数上下文）");
#else
    out.status = AiToolDispatchStatus::Disabled;
    out.note = QObject::tr("本构建未编译 ONNX Runtime，入口不可达");
#endif
    return out;
  }
  if (name == QStringLiteral("paleo.well_facies_prediction")) {
    out.target =
      QStringLiteral("ai/wellfaciesservice.h:WellFaciesService::predict");
    out.status = AiToolDispatchStatus::Routed;
    out.note = QObject::tr(
      "入口已接线：须先配置测井相服务端点与密钥，并绑定井曲线数据上下文");
    return out;
  }
  if (name == QStringLiteral("paleo.query_project")) {
    out.target = QStringLiteral(
      "catalog/datacatalog.h:DataCatalog 只读查询 + "
      "workflow/aichattoolrunner:runQuery");
    // 只读工具：无 ORT/远端前置——运行期缺的是工程上下文（catalog 未绑
    // 时执行面如实报错，与 facies 的诚实口径一致）。
    out.status = AiToolDispatchStatus::Routed;
    out.note = QObject::tr("入口已接线：只读查询，须打开工程（未打开时如实报错）");
    return out;
  }
  if (name == QStringLiteral("paleo.asset_lineage")) {
    out.target = QStringLiteral(
      "services/derivationgraph.h:paleo::derivation::Service::build/"
      "selectionClosure + workflow/aichattoolrunner:runLineage");
    out.status = AiToolDispatchStatus::Routed;
    out.note = QObject::tr("入口已接线：只读血缘查询，须打开工程（未打开时如实报错）");
    return out;
  }
  return out;
}
