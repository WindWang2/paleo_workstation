// 层：功能
#include "unitonnxcontract.h"
#include "ai/onnxpredictionservice.h"
#include "services/algorithmspec.h"
#include "services/analysisunitcontext.h"
#include "services/frozenrasterinput.h"
#include "services/seismicunitinput.h"
#include "services/unitonnxcapabilityspec.h"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonDocument>
#include <cmath>
#include <cstring>
#include <utility>

namespace paleo {
namespace {
constexpr auto kPlanSchema = "paleo.unit-onnx-plan.v1";
// 冻结计划 JSON 的字节预算：4096 float32 base64 ≈ 22 KiB + 元数据。超出即
// 拒绝（防把任意大数组灌进持久 journal）。
constexpr qsizetype kMaxPlanJsonBytes = 96 * 1024;

bool reject(QString *error, const QString &code) { if (error) *error = code; return false; }

bool isHex64(const QString &value) {
  if (value.size() != 64) return false;
  for (const auto &ch : value)
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) return false;
  return true;
}

// ModelRegistry::isSafeModelName 的本地镜像（非 ORT 构建不链接 modelregistry.cpp，
// 而本文件的序列化校验在两种构建都要工作）。注册表门控本身仍在 prepare 的
// ORT 段执行。
bool safeModelName(const QString &name) {
  if (name.isEmpty() || name.size() > 256 || name.startsWith(QLatin1Char('/'))) return false;
  if (name == QLatin1String(".") || name == QLatin1String("..")) return false;
  return !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\')) &&
         !name.contains(QChar(0));
}

bool boundedText(const QString &value, int maximum) {
  return !value.isEmpty() && value.size() <= maximum && !value.contains(QChar(0)) &&
         !value.contains('\n') && !value.contains('\r');
}

// 冻结单元必须是方案内具体 S1–S4 子层段（freezeAnalysisUnit 的产出形态）：
// 具体 revision、无 aggregate、subIndex 1..4。显示名字段允许存在但不参与身份。
bool frozenUnitShape(const QJsonObject &unit) {
  const QStringList allowed{"schemeId", "revision", "intervalId", "subIntervalId",
                            "aggregate", "subIndex", "displayName", "topBoundary", "bottomBoundary"};
  for (auto it = unit.constBegin(); it != unit.constEnd(); ++it)
    if (!allowed.contains(it.key())) return false;
  const auto selection = paleo::strat::fromMap(unit.toVariantMap());
  if (!selection.isValid() || selection.aggregate || selection.revision.isEmpty() ||
      selection.subIndex < 1 || selection.subIndex > paleo::strat::kSubIntervalCount)
    return false;
  return unit.value("revision").isString() && unit.value("intervalId").isString() &&
         unit.value("subIntervalId").isString() && unit.value("schemeId").isString();
}

QJsonObject hashBody(const QJsonObject &plan) {
  return QJsonObject{{"schema", QLatin1String(kPlanSchema)}, {"unit", plan.value("unit")},
                     {"unitKey", plan.value("unitKey")}, {"seismic", plan.value("seismic")},
                     {"model", plan.value("model")}, {"modelSha256", plan.value("modelSha256")},
                     {"inputName", plan.value("inputName")}, {"inputSignature", plan.value("inputSignature")},
                     {"outputSemantics", plan.value("outputSemantics")}, {"classes", plan.value("classes")},
                     {"featureContractVersion", plan.value("featureContractVersion")},
                     {"modelFeatures", plan.value("modelFeatures")}, {"rows", plan.value("rows")},
                     {"columns", plan.value("columns")}, {"validCells", plan.value("validCells")},
                     {"extent", plan.value("extent")}, {"crs", plan.value("crs")},
                     {"samples", plan.value("samples")},
                     {"inputFeatureContract", unitOnnxInputFeatureContract()}};
}

QString schemaHash(const QJsonObject &plan) {
  return QString::fromLatin1(QCryptographicHash::hash(
      QJsonDocument(hashBody(plan)).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}

// 模型特征声明 → 规范化单特征；与 tile 的 modelFeature 同口径（版本 1、
// 单特征、8 KiB 预算）。不猜语义：规范化结果必须与 layer_window_rms 输入
// 合同完全一致。
bool modelFeature(const QJsonArray &features, int version, QJsonObject &out) {
  if (version != 1 || features.size() != 1 ||
      QJsonDocument(features).toJson(QJsonDocument::Compact).size() > 8192)
    return false;
  auto feature = features[0].toObject();
  if (feature.contains("version") && feature.value("version") != QJsonValue(version)) return false;
  feature.insert("version", version);
  return normalizeRasterFeatureContract(feature, out);
}

#if PALEO_HAVE_ORT
// 下游服务的中文诊断不进 ExecuteResult.code（code 只走宿主 safeTag 机器码
// 词表）。按调用点分类而不是匹配句子：单元冻结失败只可能是单元身份/修订
// 问题；层段取数失败默认归地震来源不可用，语义明确的窗口问题单列。
QString unitSelectionFailure() { return QStringLiteral("unit_selection_invalid"); }

QString unitWindowFailure(const QString &message) { return unitOnnxInputFailureCode(message); }

// 首输出合同（分类）：float32、[1,C,H,W]、C 与声明一致且 1..255、元素预算
// 32 MiB。H/W 若为固定维必须等于冻结网格（动态 -1 允许，由输入侧保证）。
QString unitOutputContractCode(const OnnxModelMeta &meta, int expectedClasses, int rows, int columns) {
  if (meta.outputElemType != 1) return QStringLiteral("unit_model_output_type_unsupported");
  if (meta.outputShape.size() != 4 || meta.outputShape[0] != 1)
    return QStringLiteral("unit_model_output_signature_incompatible");
  const auto classes = meta.outputShape[1];
  if (classes < 1) return QStringLiteral("unit_model_output_classes_unknown");
  if (classes > 255) return QStringLiteral("unit_model_output_classes_out_of_range");
  if (expectedClasses > 0 && classes != expectedClasses)
    return QStringLiteral("unit_model_output_classes_mismatch");
  if ((meta.outputShape[2] > 0 && meta.outputShape[2] != rows) ||
      (meta.outputShape[3] > 0 && meta.outputShape[3] != columns))
    return QStringLiteral("unit_model_output_signature_incompatible");
  qint64 elements = 1;
  for (const auto dimension : meta.outputShape)
    if (dimension > 0) {
      if (dimension > kMaxOnnxOutputElements || elements > kMaxOnnxOutputElements / dimension)
        return QStringLiteral("model_output_budget_exceeded");
      elements *= dimension;
    }
  return {};
}
#endif
} // namespace

QString unitOnnxInputFailureCode(const QString &message) {
  // 「已取消」与「地震层位窗口读取已取消」都是取消。QLatin1String 比不了
  // 非 Latin-1 文本，精确相等会把窗口取消句落成来源不可用。
  if (message == QStringLiteral("已取消") || message.endsWith(QStringLiteral("已取消")))
    return QStringLiteral("cancelled");
  if (message.contains(QStringLiteral("已变更"))) return QStringLiteral("seismic_source_changed");
  if (message.contains(QStringLiteral("坐标系"))) return QStringLiteral("unit_window_crs_unavailable");
  if (message.contains(QStringLiteral("没有有效地震样点"))) return QStringLiteral("unit_window_no_valid_samples");
  if (message.contains(QStringLiteral("坐标范围"))) return QStringLiteral("unit_window_geometry_unavailable");
  return QStringLiteral("seismic_source_unavailable");
}

QString FrozenUnitOnnxPlan::samplesSha256() const {
  return QString::fromLatin1(QCryptographicHash::hash(samples, QCryptographicHash::Sha256).toHex());
}

QJsonObject FrozenUnitOnnxPlan::toJson() const {
  QJsonObject grid;
  grid.insert("left", extent.left());
  grid.insert("top", extent.top());
  grid.insert("width", extent.width());
  grid.insert("height", extent.height());
  QJsonObject seismic{{"versionId", seismicVersionId}, {"sha256", seismicSha256},
                      {"path", seismicPath}, {"size", double(seismicSize)},
                      {"mtimeMs", double(seismicMtimeMs)}};
  return QJsonObject{{"schema", QLatin1String(kPlanSchema)}, {"unit", unit}, {"unitKey", unitKey},
                     {"seismic", seismic}, {"model", model}, {"modelSha256", modelSha256},
                     {"inputName", inputName}, {"inputSignature", inputSignature},
                     {"outputSemantics", outputSemantics}, {"classes", classCount},
                     {"featureContractVersion", featureContractVersion},
                     {"modelFeatures", modelFeatures}, {"rows", rows}, {"columns", columns},
                     {"validCells", validCells}, {"extent", grid}, {"crs", crs},
                     {"samples", QLatin1String(samples.toBase64())},
                     {"featureSchemaHash", featureSchemaHash}};
}

namespace {
// 解析 + 逐字段校验在调用方给的值上完成；失败时该值不得被使用（fromJson
// 进一步保证失败不向外出参留下半份计划）。
bool parseFrozenUnitPlan(const QJsonObject &json, FrozenUnitOnnxPlan &out, QString *error) {
  out = {};
  if (json.value("schema") != QLatin1String(kPlanSchema)) return reject(error, "invalid_frozen_unit_plan");
  out.unit = json.value("unit").toObject();
  out.unitKey = json.value("unitKey").toString();
  const auto seismic = json.value("seismic").toObject();
  out.seismicVersionId = seismic.value("versionId").toString();
  out.seismicSha256 = seismic.value("sha256").toString();
  out.seismicPath = seismic.value("path").toString();
  out.seismicSize = qlonglong(seismic.value("size").toDouble(-1));
  out.seismicMtimeMs = qlonglong(seismic.value("mtimeMs").toDouble(-1));
  out.model = json.value("model").toString();
  out.modelSha256 = json.value("modelSha256").toString();
  out.inputName = json.value("inputName").toString();
  out.inputSignature = json.value("inputSignature").toString();
  out.outputSemantics = json.value("outputSemantics").toString();
  out.classCount = json.value("classes").toInt(-1);
  out.featureContractVersion = json.value("featureContractVersion").toInt(-1);
  out.modelFeatures = json.value("modelFeatures").toArray();
  out.rows = json.value("rows").toInt(-1);
  out.columns = json.value("columns").toInt(-1);
  out.validCells = json.value("validCells").toInt(-1);
  const auto grid = json.value("extent").toObject();
  out.extent = QRectF(grid.value("left").toDouble(), grid.value("top").toDouble(),
                      grid.value("width").toDouble(), grid.value("height").toDouble());
  out.crs = json.value("crs").toString();
  out.samples = QByteArray::fromBase64(json.value("samples").toString().toLatin1(),
                                       QByteArray::AbortOnBase64DecodingErrors);
  out.featureSchemaHash = json.value("featureSchemaHash").toString();
  // 逐字段闸：任何越界/缺失/前后不一致都整体拒绝，不部分采信。
  if (QJsonDocument(json).toJson(QJsonDocument::Compact).size() > kMaxPlanJsonBytes ||
      !frozenUnitShape(out.unit) || !boundedText(out.unitKey, 256) ||
      !boundedText(out.seismicVersionId, 256) || !isHex64(out.seismicSha256) ||
      !QFileInfo(out.seismicPath).isAbsolute() || out.seismicSize < 0 || out.seismicMtimeMs < 0 ||
      !safeModelName(out.model) || !isHex64(out.modelSha256) ||
      !boundedText(out.inputName, 256) || !boundedText(out.inputSignature, 256) ||
      (out.outputSemantics != QLatin1String("logits") && out.outputSemantics != QLatin1String("probabilities")) ||
      out.classCount < 1 || out.classCount > 255 || out.rows != 64 || out.columns != 64 ||
      out.validCells < 1 || out.validCells > out.rows * out.columns ||
      !out.extent.isValid() || !(out.extent.width() > 0) || !(out.extent.height() > 0) ||
      out.crs.isEmpty() || out.crs.size() > 4096 ||
      out.samples.size() != qsizetype(out.rows) * out.columns * 4)
    return reject(error, "invalid_frozen_unit_plan");
  QJsonObject feature;
  if (!modelFeature(out.modelFeatures, out.featureContractVersion, feature) ||
      feature != unitOnnxInputFeatureContract() || schemaHash(json) != out.featureSchemaHash)
    return reject(error, "invalid_frozen_unit_plan");
  return true;
}
} // namespace

// 失败不向外留下半份计划：解析/校验在局部值上完成，整体通过才交付。
bool FrozenUnitOnnxPlan::fromJson(const QJsonObject &json, FrozenUnitOnnxPlan &out, QString *error) {
  out = {};
  FrozenUnitOnnxPlan parsed;
  if (!parseFrozenUnitPlan(json, parsed, error)) return false;
  out = std::move(parsed);
  return true;
}

bool prepareUnitOnnxPlan(PaleoOnnxService *onnx, const UnitOnnxPrepareSource &source,
                         FrozenUnitOnnxPlan &out, const std::function<bool()> &cancelled,
                         QString *error) {
  out = {};
#if !PALEO_HAVE_ORT
  Q_UNUSED(onnx); Q_UNUSED(source); Q_UNUSED(cancelled);
  return reject(error, "ort_unavailable");
#else
  if (cancelled && cancelled()) return reject(error, "cancelled");
  if (!onnx) return reject(error, "ort_unavailable");
  if (source.projectDir.isEmpty() || !safeModelName(source.model) ||
      source.seismicVersionId.isEmpty() || source.seismicPath.isEmpty() ||
      !QFileInfo(source.seismicPath).isAbsolute() || source.seismicSize < 0 || source.seismicMtimeMs < 0)
    return reject(error, "unit_prepare_input_invalid");
  // W2：owner 已冻结的注册表条目在此复核——注册表是身份来源，availableModels
  // 的列表成功不构成运行通过。
  const auto entry = onnx->registryEntryFor(source.model);
  if (entry.status != ModelRegistryEntry::Status::Ok || !isHex64(entry.sha256Pinned))
    return reject(error, "model_unavailable");
  // 真实 ORT 兼容检查：加载、实测 SHA、warmup 计时与错误原样保留（不吞）。
  OnnxModelMeta meta;
  QString loadError;
  const auto status = onnx->loadModelMeta(source.model, &meta, &loadError);
  if (status != OnnxLoadStatus::Ok) return reject(error, onnxPublicLoadCode(status, loadError));
  if (meta.sha256.compare(entry.sha256Pinned, Qt::CaseInsensitive) != 0)
    return reject(error, "model_fingerprint_mismatch");
  // 单输入/首输出合同：[1,1,·,·] float32；输出分类合同（与声明一致）。
  if (meta.inputShape.size() != 4 || meta.inputShape[0] > 1 || meta.inputShape[1] > 1 ||
      (meta.inputShape[2] > 0 && meta.inputShape[2] != kUnitOnnxGridRows) ||
      (meta.inputShape[3] > 0 && meta.inputShape[3] != kUnitOnnxGridColumns))
    return reject(error, "unit_model_signature_incompatible");
  if (meta.inputElemType != 1) return reject(error, "unit_model_input_type_unsupported");
  const auto outputCode = unitOutputContractCode(meta, meta.classes, kUnitOnnxGridRows, kUnitOnnxGridColumns);
  if (!outputCode.isEmpty()) return reject(error, outputCode);
  if (meta.outputSemantics != QLatin1String("logits") && meta.outputSemantics != QLatin1String("probabilities"))
    return reject(error, "unit_model_output_semantics_unsupported");
  // W5：特征语义匹配——声明必须可验证兼容 layer_window_rms 输入合同。
  QJsonObject feature;
  if (!modelFeature(meta.features, meta.featureContractVersion, feature))
    return reject(error, "model_feature_contract_unavailable");
  if (feature != unitOnnxInputFeatureContract())
    return reject(error, "unit_feature_contract_mismatch");
  if (cancelled && cancelled()) return reject(error, "cancelled");
  // W3：稳定分析单元——freezeAnalysisUnit 拒绝 Boundary/伪造 S/aggregate/
  // 旧 revision；显示名不参与身份。
  paleo::strat::UnitSelection frozen;
  QString unitError;
  if (!paleo::strat::freezeAnalysisUnit(source.projectDir, source.unit, frozen, &unitError))
    return reject(error, unitSelectionFailure());
  // W4：复用层段窗口生成有限 RMS 输入（64×64、NaN 缺覆盖、读取窗口预算
  // 1,048,576 值；selectVolume 验证来源确在本工区数据服务中）。
  const auto input = paleo::strat::prepareSeismicUnitInput(source.projectDir, frozen, source.seismicPath, cancelled);
  if (!input.error.isEmpty()) return reject(error, unitWindowFailure(input.error));
  if (input.rows != kUnitOnnxGridRows || input.columns != kUnitOnnxGridColumns ||
      input.samples.size() != qsizetype(input.rows) * input.columns)
    return reject(error, "unit_window_grid_unexpected");
  if (input.crs.isEmpty()) return reject(error, "unit_window_crs_unavailable");
  if (input.validCells < 1) return reject(error, "unit_window_no_valid_samples");
  if (cancelled && cancelled()) return reject(error, "cancelled");
  out.unit = QJsonObject::fromVariantMap(paleo::strat::toMap(frozen));
  out.unitKey = paleo::strat::analysisLayerKey(frozen);
  out.seismicVersionId = source.seismicVersionId;
  out.seismicSha256 = source.seismicSha256;
  out.seismicPath = source.seismicPath;
  out.seismicSize = source.seismicSize;
  out.seismicMtimeMs = source.seismicMtimeMs;
  out.model = source.model;
  out.modelSha256 = entry.sha256Pinned;
  out.inputName = meta.inputName;
  out.inputSignature = meta.inputSignature;
  out.outputSemantics = meta.outputSemantics;
  out.classCount = int(meta.outputShape[1]);
  out.featureContractVersion = meta.featureContractVersion;
  out.modelFeatures = meta.features;
  out.rows = input.rows;
  out.columns = input.columns;
  out.validCells = input.validCells;
  out.extent = input.extent;
  out.crs = input.crs;
  out.samples.resize(input.samples.size() * qsizetype(sizeof(float)));
  std::memcpy(out.samples.data(), input.samples.constData(), qsizetype(out.samples.size()));
  const auto json = out.toJson();
  if (QJsonDocument(json).toJson(QJsonDocument::Compact).size() > kMaxPlanJsonBytes)
    return reject(error, "unit_plan_budget_exceeded");
  out.featureSchemaHash = schemaHash(json);
  return true;
#endif
}
} // namespace paleo
