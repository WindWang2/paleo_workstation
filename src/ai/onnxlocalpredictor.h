// 层：功能
#pragma once
#include "remotepredictrouter.h"
#include <QString>

class PaleoOnnxService;

// ORT 本地降级引擎（方向51：由 RemotePredictionRouter::runLocalFallback 析出）。
//
// router 不再知道 ONNX，只按 LocalPredictor 接口调用本类；「 requests must
// carry samples，没有数据就不造假」这条红线随代码一起搬过来，语义与
// goal/ai-geological-assist 时期的实线完全一致（见 tst_remotepredictrouter）。
//
// 依赖 PaleoOnnxService 的 ORT-free 头：编译不需要 ONNX SDK；但本 TU 只在
// PALEO_HAVE_ORT 的构建里进 CMake（链接期才需要 ORT 符号）。
class OnnxLocalPredictor : public LocalPredictor {
public:
  OnnxLocalPredictor(PaleoOnnxService *onnx, const QString &model)
    : m_onnx(onnx), m_model(model) {}
  ~OnnxLocalPredictor() override = default;

  bool isConfigured() const override;
  QString engineId() const override;
  bool predict(const RemotePredictionRequest &request, QVector<int> &cells,
               QString *error) override;

private:
  PaleoOnnxService *m_onnx = nullptr;
  QString m_model;
};
