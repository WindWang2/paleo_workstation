// 层：组装根
#include "unitonnxwiring.h"
#include "services/unitonnxcapabilityspec.h"
#include "workflow/aichattoolrunner.h" // findChild<PaleoUnitOnnxBinding *> 需要完整类型
#include "workflow/capabilityhost.h"
#include "workflow/unitonnxservice.h"
#include <QPointer>
#include <catalog/datacatalog.h>
#include <ai/onnxpredictionservice.h>

// 绑定状态挂在 runner 名下（TileOperationService::configure 的 changed 守卫
// 同款语义）：onnx/catalog/host 三者都没变时不重注册——registerExecutionAdapter
// 对已册 id 的重注册会翻转宿主全局准备失效标志，误伤在途 prepare。
// 类名带 Paleo 前缀且位于文件作用域：moc 生成的 staticMetaObject 定义在
// 包含 .moc 的文件作用域，不放进匿名命名空间。
class PaleoUnitOnnxBinding final : public QObject
{
  Q_OBJECT
public:
  using QObject::QObject;
  PaleoOnnxService *onnx = nullptr;
  QPointer<DataCatalog> catalog;
  QPointer<paleo::capability::CapabilityHost> host;
};

void installUnitOnnxCapability(AiChatToolRunner *runner, PaleoOnnxService *onnx,
                               DataCatalog *catalog, paleo::capability::CapabilityHost *host) {
  if (!runner || !host) return;
  auto *binding = runner->findChild<PaleoUnitOnnxBinding *>(QStringLiteral("unit-onnx-binding"),
                                                            Qt::FindDirectChildrenOnly);
  if (!binding) {
    binding = new PaleoUnitOnnxBinding(runner);
    binding->setObjectName(QStringLiteral("unit-onnx-binding"));
  }
  if (binding->onnx == onnx && binding->catalog == catalog && binding->host == host) return;
  binding->onnx = onnx;
  binding->catalog = catalog;
  binding->host = host;
  host->registerExecutionAdapter(QLatin1String(paleo::kUnitOnnxCapabilityId),
                                 paleo::unitOnnxExecutionAdapter(onnx, QPointer<DataCatalog>(catalog)));
}

#include "unitonnxwiring.moc"
