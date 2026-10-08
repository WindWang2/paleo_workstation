// 层：组装根
#pragma once
#include <QString>

class PaleoOnnxService;

// app/onnxwiring — ONNX 模型注册表装配（方向 83 装配根审视的最小落地，
// aiwiring 同构：纯函数、入参显式、不动 AppContext 类接口）。
// 工程打开时把 <工程目录>/models 钉为模型根并按注册表门控可见性/加载；
// 未装模型静默降级，坏 manifest/坏模型逐条进消息日志（不报错轰炸）。
// 无 ORT 构建为空实现——调用方无需自行守卫。
namespace paleo::app {

void installOnnxModelsOnOpen(PaleoOnnxService *onnxSvc, const QString &projectDir);

} // namespace paleo::app
