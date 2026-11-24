// 层：组装根
#pragma once
#include <QPointer>

class AiChatToolRunner;
class PaleoOnnxService;
class DataCatalog;
namespace paleo::capability { class CapabilityHost; }

// I2：把 unit.onnx_predict 的 per-capability 执行适配器装进宿主。每次工程
// 重绑（bindChatToolRunner）都会以当期 onnx/catalog 重注册——旧适配器连同
// 其未完成准备一并作废（registerExecutionAdapter 的既有语义），不触碰
// tile/Python 的全局恢复回调。
void installUnitOnnxCapability(AiChatToolRunner *runner, PaleoOnnxService *onnx,
                               DataCatalog *catalog, paleo::capability::CapabilityHost *host);
