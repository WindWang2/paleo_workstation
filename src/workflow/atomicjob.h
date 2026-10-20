// 层：功能
// workflow/atomicjob.h — algo-atom：薄共享助手（非巨型模板框架）。
// JobRunner 已承载异步 prepare/compute/publish 调度；本头只抽「Processing
// Adapter.run → publishArtifacts」同步胶合，供 fusion / 简单同步路径复用，
// 避免每处复制 adapter+publish 五六行。不重写 ConstraintJob / PredictionJob。
#pragma once

#include "algorithmadapters.h"
#include "algorithmpublish.h"

#include <QString>
#include <QVariantMap>

namespace paleo
{

struct AtomicProcessingDest
{
  DerivedAssetRegistrar *registrar = nullptr;
  DerivedStaging staging;
  QStringList parentVersionIds;
  QString sourceUri;
  QVariantMap extra;
  const DerivedFileFingerprint *fingerprint = nullptr;
};

// Spec + params → ProcessingAlgorithmAdapter::run → publishArtifacts。
// 失败时 *error 已填；成功返回 true。
bool runProcessingAndPublish( ProcessingAlgorithmAdapter &adapter, const AlgorithmSpec &spec,
                              const QVariantMap &params, const AtomicProcessingDest &dest,
                              QString *error = nullptr,
                              const QgisProcessingService::ProcessingHooks &hooks = {} );

// 便捷：按 algorithmId 查 Spec（未知 id 时用 processing 回退壳）。
bool runProcessingAndPublishById( ProcessingAlgorithmAdapter &adapter, const QString &algorithmId,
                                  const QVariantMap &params, const AtomicProcessingDest &dest,
                                  QString *error = nullptr,
                                  const QgisProcessingService::ProcessingHooks &hooks = {} );

} // namespace paleo
