// 层：数据
#include "wellavailability.h"
#include "datacatalog.h"
#include <paleo_data/service.h>
#include <QObject>

bool readWellDataAvailability(paleo::data::DataService &service,
                             QHash<qlonglong,WellDataAvailability> &out, QString *error) {
  out.clear(); if(error)error->clear(); std::string detail;
  const auto fail=[&]{out.clear();if(error)*error=QString::fromUtf8(detail);return false;};
  std::vector<paleo::data::WellSummary> wells;
  if(!service.listWells(wells,detail))return fail();
  for(const auto &w:wells)out[w.id].timeDepth=w.timeDepthTables>0;
  for(const auto *kind:{"curves","intervals","markers"}) {
    std::vector<paleo::data::DataEntry> rows;
    if(!service.listData(kind,0,rows,detail))return fail();
    for(const auto &row:rows)if(row.wellId>0&&row.count>0) {
      out[row.wellId].tracks=true;
      if(QString::fromLatin1(kind)==QLatin1String("curves"))out[row.wellId].curves=true;
    }
  }
  for(auto it=out.begin();it!=out.end();++it)
    it->label=it->tracks ? (it->curves?QObject::tr("井道资料 · 含曲线"):QObject::tr("井道资料 · 分层/标志层"))
        :it->timeDepth?QObject::tr("时深资料 · 无井道内容"):QObject::tr("无井道内容 · 井位或辅助资料");
  return true;
}
bool wellSupportsAnalysis(const DataCatalog *catalog,const CatalogEntity &well,bool requireCurves) {
  if(well.extra.value("areaModelWellId").toLongLong()>0)
    return well.extra.value(requireCurves?"wellHasCurveData":"wellHasTrackData").toBool();
  if(!catalog)return false;
  for(const auto &link:catalog->linksForEntity(well.id)) {
    if(link.unresolved||catalog->currentVersion(link.assetId).id.isEmpty())continue;
    if(link.role==QLatin1String("well_log"))return true;
    if(!requireCurves&&QStringList{"tops","cuttings","interpretation"}.contains(link.role))return true;
  }
  return false;
}
