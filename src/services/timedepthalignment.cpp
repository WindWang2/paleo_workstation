// 层：数据
#include "timedepthalignment.h"
#include "catalog/datacatalog.h"
#include "io/areamodel.h"
#include <paleo_data/service.h>
#include <QSet>
#include <cmath>

namespace paleo {
QVariantMap timeDepthAlignmentOptions(DataCatalog *catalog,const QStringList &ids,QString *error){
  if(error)error->clear();if(!catalog||!catalog->isOpen())return {};
  data::DataService service;std::string detail;
  const auto fail=[&]{if(error)*error=QString::fromStdString(detail);return QVariantMap{};};
  if(!service.openProjectReadOnly(area_model::directory(catalog->projectDir()).toStdString(),detail))return fail();
  QSet<qlonglong> wells;for(const auto &id:ids)wells.insert(catalog->entityById(id).extra.value("areaModelWellId").toLongLong());
  std::vector<data::DataEntry> entries;std::vector<data::SourceIndexEntry> sources;
  if(!service.listData("time_depth",0,entries,detail)||!service.listSourceIndexEntries(sources,detail))return fail();
  QSet<qlonglong> included;QVariantList choices,volumes;
  for(const auto &entry:entries){
    if((!ids.isEmpty()&&!wells.contains(entry.wellId))||included.contains(entry.sourceId))continue;
    data::TimeDepthTable table;if(!service.readTimeDepth(entry.id,table,detail))return fail();
    included.insert(entry.sourceId);int relations=0,negative=0;double lo=INFINITY,hi=-INFINITY;
    for(const auto &other:entries)relations+=other.sourceId==entry.sourceId;
    for(const auto &point:table.depthTimePairs)if(std::isfinite(point.timeValue)){
      lo=std::min(lo,point.timeValue);hi=std::max(hi,point.timeValue);negative+=point.timeValue<0;
    }
    QString path;for(const auto &source:sources)if(source.id==entry.sourceId){path=QString::fromStdString(source.path);break;}
    choices<<QVariantMap{{"id",entry.id},{"name",QString::fromStdString(entry.name)},{"path",path},{"relations",relations},
      {"unit",QString::fromStdString(table.timeUnit)},{"type",QString::fromStdString(table.timeType)},
      {"reference",QString::fromStdString(table.timeReference)},{"negate",table.timeSign=="negated"},
      {"samples",int(table.depthTimePairs.size())},{"negative",negative},{"minTime",lo},{"maxTime",hi}};
  }
  if(!service.listData("volumes",0,entries,detail))return fail();
  for(const auto &entry:entries){data::SeismicVolume volume;if(!service.readVolume(entry.id,volume,detail))return fail();
    volumes<<QVariantMap{{"id",entry.id},{"name",QString::fromStdString(volume.volumeName)},
      {"unit",QString::fromStdString(volume.sampling.timeUnit)},{"type",QString::fromStdString(volume.sampling.timeType)},
      {"reference",QString::fromStdString(volume.sampling.timeReference)}};
  }
  return {{"sources",choices},{"volumes",volumes},{"writable",!catalog->refusesWrites()}};
}
bool saveTimeDepthAlignment(DataCatalog *catalog,const QVariantMap &selection,QString *error){
  if(error)error->clear();
  if(!catalog||!catalog->isOpen()||catalog->refusesWrites()){
    if(error)*error=QStringLiteral("工程未打开或为只读，不能保存时深口径");return false;
  }
  data::DataService service;std::string detail;
  if(!service.openProject(area_model::directory(catalog->projectDir()).toStdString(),detail)){
    if(error)*error=QString::fromStdString(detail);return false;
  }
  std::vector<long long> ids;for(const auto &id:selection.value("sources").toList())ids.push_back(id.toLongLong());
  const bool ok=service.confirmTimeDepthAlignment(ids,selection.value("unit").toString().toStdString(),
    selection.value("type").toString().toStdString(),selection.value("reference").toString().trimmed().toStdString(),
    selection.value("negate").toBool(),selection.value("volume").toLongLong(),detail);
  if(!ok&&error)*error=QString::fromStdString(detail);return ok;
}
}
