// 层：数据
#pragma once
#include <QHash>
#include <QString>
#include <vector>
namespace paleo::data { class DataService; struct WellSummary; }
class DataCatalog;
struct CatalogEntity;
struct WellDataAvailability {
  bool tracks=false, curves=false, timeDepth=false;
  QString label;
};
// Metadata only: nonempty stored curves/intervals/markers support well tracks;
// time-depth, location, photos and attachments alone do not.
bool readWellDataAvailability(paleo::data::DataService &service,
                             QHash<qlonglong,WellDataAvailability> &out, QString *error=nullptr,
                             const std::vector<paleo::data::WellSummary> *knownWells=nullptr);
bool wellSupportsAnalysis(const DataCatalog *catalog, const CatalogEntity &well, bool requireCurves=false);
