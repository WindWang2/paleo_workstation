// 层：功能
// workflow/algorithmadapters.h — algo-atom P2：执行适配器。
// ProcessingAlgorithmAdapter 包装 QgisProcessingService::run → AlgorithmRunResult；
// NativeCppAdapter 见 nativecppadapter.h（+ nativeinputs_*.h）。
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

    AlgorithmRunResult run( const AlgorithmSpec &spec, const QVariantMap &paramValues,
                            QString *error = nullptr,
                            const QgisProcessingService::ProcessingHooks &hooks = {} );

    AlgorithmRunResult runById( const QString &algorithmId, const QVariantMap &paramValues,
                                QString *error = nullptr,
                                const QgisProcessingService::ProcessingHooks &hooks = {} );

    // 公开入口：未登记或版本不符直接拒绝，不走 runById 的旧 Processing fallback。
    AlgorithmRunResult runRegisteredById( const QString &algorithmId, int contractVersion,
                                          const QVariantMap &paramValues, QString *error = nullptr,
                                          const QgisProcessingService::ProcessingHooks &hooks = {} );

  private:
    QgisProcessingService *m_proc = nullptr;
};

} // namespace paleo
