// 层：功能
#include "unitonnxoperation.h"
#include "ai/onnxpredictionservice.h"
#include "ai/tileinference.h"
#include "algorithms/projectgeotiff.h"
#include "services/unitonnxcapabilityspec.h"
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QVector>
#include <cmath>
#include <cstring>

namespace paleo {
namespace {
#if PALEO_HAVE_ORT
// runPinnedTensor 的失败链 → 稳定机器码（内部诊断文本不进
// ExecuteResult.code）。它只有两个稳定机器码原样保留；其余（会话在加载与
// 推理之间被淘汰/销毁、Run 失败等）一律 unit_inference_failed——不靠匹配
// 句子猜原因。
QString runFailureCode(const QString &message) {
  if (message == QLatin1String("model_revision_changed") ||
      message == QLatin1String("model_fingerprint_required"))
    return message;
  return QStringLiteral("unit_inference_failed");
}

bool writeByteRaster(const QString &path, int columns, int rows, const double gt[6], const QString &crs,
                     const QVector<quint8> &values, const QJsonObject &provenance, QString &error) {
  if (QFileInfo::exists(path)) { error = "private_output_already_exists"; return false; }
  auto ds = ProjectGeoTiff::create(path, columns, rows, GDT_Byte);
  if (!ds) { error = "unit_output_create_failed"; return false; }
  bool ok = GDALSetGeoTransform(ds, gt) == CE_None && GDALSetProjection(ds, crs.toUtf8().constData()) == CE_None;
  const auto band = GDALGetRasterBand(ds, 1);
  GDALSetRasterNoDataValue(band, 255.0);
  GDALSetMetadataItem(ds, "PALEO_PROVENANCE",
                      QJsonDocument(provenance).toJson(QJsonDocument::Compact).constData(), nullptr);
  ok = ok && GDALRasterIO(band, GF_Write, 0, 0, columns, rows, const_cast<quint8 *>(values.constData()),
                          columns, rows, GDT_Byte, 0, 0) == CE_None;
  ok = GDALClose(ds) == CE_None && ok;
  if (!ok) { QFile::remove(path); error = "unit_output_write_failed"; }
  return ok;
}
#endif
} // namespace

AlgorithmRunResult computeUnitOnnxPlan(PaleoOnnxService *onnx, const FrozenUnitOnnxPlan &plan,
                                       const QString &privateDir, AlgorithmRunResult &diagnostics,
                                       QJsonObject &summary, const std::function<bool()> &cancelled) {
  AlgorithmRunResult result;
  diagnostics = {};
  summary = {};
  const auto fail = [&](const QString &code) { result.ok = false; result.error = code; result.artifacts.clear(); diagnostics = {}; return result; };
#if !PALEO_HAVE_ORT
  Q_UNUSED(onnx); Q_UNUSED(plan); Q_UNUSED(privateDir); Q_UNUSED(cancelled); Q_UNUSED(summary);
  return fail("ort_unavailable");
#else
  if (!onnx) return fail("ort_unavailable");
  FrozenUnitOnnxPlan checked;
  QString error;
  if (!FrozenUnitOnnxPlan::fromJson(plan.toJson(), checked, &error)) return fail(error);
  if (privateDir.isEmpty() || !QFileInfo(privateDir).isAbsolute() || !QDir().mkpath(privateDir))
    return fail("private_directory_unavailable");
  if (cancelled && cancelled()) return fail("cancelled");
  // W6：每次执行重新验证冻结 SHA/签名——准备后换权重/换注册表/并发淘汰
  // 都在这里被拒绝，绝不静默切到另一个活动模型。
  OnnxModelMeta meta;
  const auto status = onnx->loadModelMeta(checked.model, &meta, &error);
  if (status != OnnxLoadStatus::Ok) return fail(onnxPublicLoadCode(status, error));
  if (meta.sha256.compare(checked.modelSha256, Qt::CaseInsensitive) != 0)
    return fail("model_changed_since_prepare");
  if (meta.inputSignature != checked.inputSignature || meta.inputName != checked.inputName ||
      meta.outputSemantics != checked.outputSemantics || int(meta.outputShape.value(1)) != checked.classCount)
    return fail("model_contract_changed_since_prepare");
  if (cancelled && cancelled()) return fail("cancelled");
  const qsizetype pixels = qsizetype(checked.rows) * checked.columns;
  QVector<float> input(pixels);
  QVector<bool> valid(pixels, false);
  for (qsizetype i = 0; i < pixels; ++i) {
    float value;
    std::memcpy(&value, checked.samples.constData() + i * 4, 4);
    // 缺测输入的既有适配：NaN→0 喂模型；输出端重施有效 mask，不宣称缺测格
    // 获得可信预测。
    valid[i] = std::isfinite(value);
    input[i] = valid[i] ? value : 0.0f;
  }
  QElapsedTimer elapsed;
  elapsed.start();
  const OnnxTensor tensor = onnx->runPinnedTensor(checked.model, checked.modelSha256, checked.inputName,
                                                  input, {1, 1, checked.rows, checked.columns}, &error);
  const qint64 inferenceMs = elapsed.elapsed();
  if (tensor.values.isEmpty())
    return fail(runFailureCode(error));
  if (cancelled && cancelled()) return fail("cancelled");
  if (tensor.shape.size() != 4 || tensor.shape[0] != 1 || tensor.shape[1] != checked.classCount ||
      (tensor.shape[2] > 0 && tensor.shape[2] != checked.rows) ||
      (tensor.shape[3] > 0 && tensor.shape[3] != checked.columns) ||
      tensor.values.size() != qsizetype(checked.classCount) * pixels)
    return fail("unit_model_output_shape_mismatch");
  TileClassGrid grid;
  if (checked.outputSemantics == QLatin1String("probabilities")) {
    if (!probabilityGrid(tensor.values, checked.classCount, checked.rows, checked.columns, valid, &grid, &error))
      return fail(error);
  } else {
    softmaxGrid(tensor.values, checked.classCount, checked.rows, checked.columns, valid, &grid,
                QStringLiteral("logits"));
  }
  if (cancelled && cancelled()) return fail("cancelled");
  QJsonObject histogram;
  for (qsizetype i = 0; i < pixels; ++i)
    if (grid.argmax[i] != 255)
      histogram.insert(QString::number(int(grid.argmax[i])), histogram.value(QString::number(int(grid.argmax[i]))).toInt() + 1);
  double gt[6] = {checked.extent.left(), checked.extent.width() / checked.columns, 0,
                  checked.extent.bottom(), 0, -checked.extent.height() / checked.rows};
  const auto provenance = QJsonObject{{"schema", "paleo.unit-onnx-provenance.v1"},
      {"unit", checked.unit}, {"unitKey", checked.unitKey},
      {"sourceVersionId", checked.seismicVersionId}, {"sourceSha256", checked.seismicSha256},
      {"model", checked.model}, {"modelSha256Pinned", checked.modelSha256},
      {"modelSha256Actual", meta.sha256}, {"inputSignature", checked.inputSignature},
      {"outputSemantics", checked.outputSemantics}, {"classes", checked.classCount},
      {"classEncoding", "declared_channel_index"}, {"featureSchemaHash", checked.featureSchemaHash},
      {"rows", checked.rows}, {"columns", checked.columns}, {"validCells", checked.validCells},
      {"inputAttribute", "layer_window_rms"}};
  const auto outputPath = QDir(privateDir).absoluteFilePath("OUTPUT.tif");
  if (!writeByteRaster(outputPath, checked.columns, checked.rows, gt, checked.crs, grid.argmax, provenance, error))
    return fail(error);
  if (cancelled && cancelled()) { QFile::remove(outputPath); return fail("cancelled"); }
  const auto diagnosticsJson = QJsonObject{{"schema", "paleo.unit-onnx-diagnostics.v1"},
      {"provenance", provenance}, {"samplesSha256", checked.samplesSha256()},
      {"classHistogram", histogram}, {"inferenceMs", double(inferenceMs)},
      {"warmupMs", double(meta.warmupMs)}, {"warmupError", meta.warmupError},
      {"budgets", QJsonObject{{"maxWindowValues", 1048576}, {"maxOutputElements", double(kMaxOnnxOutputElements)},
                              {"planJsonBytes", double(QJsonDocument(checked.toJson()).toJson(QJsonDocument::Compact).size())}}},
      {"sourceFingerprint", QJsonObject{{"size", double(checked.seismicSize)}, {"mtimeMs", double(checked.seismicMtimeMs)}}},
      {"inputFeatureContract", unitOnnxInputFeatureContract()},
      {"note", "class codes are model channel indexes only; no geological label mapping is implied"}};
  const auto diagnosticsPath = QDir(privateDir).absoluteFilePath("DIAGNOSTICS.json");
  if (QFileInfo::exists(diagnosticsPath)) return fail("private_output_already_exists");
  QFile diagnosticsFile(diagnosticsPath);
  if (!diagnosticsFile.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    return fail("unit_diagnostics_write_failed");
  const auto diagnosticsBytes = QJsonDocument(diagnosticsJson).toJson(QJsonDocument::Compact);
  if (diagnosticsFile.write(diagnosticsBytes) != diagnosticsBytes.size() || !diagnosticsFile.flush()) {
    diagnosticsFile.close();
    QFile::remove(diagnosticsPath);
    return fail("unit_diagnostics_write_failed");
  }
  diagnosticsFile.close();
  AlgorithmOutputArtifact output;
  output.portId = "OUTPUT";
  output.absolutePath = outputPath;
  output.suggestedAssetType = "aifacies";
  output.suggestedDisplayName = QStringLiteral("%1 unit onnx %2").arg(checked.unitKey, checked.model);
  output.extra = provenance.toVariantMap();
  result.artifacts.append(output);
  AlgorithmOutputArtifact diagnosticsArtifact;
  diagnosticsArtifact.portId = "DIAGNOSTICS";
  diagnosticsArtifact.absolutePath = diagnosticsPath;
  diagnosticsArtifact.extra = QVariantMap{{"schema", QStringLiteral("paleo.unit-onnx-diagnostics.v1")}};
  // 宿主 validateDiagnostics 只保留 ok 的私有诊断（Python 诊断同约定）；
  // 失败路径不产出诊断文件，fail() 一并清空出参。
  diagnostics.ok = true;
  diagnostics.artifacts.append(diagnosticsArtifact);
  summary = {{"rows", checked.rows}, {"columns", checked.columns}, {"classes", checked.classCount},
             {"validCells", checked.validCells}, {"unitKey", checked.unitKey},
             {"sourceVersionId", checked.seismicVersionId}, {"modelSha256", checked.modelSha256},
             {"featureSchemaHash", checked.featureSchemaHash}, {"classHistogram", histogram},
             {"inferenceMs", double(inferenceMs)}};
  result.ok = true;
  return result;
#endif
}
} // namespace paleo
