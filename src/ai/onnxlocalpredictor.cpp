// 层：功能
#include "onnxlocalpredictor.h"

#include "onnxpredictionservice.h"
#include "tileinference.h"

#include <QVector>
#include <memory>

// ORT 本地降级引擎实现 + RemotePredictionRouter 的 ORT 便捷绑定。
// 错误文案与 goal/ai-geological-assist 一致（测试按子串断言），不得改写。

QString OnnxLocalPredictor::engineId() const {
  return m_model.isEmpty() ? QString()
                           : QStringLiteral("local-ort:%1").arg(m_model);
}

bool OnnxLocalPredictor::isConfigured() const {
  return m_onnx != nullptr && !m_model.isEmpty();
}

bool OnnxLocalPredictor::predict(const RemotePredictionRequest &request,
                                 QVector<int> &cells, QString *error) {
  const auto fail = [error](const QString &msg) {
    if (error)
      *error = msg;
    return false;
  };
  if (!m_onnx || m_model.isEmpty())
    return fail(QObject::tr("无本地 ORT 降级（未绑定模型）"));
  if (request.samples.size() != qsizetype(request.columns) * request.rows)
    return fail(QObject::tr("请求未携带网格数据——本地降级不造假（samples=%1，网格 %2×%3）")
                  .arg(request.samples.size())
                  .arg(request.columns)
                  .arg(request.rows));
  OnnxModelMeta meta; // #144：meta 随加载取回，推理绑定模型名
  if (m_onnx->loadModelMeta(m_model, &meta, error) != OnnxLoadStatus::Ok)
    return fail(error && !error->isEmpty() ? *error
                                           : QObject::tr("本地模型加载失败"));

  if (meta.inputShape.size() != 4 || meta.inputShape[0] > 1 ||
      meta.inputShape[1] > 1)
    return fail(QObject::tr("本地模型输入须为 [1,1,H,W]，实际 %1")
                  .arg(meta.inputSignature));

  QVector<float> samples = request.samples;
  QVector<bool> valid;
  sanitizeModelInput(samples, &valid); // #143：非有限样置 0 喂模型，输出端置 255

  QString runErr;
  const OnnxTensor out =
    m_onnx->runTensorOn(m_model, meta.inputName, samples,
                        {1, 1, request.rows, request.columns}, &runErr);
  if (!runErr.isEmpty())
    return fail(runErr);
  if (out.shape.size() != 4 || out.shape[0] != 1 ||
      out.shape[2] != request.rows || out.shape[3] != request.columns)
    return fail(QObject::tr("本地模型输出形状与请求网格不符"));
  if (out.shape[1] < 1 || out.shape[1] > kMaxTileClasses)
    return fail(QObject::tr("本地模型输出类数 %1 越界 [1, %2]")
                  .arg(out.shape[1])
                  .arg(kMaxTileClasses));

  TileClassGrid grid;
  softmaxGrid(out.values, int(out.shape[1]), request.rows, request.columns,
              valid, &grid);
  cells = QVector<int>(grid.argmax.size(), 0);
  for (qsizetype i = 0; i < grid.argmax.size(); ++i)
    cells[i] = int(grid.argmax[i]); // 255=nodata 语义与 tile 产品一致
  return true;
}

void RemotePredictionRouter::setLocalFallback(PaleoOnnxService *onnx,
                                              const QString &model) {
  // 所有权：由 router 持有（unique_ptr），指针视图记在 m_local。重复绑定时
  // 旧引擎先被替换掉再析构，不会留下悬垂指针。
  m_ownedLocal = std::make_unique<OnnxLocalPredictor>(onnx, model);
  m_local = m_ownedLocal.get();
}
