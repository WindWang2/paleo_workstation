// 层：功能
#pragma once
#include "capabilityhost.h"
#include <QPointer>

class PaleoOnnxService;
class DataCatalog;

namespace paleo {

// unit.onnx_predict 的 per-capability 宿主适配器（CapabilityHost::
// registerExecutionAdapter 注册；不触碰 tile/Python 的全局恢复回调）。
// · capture：prepare 阶段在 owner 线程做注册表/catalog 轻查询，把重活
//   （ORT 加载、单元冻结、64×64 RMS 物化）交给任务线程 worker；compute-only
//   阶段只组装继承源操作的冻结意图，不接受重述输入；
// · restoreWorker / restoreInputValidator / validateFrozen：进程重启后的
//   恢复入口——compute 校验冻结计划与 catalog 版本身份；publish 只做结构
//   校验，不要求当前 ORT/模型仍可用（已认证输出的再发布不需要新推理）。
// onnx/catalog 由装配根在每次重绑时注入；适配器只持值与 QPointer，不持
// 跨线程 QObject 裸指针语义（推理服务自身线程安全）。
capability::CapabilityExecutionAdapter unitOnnxExecutionAdapter(PaleoOnnxService *onnx,
                                                                const QPointer<DataCatalog> &catalog);

} // namespace paleo
