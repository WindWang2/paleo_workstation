// 层：组装根
#include "onnxwiring.h"

#if PALEO_HAVE_ORT
#include "../ai/modelregistry.h" // ModelRegistry/ModelRegistryScan（#145 门控）
#include "../ai/onnxpredictionservice.h"
#include <qgis.h>
#include <qgsmessagelog.h>
#include <QDir>
#include <QObject>
#endif

namespace paleo::app {

#if PALEO_HAVE_ORT
void installOnnxModelsOnOpen(PaleoOnnxService *onnxSvc, const QString &projectDir,
                             const ModelRegistryScan *preparedModels)
{
  // onnx:* 模型按层位钉在 <工程目录>/models/*.onnx。
  if (!onnxSvc)
    return;
  const QString modelsDir =
      QDir(projectDir).filePath(QStringLiteral("models"));
  onnxSvc->setModelRoot(modelsDir);
  // 模型注册表如实扫描（范围5）：未装模型静默降级；manifest 在而
  // 坏/缺文件/指纹不符 → 消息日志逐条说明，不报错轰炸。
  // 后台准备已经扫过同一目录时直接用那份结果。
  const ModelRegistryScan registry =
      preparedModels ? *preparedModels : ModelRegistry::scan(modelsDir);
  // #145：扫描结果门控模型可见性与加载（只放行 status==Ok；加载时复核钉哈希）。
  onnxSvc->setModelRegistry(registry);
  if (!registry.manifestFound)
    QgsMessageLog::logMessage(
        QObject::tr("未装模型：%1 无 manifest.json——AI 辅助按无模型降级").arg(modelsDir),
        QStringLiteral("Paleo"));
  if (!registry.manifestError.isEmpty())
    QgsMessageLog::logMessage(registry.manifestError, QStringLiteral("Paleo"),
                              Qgis::Critical);
  for (const ModelRegistryEntry &e : registry.entries)
    if (e.status != ModelRegistryEntry::Status::Ok)
      QgsMessageLog::logMessage(
          QObject::tr("模型 %1: %2 (%3)").arg(e.name, ModelRegistry::statusLabel(e.status), e.detail),
          QStringLiteral("Paleo"), Qgis::Warning);
}
#else
void installOnnxModelsOnOpen(PaleoOnnxService *, const QString &,
                             const ModelRegistryScan *) {}
#endif

} // namespace paleo::app
