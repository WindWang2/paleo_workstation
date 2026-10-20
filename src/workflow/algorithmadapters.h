// 层：功能
// workflow/algorithmadapters.h — algo-atom P2：执行适配器。
// ProcessingAlgorithmAdapter 包装 QgisProcessingService::run → AlgorithmRunResult；
// NativeCppAdapter 为纯核车道预留桩（P2 完整落地前返回未实现）。
// 算法核 / catalog 仍分离：适配器只认 Spec + 参数 map，发布走 publishArtifacts。
// 设计：docs/designs/algorithm-atom-contract.md §6 P2。
#pragma once

#include "../qgis/qgisprocessingservice.h"
#include "../services/algorithmspec.h"

#include <QString>
#include <QVariantMap>

namespace paleo
{

class ProcessingAlgorithmAdapter
{
  public:
    explicit ProcessingAlgorithmAdapter( QgisProcessingService *proc );

    // 按 Spec 同步跑 Processing；成功 → artifacts 含目的地端口路径，diagnostics
    // 透传其余结果键（含 string 诊断输出）。失败 → ok=false，error 可读。
    AlgorithmRunResult run( const AlgorithmSpec &spec, const QVariantMap &paramValues,
                            QString *error = nullptr,
                            const QgisProcessingService::ProcessingHooks &hooks = {} );

    AlgorithmRunResult runById( const QString &algorithmId, const QVariantMap &paramValues,
                                QString *error = nullptr,
                                const QgisProcessingService::ProcessingHooks &hooks = {} );

  private:
    QgisProcessingService *m_proc = nullptr;
};

// 纯核（geostat / faciesmapping …）适配器桩——P2 完整实现前不可用。
class NativeCppAdapter
{
  public:
    AlgorithmRunResult run( const AlgorithmSpec &spec, const QVariantMap &paramValues,
                            QString *error = nullptr );
};

} // namespace paleo
