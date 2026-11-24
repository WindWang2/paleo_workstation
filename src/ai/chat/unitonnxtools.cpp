// 层：功能
#include "unitonnxtools.h"
#include "domaintools.h"
#include "services/unitonnxcapabilityspec.h"
#include <QJsonArray>
#include <QSet>
#include <cmath>

// paleo.unit_onnx_predict（unit.onnx_predict v1）：明确地层分析单元（S1–S4）
// 的层段窗口 RMS → 钉权重本地 ONNX 分类推理。三段式与 tile v2 同语义：
// prepare 冻结并物化输入（持久 operationId）、compute-only 私有推理、
// publish 只提交已计算的 source 操作；后两段不接受重述输入。

namespace {
const QSet<QString> &unitSelectionKeys() {
  static const QSet<QString> keys{"schemeId", "revision", "intervalId", "subIntervalId",
                                  "subIndex", "aggregate", "displayName", "topBoundary", "bottomBoundary"};
  return keys;
}

bool safeName(const QString &name) {
  return !name.isEmpty() && name.size() <= 256 && !name.contains(QLatin1Char('/')) &&
         !name.contains(QLatin1Char('\\')) && !name.contains(QChar(0));
}

bool strictSubIndex(const QJsonValue &value) {
  if (!value.isDouble()) return false;
  const double number = value.toDouble();
  return std::isfinite(number) && number == std::floor(number) && number >= 1 && number <= 4;
}
} // namespace

bool isUnitOnnxTool(const QString &name) {
  return name == QLatin1String(paleo::kUnitOnnxToolName);
}

QString validateUnitOnnxToolArguments(const QJsonObject &arguments) {
  const auto phase = arguments.value(QStringLiteral("publish_intent")).toString();
  if (arguments.contains(QStringLiteral("contract_version"))) {
    const auto version = arguments.value(QStringLiteral("contract_version"));
    if (!version.isDouble() || version.toDouble() != 1) return QStringLiteral("unsupported_version");
  }
  if (arguments.contains(QStringLiteral("binding_mode")) &&
      arguments.value(QStringLiteral("binding_mode")).toString() != QLatin1String("pinned-value"))
    return QStringLiteral("binding_mode_unsupported_in_v1");
  const auto inputsValue = arguments.value(QStringLiteral("inputs"));
  if (!inputsValue.isObject()) return QStringLiteral("unit_required_inputs");
  const auto inputs = inputsValue.toObject();
  if (phase == QLatin1String("prepare")) {
    if (inputs.size() != 3 || !inputs.contains(QStringLiteral("model")) ||
        !inputs.contains(QStringLiteral("seismic_version_id")) ||
        !inputs.contains(QStringLiteral("unit_selection")))
      return QStringLiteral("unit_required_inputs");
    if (!safeName(inputs.value(QStringLiteral("model")).toString()) ||
        !inputs.value(QStringLiteral("model")).isString())
      return QStringLiteral("invalid_model_name");
    const auto seismic = inputs.value(QStringLiteral("seismic_version_id"));
    if (!seismic.isString() || seismic.toString().isEmpty() || seismic.toString().size() > 256)
      return QStringLiteral("seismic_version_required");
    const auto unit = inputs.value(QStringLiteral("unit_selection"));
    if (!unit.isObject()) return QStringLiteral("unit_selection_invalid");
    const auto selection = unit.toObject();
    for (auto it = selection.constBegin(); it != selection.constEnd(); ++it)
      if (!unitSelectionKeys().contains(it.key())) return QStringLiteral("unit_selection_invalid");
    for (const auto &key : {QStringLiteral("schemeId"), QStringLiteral("intervalId"), QStringLiteral("subIntervalId")}) {
      const auto value = selection.value(key);
      if (!value.isString() || value.toString().isEmpty() || value.toString().size() > 256)
        return QStringLiteral("unit_selection_invalid");
    }
    if (selection.contains(QStringLiteral("aggregate")) &&
        (!selection.value(QStringLiteral("aggregate")).isBool() ||
         selection.value(QStringLiteral("aggregate")).toBool()))
      return QStringLiteral("unit_selection_invalid");
    if (selection.contains(QStringLiteral("subIndex")) &&
        !strictSubIndex(selection.value(QStringLiteral("subIndex"))))
      return QStringLiteral("unit_selection_invalid");
    return QString();
  }
  const auto source = inputs.value(QStringLiteral("source_operation_id"));
  if (inputs.size() != 1 || !source.isString() || source.toString().isEmpty() ||
      source.toString().size() > 256)
    return QStringLiteral("source_operation_required");
  return QString();
}

QString unitOnnxReadinessCode(PaleoOnnxService *onnx, const QJsonObject &args, bool listing) {
  const auto phase = args.value(QStringLiteral("publish_intent")).toString();
  if (listing || (phase != QLatin1String("prepare") && phase != QLatin1String("compute-only")))
    return {};
#if !PALEO_HAVE_ORT
  Q_UNUSED(onnx);
  return QStringLiteral("ort_unavailable");
#else
  return onnx ? QString() : QStringLiteral("ort_provider_unavailable");
#endif
}

void appendAgentUnitOnnxTools(QVector<AiToolSpec> &specs) {
  AiToolSpec tool;
  tool.name = QLatin1String(paleo::kUnitOnnxToolName);
  tool.title = QObject::tr("地层单元 ONNX 预测");
  tool.description = QObject::tr(
      "对明确的地层分析单元（方案内 S1–S4 子层段）从受管地震体提取 64×64 层段窗口 RMS，"
      "交给当前工程 models/ 注册表内钉 SHA 的本地 ONNX 分类模型推理。先用 query_project "
      "（local_models / version）取得精确模型名、地震体目录版本与单元引用。"
      "prepare 冻结模型/来源/单元并物化输入，返回持久 operationId；compute-only 只接受 "
      "prepare 的 source_operation_id（输入不可重述，重启后仍按同一冻结输入计算）；"
      "publish 只提交已计算的 source_operation_id，不重复推理。类别输出是模型通道索引，"
      "不带地质标签映射；不自动选择或显示图层。");
  tool.requiresLocalOrt = false; // ORT/模型只在 prepare/新 compute 就绪段要求（unitOnnxReadinessCode）。
  tool.parameters = {
      {QStringLiteral("client_request_id"), QStringLiteral("string"),
       QObject::tr("调用方请求号（同一意图的幂等键；重放返回原操作事实）"), true, {}},
      {QStringLiteral("publish_intent"), QStringLiteral("string"),
       QObject::tr("执行阶段"), true,
       {QStringLiteral("prepare"), QStringLiteral("compute-only"), QStringLiteral("publish")}},
      {QStringLiteral("contract_version"), QStringLiteral("integer"),
       QObject::tr("能力契约版本；缺省 1（v1 仅接受 1）"), false, {}},
      {QStringLiteral("binding_mode"), QStringLiteral("string"),
       QObject::tr("绑定模式；v1 仅支持 pinned-value（可省略，省略等同 pinned-value）"), false,
       {QStringLiteral("pinned-value"), QStringLiteral("follow-object-head"), QStringLiteral("follow-selection")}},
      {QStringLiteral("inputs"), QStringLiteral("object"),
       QObject::tr("prepare：{model: 注册表模型名, seismic_version_id: 地震体目录版本 id, "
                  "unit_selection: {schemeId, revision?, intervalId, subIntervalId, subIndex, aggregate:false}}；"
                  "compute-only/publish：{source_operation_id}（继承输入不可重述）"),
       true, {}},
  };
  tool.effects = paleo::host::Read | paleo::host::PrivateStage;
  tool.resultSchemaJson = QStringLiteral(
      R"({"type":"object","required":["code"],"properties":{"ok":{"type":"boolean"},"code":{"type":"string"},"operationId":{"type":"string"},"execution":{"type":"string"},"commitState":{"type":"string"},"presentation":{"type":"string"},"revision":{"type":"string"},"computeCount":{"type":"integer"},"resultRef":{"type":"object"}}})");
  specs.append(tool);
}
