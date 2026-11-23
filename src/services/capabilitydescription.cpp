// 层：数据
// services/capabilitydescription.cpp — 见 capabilitydescription.h。只读投影实现。
#include "capabilitydescription.h"
#include "capabilityspec.h"
#include "hostcallcontext.h"

#include <QJsonArray>

namespace paleo::capability {
namespace {

QJsonArray stringArray(const QStringList &values) {
  QJsonArray out;
  for (const QString &value : values)
    out.append(value);
  return out;
}

// host 效果位 → 稳定名（hostcallcontext 的枚举单源；不把效果名当独立授权）。
QStringList effectNames(unsigned effects) {
  QStringList names;
  if (effects & paleo::host::Read) names << QStringLiteral("read");
  if (effects & paleo::host::PrivateStage) names << QStringLiteral("private_stage");
  if (effects & paleo::host::CatalogPublish) names << QStringLiteral("catalog_publish");
  if (effects & paleo::host::Presentation) names << QStringLiteral("presentation");
  if (effects & paleo::host::FileExport) names << QStringLiteral("file_export");
  if (effects & paleo::host::ExternalTransfer) names << QStringLiteral("external_transfer");
  if (effects & paleo::host::LocalExecute) names << QStringLiteral("local_execute");
  return names;
}

QJsonObject effectsDescription(unsigned effects) {
  return QJsonObject{{"mask", QString::number(effects)}, {"names", stringArray(effectNames(effects))}};
}

} // namespace

QString capabilityKindName(CapabilityKind kind) {
  switch (kind) {
  case CapabilityKind::Query: return QStringLiteral("query");
  case CapabilityKind::Command: return QStringLiteral("command");
  case CapabilityKind::Compute: return QStringLiteral("compute");
  case CapabilityKind::Workflow: return QStringLiteral("workflow");
  }
  return QStringLiteral("compute");
}

QString algorithmReproducibilityName(AlgorithmReproducibility value) {
  switch (value) {
  case AlgorithmReproducibility::Deterministic: return QStringLiteral("deterministic");
  case AlgorithmReproducibility::Tolerance: return QStringLiteral("tolerance");
  case AlgorithmReproducibility::Nondeterministic: return QStringLiteral("nondeterministic");
  case AlgorithmReproducibility::Unspecified: break;
  }
  return QStringLiteral("unspecified");
}

QString algorithmParamTypeName(AlgorithmParamField::Type type) {
  switch (type) {
  case AlgorithmParamField::Int: return QStringLiteral("integer");
  case AlgorithmParamField::Double: return QStringLiteral("number");
  case AlgorithmParamField::String: return QStringLiteral("string");
  case AlgorithmParamField::FloatList: return QStringLiteral("number_list");
  case AlgorithmParamField::InList: return QStringLiteral("enum");
  case AlgorithmParamField::StringList: return QStringLiteral("string_list");
  }
  return QStringLiteral("number");
}

QJsonObject capabilitySummaryCard(const CapabilitySpec &spec) {
  return QJsonObject{
      {"kind", QStringLiteral("capability")},
      {"id", spec.capabilityId},
      {"contract_version", spec.contractVersion},
      {"capability_kind", capabilityKindName(spec.kind)},
      {"exposure", capabilityExposureName(spec.exposure)},
      {"algorithm_id", spec.algorithmId},
      {"algorithm_contract_version", spec.algorithmContractVersion},
  };
}

QJsonObject capabilityIdentityDescription(const CapabilitySpec &spec) {
  QJsonObject out = capabilitySummaryCard(spec);
  out.insert(QStringLiteral("exposure_reason"), spec.exposureReason);
  out.insert(QStringLiteral("aliases"), stringArray(spec.aliases));
  out.insert(QStringLiteral("publish_effect"), spec.publishEffect);
  out.insert(QStringLiteral("cancel_supported"), spec.cancelSupported);
  out.insert(QStringLiteral("resume_supported"), spec.resumeSupported);
  out.insert(QStringLiteral("restartable"), spec.restartable);
  out.insert(QStringLiteral("operator_shapes"), stringArray(spec.operatorShapes));
  out.insert(QStringLiteral("execution_effects"), effectsDescription(spec.executionEffects));
  return out;
}

QJsonObject algorithmSummaryCard(const AlgorithmSpec &spec) {
  return QJsonObject{
      {"kind", QStringLiteral("algorithm")},
      {"id", spec.id},
      {"contract_version", spec.contractVersion},
      {"display_name", spec.displayName},
      {"engine", spec.engine},
      {"complete", spec.complete},
  };
}

QJsonObject algorithmIdentityDescription(const AlgorithmSpec &spec) {
  QJsonObject out = algorithmSummaryCard(spec);
  out.insert(QStringLiteral("implementation_version"), spec.implementationVersion);
  out.insert(QStringLiteral("param_schema_version"), spec.paramSchemaVersion);
  out.insert(QStringLiteral("execution_shape"), executionShapeName(spec.executionShape));
  out.insert(QStringLiteral("reproducibility"), algorithmReproducibilityName(spec.reproducibility));
  out.insert(QStringLiteral("cancel_supported"), spec.cancelSupported);
  out.insert(QStringLiteral("resume_supported"), spec.resumeSupported);
  return out;
}

QJsonObject algorithmParamDescription(const AlgorithmParamField &field) {
  QJsonObject out;
  out.insert(QStringLiteral("key"), field.key);
  out.insert(QStringLiteral("label"), field.label);
  out.insert(QStringLiteral("type"), algorithmParamTypeName(field.type));
  out.insert(QStringLiteral("required"), field.required);
  // applyDefaultWhenAbsent=false：缺席时不把默认值写进执行参数（如 FACIES_CODE）。
  out.insert(QStringLiteral("apply_default_when_absent"), field.applyDefaultWhenAbsent);
  if (field.defaultValue.isValid())
    out.insert(QStringLiteral("default_value"), QJsonValue::fromVariant(field.defaultValue));
  if (field.hasMin)
    out.insert(QStringLiteral("minimum"), field.minValue);
  if (field.hasMax)
    out.insert(QStringLiteral("maximum"), field.maxValue);
  if (field.type == AlgorithmParamField::Double && field.decimals > 0)
    out.insert(QStringLiteral("decimals"), field.decimals);
  if (!field.suffix.isEmpty())
    out.insert(QStringLiteral("suffix"), field.suffix);
  if (!field.options.isEmpty()) {
    QJsonArray options;
    for (const AlgorithmParamOption &option : field.options) {
      QJsonObject item;
      item.insert(QStringLiteral("label"), option.label);
      item.insert(QStringLiteral("value"), QJsonValue::fromVariant(option.value));
      options.append(item);
    }
    out.insert(QStringLiteral("options"), options);
  }
  return out;
}

QJsonObject algorithmParamsDescription(const AlgorithmSpec &spec) {
  QJsonObject out;
  out.insert(QStringLiteral("source"), QStringLiteral("AlgorithmSpec"));
  out.insert(QStringLiteral("defaults_are_single_source"), true);
  QJsonArray params;
  for (const AlgorithmParamField &field : spec.params)
    params.append(algorithmParamDescription(field));
  out.insert(QStringLiteral("fields"), params);
  return out;
}

QJsonObject algorithmPortDescription(const AlgoPort &port) {
  QJsonObject out;
  out.insert(QStringLiteral("id"), port.id);
  out.insert(QStringLiteral("kind"), port.kind);
  if (!port.role.isEmpty())
    out.insert(QStringLiteral("role"), port.role);
  if (!port.geometryHint.isEmpty())
    out.insert(QStringLiteral("geometry"), port.geometryHint);
  out.insert(QStringLiteral("required"), port.required);
  out.insert(QStringLiteral("hidden"), port.hidden);
  out.insert(QStringLiteral("cardinality_min"),
             port.cardinalityMin >= 0 ? QJsonValue(port.cardinalityMin)
                                      : QJsonValue(port.effectiveCardinalityMin()));
  out.insert(QStringLiteral("cardinality_max"), port.cardinalityMax);
  // 空 unit/axis/verticalDatum 原样留空——不从显示名猜测补齐。
  out.insert(QStringLiteral("unit"), port.unit);
  out.insert(QStringLiteral("axis"), port.axis);
  out.insert(QStringLiteral("vertical_datum"), port.verticalDatum);
  if (!port.constraints.isEmpty())
    out.insert(QStringLiteral("constraints"), QJsonObject::fromVariantMap(port.constraints));
  return out;
}

QJsonObject algorithmPortsDescription(const AlgorithmSpec &spec) {
  QJsonArray inputs, outputs;
  for (const AlgoPort &port : spec.inputs)
    inputs.append(algorithmPortDescription(port));
  for (const AlgoPort &port : spec.outputs)
    outputs.append(algorithmPortDescription(port));
  return QJsonObject{{"inputs", inputs}, {"outputs", outputs}};
}

QJsonObject commandSummaryCard(const paleo::command::CommandSpec &spec) {
  return QJsonObject{
      {"kind", QStringLiteral("command")},
      {"id", spec.commandId},
      {"contract_version", spec.contractVersion},
      {"command_kind", paleo::command::commandKindName(spec.kind)},
      {"stage", spec.stageId},
      {"effect", spec.effect},
      {"agent_surface", spec.hasSurface(paleo::command::SurfaceAgent)},
  };
}

QJsonObject commandIdentityDescription(const paleo::command::CommandSpec &spec) {
  QJsonObject out = commandSummaryCard(spec);
  out.insert(QStringLiteral("title"), QString::fromUtf8(spec.title));
  if (spec.tooltip && *spec.tooltip)
    out.insert(QStringLiteral("tooltip"), QString::fromUtf8(spec.tooltip));
  if (!spec.ribbonGroup.isEmpty())
    out.insert(QStringLiteral("ribbon_group"), spec.ribbonGroup);
  out.insert(QStringLiteral("required_context"), stringArray(spec.requiredContext));
  out.insert(QStringLiteral("doc_kinds"), stringArray(spec.docKinds));
  out.insert(QStringLiteral("shortcut_id"), spec.shortcutId);
  QStringList surfaces;
  if (spec.hasSurface(paleo::command::SurfaceRibbon)) surfaces << QStringLiteral("ribbon");
  if (spec.hasSurface(paleo::command::SurfaceLocator)) surfaces << QStringLiteral("locator");
  if (spec.hasSurface(paleo::command::SurfaceContextMenu)) surfaces << QStringLiteral("context_menu");
  if (spec.hasSurface(paleo::command::SurfaceShortcut)) surfaces << QStringLiteral("shortcut");
  if (spec.hasSurface(paleo::command::SurfaceAgent)) surfaces << QStringLiteral("agent");
  if (spec.hasSurface(paleo::command::SurfaceMenu)) surfaces << QStringLiteral("menu");
  out.insert(QStringLiteral("surfaces"), stringArray(surfaces));
  // View 命令无 CapabilitySpec 是注册表既定规则，原样呈现空引用。
  out.insert(QStringLiteral("capability_id"), spec.capabilityId);
  return out;
}

} // namespace paleo::capability
