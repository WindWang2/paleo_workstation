// 层：组装根
#pragma once
#include "../ai/remotepredictconfig.h"
#include <QString>

class MappingWorkbench;
class PaleoOnnxService;
class RemotePredictionRouter;
class RemotePredictionService;

// app/ — AI 面装配（方向51：接管 mappingworkbench 原先自装的替身 Mock）。
//
// 规则：产品装配路径里不存在替身。本函数是远端预测的唯一装配入口：
//   · 端点可用 → HttpRemoteTransport + RemotePredictionRouter
//   · 端点未配置/禁用/非法 → 仍然返回 router，但不给它传输；router 收到
//     请求时如实失败（"未绑定远端传输"），UI 按 statusHint 明示
//     「远端预测未配置，走本地引擎」——不再安静跑 Mock 冒充远端推理。
//
// 装配本身要可测：入参全部显式（配置 + 本地引擎），结果与状态文本一起返回；
// 测试用同一函数断言「出来的是 router，不是替身」（见 tests/tst_mappingworkbench
// 的装配用例：依赖的最小装配面在 app 层，和 appcontext 调用的是同一个函数）。
struct RemotePredictionAssembly {
  RemotePredictionRouter *router = nullptr;   // 恒非空（parent 拥有）
  RemotePredictionService *service = nullptr; // == router（给 setPredictionService 用）
  RemotePredictConfig config;                 // 实际生效的配置
  QString statusHint;                         // 面向用户的状态（已翻译）
  bool transportBound = false;                // 是否真的挂上了 HTTP 传输
};

RemotePredictionAssembly assembleRemotePrediction(
    const RemotePredictConfig &config, PaleoOnnxService *onnx,
    const QString &localModel, QObject *parent);

// 装配 + 接到 MappingWorkbench（appcontext 走这条）。永远返回非空 router：
// 未配置时它由 router 自己如实失败。
RemotePredictionAssembly installRemotePrediction(MappingWorkbench *bench,
                                                 const RemotePredictConfig &config,
                                                 PaleoOnnxService *onnx,
                                                 const QString &localModel,
                                                 QObject *parent);

class AiChatController;
class AiAssistWorkflow;
class QgisLayerService;

// 方向61：聊天助手的工具执行器装配——把 controller 里的 runner 绑到
// AiAssistWorkflow 执行面 + 应用级数据上下文（活动层位 + 层位栅格取数）。
// traceFetch（道窗，须地震体服务）与 faciesInput（井曲线，须井缓存）暂不
// 绑定：对应工具被点名时执行器如实报「上下文未绑定」，不冒充成功
// （递延项见 TODOS.md）。appcontext 在启动与工程打开（AreaRules 可能换
// 工区钉值）各调一次。
void bindChatToolRunner(AiChatController *chat, AiAssistWorkflow *assist,
                        QgisLayerService *layers);
