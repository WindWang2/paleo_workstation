// 层：功能
// workflow/algorithmadapters.h — algo-atom P2：执行适配器。
// ProcessingAlgorithmAdapter 包装 QgisProcessingService::run → AlgorithmRunResult；
// NativeCppAdapter：geostat:ordinary_kriging 已实装；其余纯核 TODO。
// 设计：docs/designs/algorithm-atom-contract.md §6 P2。
#pragma once

#include "../algorithms/geostat/kriging.h"
#include "../qgis/qgisprocessingservice.h"
#include "../services/algorithmspec.h"

#include <qgscoordinatereferencesystem.h>

#include <QString>
#include <QVariantMap>
#include <array>
#include <vector>

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

  private:
    QgisProcessingService *m_proc = nullptr;
};

struct OrdinaryKrigingInputs
{
  std::vector<paleo::geostat::Sample> samples;
  paleo::geostat::GridSpec grid;
  paleo::geostat::VariogramModel model;
  paleo::geostat::KrigingParams params;
  paleo::geostat::Control control;
  QString estimatePath;
  QString variancePath; // 空 = 不写方差
  std::array<double, 6> geoTransform{ { 0, 1, 0, 0, 0, -1 } };
  QgsCoordinateReferenceSystem crs;
  QString canonicalCrsWkt;
};

class NativeCppAdapter
{
  public:
    // QVariantMap 入口：OK 请用 runOrdinaryKriging；其它 id 返回未实现。
    AlgorithmRunResult run( const AlgorithmSpec &spec, const QVariantMap &paramValues,
                            QString *error = nullptr );

    AlgorithmRunResult runOrdinaryKriging( const OrdinaryKrigingInputs &in,
                                           QString *error = nullptr );
};

} // namespace paleo
