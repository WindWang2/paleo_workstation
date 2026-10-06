// 层：组装根
#include "aiwiring.h"

#include "../ai/remotepredictrouter.h"
#include "../workflow/mappingworkbench.h"
#if PALEO_HAVE_ORT
#include "../ai/onnxlocalpredictor.h"
#endif

// app/ — AI 装配实现（方向51）。
//
// 唯一的远端预测装配点：产品（appcontext）与测试（tst_aiwiring 的装配断言，
// 以及 tst_mappingworkbench 的两条用例）都走这两个函数，因此「装配出来的是
// router 而不是替身」是一条可执行的断言，而不是靠读代码的人自觉。
RemotePredictionAssembly assembleRemotePrediction(
    const RemotePredictConfig &config, PaleoOnnxService *onnx,
    const QString &localModel, QObject *parent) {
  RemotePredictionAssembly out;
  out.config = config;
  out.statusHint = config.statusHint();

  HttpRemoteTransport *transport = nullptr;
  if (config.usable()) {
    transport = new HttpRemoteTransport(config.baseUrl);
    out.transportBound = true;
  }
  auto *router = new RemotePredictionRouter(transport, parent);
  if (transport)
    transport->setParent(router); // 生命周期挂在 router 上

  const QString model = localModel.isEmpty() ? config.model : localModel;
#if PALEO_HAVE_ORT
  if (onnx && !model.isEmpty())
    router->setLocalFallback(onnx, model); // 本地 ORT 降级（有 samples 才可用）
#else
  Q_UNUSED(onnx)
  Q_UNUSED(model)
#endif
  out.router = router;
  out.service = router;
  return out;
}

RemotePredictionAssembly installRemotePrediction(MappingWorkbench *bench,
                                                 const RemotePredictConfig &config,
                                                 PaleoOnnxService *onnx,
                                                 const QString &localModel,
                                                 QObject *parent) {
  const RemotePredictionAssembly assembled =
    assembleRemotePrediction(config, onnx, localModel, parent);
  if (bench) {
    bench->setPredictionService(assembled.service);
    bench->setPredictionStatusHint(assembled.statusHint);
  }
  return assembled;
}
