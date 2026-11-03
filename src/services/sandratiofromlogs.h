// 层：数据
#pragma once
#include "domain/grsandratio.h"
#include <QVariantMap>
#include <QStringList>
#include <functional>
class DataCatalog;
namespace paleo::singlefactor {
struct WellGrSandResult {
  GrSandEstimate estimate;
  QVariantMap provenance;
  QString source;
  QStringList paths,versionIds;
};
// 全部曲线组均检查，按当前层段有效覆盖选择 GR；不按全井点数选组。
WellGrSandResult sandRatioFromLogs(const DataCatalog *catalog,const QString &project,
    const QString &wellId,qlonglong nativeId,double top,double bottom,
    const GrSandOptions &options={},const std::function<bool()> &cancelled={});
}
