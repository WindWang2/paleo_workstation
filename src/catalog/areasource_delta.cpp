// 层：数据
#include "areasource_delta_internal.h"
#include "areawellprojection.h"
#include "domain/documentsource.h"
#include <paleo_data/service.h>
#include <QDir>
#include <QFileInfo>

namespace {
using paleo::data::ModelChangeHead;
using paleo::data::ModelChangeItem;
constexpr auto kProjection="area-sources";
bool reject(QString *error,const QString &message) {if(error)*error=message;return false;}
bool same(const ModelChangeHead &a,const ModelChangeHead &b) {
  return a.available && b.available && a.storeId==b.storeId && a.generation==b.generation && a.sequence==b.sequence;
}
CatalogProjectionCheckpoint checkpoint(const ModelChangeHead &head,qint64 sequence) {
  return {QString::fromUtf8(head.storeId),QString::fromUtf8(head.generation),sequence};
}
bool bootstrap(DataCatalog &catalog,paleo::data::DataService &reader,const ModelChangeHead &head,
               AreaSourceProjectionStats *stats,QString *error) {
  const auto previous=catalog.projectionCheckpoint(kProjection);
  if(previous.valid() && previous.storeId!=QString::fromUtf8(head.storeId))
    return reject(error,QStringLiteral("ModelStoreChanged：需要明确迁移井和来源身份映射"));
  DataCatalog::BatchSave batch(&catalog);
  if(!registerAreaWells(catalog,error)){batch.abort();return false;}
  if(!bootstrapAreaSources(catalog,QString::fromUtf8(head.generation),true,error)){batch.abort();return false;}
  std::string detail;ModelChangeHead after;
  if(!reader.readModelChangeHead(after,detail) || !same(head,after)) {
    batch.abort();return reject(error,QStringLiteral("CatchingUp：建立索引期间模型发生变化，请重试"));
  }
  if(!catalog.setProjectionCheckpoint(kProjection,checkpoint(head,head.sequence),error)){batch.abort();return false;}
  if(stats)++stats->bootstrapScans;
  return batch.flush(error);
}
bool readBatch(paleo::data::DataService &reader,const ModelChangeHead &head,qint64 after,
               qint64 &sequence,std::vector<ModelChangeItem> &items,AreaSourceProjectionStats *stats,QString *error) {
  paleo::data::ModelChangeQuery query;query.expectedGeneration=head.generation;query.afterSequence=after;
  query.maxBytes=16*1024*1024;query.maxItems=200;
  std::string detail;std::size_t bytes=0;
  do {
    paleo::data::ModelChangePage page;
    if(!reader.readModelChanges(query,page,detail))return reject(error,QString::fromUtf8(detail));
    if(!page.sequence || page.sequence>head.sequence || page.head.storeId!=head.storeId)
      return reject(error,QStringLiteral("ModelProjectionRebuildRequired"));
    if(query.sequence && page.sequence!=query.sequence)return reject(error,QStringLiteral("模型日志分页批次变化"));
    sequence=page.sequence;if(stats){++stats->pages;stats->items+=int(page.items.size());}
    for(auto &item:page.items) {
      if(item.metadataJson.size()>64*1024*1024-bytes || items.size()>=50000)
        return reject(error,QStringLiteral("ModelProjectionBudgetExceeded：单提交元数据超过64 MiB/50000项"));
      bytes+=item.metadataJson.size();items.push_back(std::move(item));
    }
    if(page.complete)return true;
    if(page.nextOrdinal<=query.afterOrdinal)return reject(error,QStringLiteral("模型日志游标未推进"));
    query.sequence=page.sequence;query.afterOrdinal=page.nextOrdinal;
  } while(true);
}
}
void paleo::area_projection::inheritRelocatedSourceBindings(const CatalogVersion &previous,
                                                            CatalogVersion &replacement) {
  const auto sourceId=replacement.extra.value("areaModelSourceId").toLongLong();
  if (sourceId<=0 || previous.extra.value("areaModelSourceId").toLongLong()!=sourceId)
    return;
  const auto fingerprint=paleo::document::SourceFingerprint::fromStored(replacement.sha256);
  const auto bindings=previous.extra.constFind(QStringLiteral("documentSourceBindings"));
  if (fingerprint.valid() && fingerprint==paleo::document::SourceFingerprint::fromStored(previous.sha256) &&
      bindings!=previous.extra.cend())
    replacement.extra.insert(QStringLiteral("documentSourceBindings"),bindings.value());
}

bool applyAreaSourceChangeBatch(DataCatalog &catalog,const ModelChangeHead &head,qint64 sequence,
    const std::vector<ModelChangeItem> &items,QString *error) {
  if(error)error->clear();
  if(catalog.isStaging())return reject(error,QStringLiteral("增量投影需要持久目录事务"));
  if(!head.available || sequence<1 || sequence>head.sequence || items.empty())return reject(error,QStringLiteral("模型提交水位无效"));
  const auto old=catalog.projectionCheckpoint(kProjection);
  if(old.valid() && (old.storeId!=QString::fromUtf8(head.storeId) || old.generation!=QString::fromUtf8(head.generation)))
    return reject(error,QStringLiteral("ModelProjectionRebuildRequired"));
  if(old.valid() && old.sequence>=sequence)return true;
  auto cache=paleo::area_projection::cacheFor(catalog,QString::fromUtf8(head.generation),nullptr);
  DataCatalog::BatchSave batch(&catalog);
  for(const auto &item:items) if(item.kind=="well" && !paleo::area_projection::applyItem(catalog,*cache,item,sequence,error)) {
    batch.abort();return false;
  }
  for(const auto &item:items) if(!paleo::area_projection::prepareItem(catalog,*cache,item,error)) {batch.abort();return false;}
  for(const auto &item:items) if(item.kind!="well" && !paleo::area_projection::applyItem(catalog,*cache,item,sequence,error)) {
    batch.abort();return false;
  }
  if(!catalog.setProjectionCheckpoint(kProjection,checkpoint(head,sequence),error)){batch.abort();return false;}
  if(!batch.flush(error))return false;
  cache->mutationSequence=catalog.mutationSeq();return true;
}
bool registerAreaSourcesWithStats(DataCatalog &catalog,QString *error,AreaSourceProjectionStats *stats) {
  if(error)error->clear();if(stats)*stats={};
  const auto root=QDir(catalog.projectDir()).absoluteFilePath("artifacts/metadata/area-model");
  if(!QFileInfo::exists(root+"/data.sqlite"))return true;
  paleo::data::DataService reader;std::string detail;ModelChangeHead head;
  if(!reader.openProjectReadOnly(root.toUtf8().toStdString(),detail) || !reader.readModelChangeHead(head,detail))
    return reject(error,QString::fromUtf8(detail));
  if(!head.available) {if(stats){stats->legacy=true;++stats->bootstrapScans;}return bootstrapAreaSources(catalog,{},true,error);}
  auto current=catalog.projectionCheckpoint(kProjection);
  if(!current.valid() || current.generation!=QString::fromUtf8(head.generation) || current.storeId!=QString::fromUtf8(head.storeId))
    return bootstrap(catalog,reader,head,stats,error);
  if(current.sequence>head.sequence)return reject(error,QStringLiteral("ModelProjectionRebuildRequired"));
  if(current.sequence==head.sequence){if(stats)stats->unchanged=true;return true;}
  paleo::area_projection::cacheFor(catalog,QString::fromUtf8(head.generation),stats);
  while(current.sequence<head.sequence) {
    std::vector<ModelChangeItem> items;qint64 sequence=0;
    if(!readBatch(reader,head,current.sequence,sequence,items,stats,error))return false;
    if(!applyAreaSourceChangeBatch(catalog,head,sequence,items,error)) {
      if(error && *error==QLatin1String("ModelProjectionRebuildRequired"))return bootstrap(catalog,reader,head,stats,error);
      return false;
    }
    if(stats)++stats->batches;current=catalog.projectionCheckpoint(kProjection);
  }
  return true;
}
bool registerAreaSources(DataCatalog &catalog,QString *error) {return registerAreaSourcesWithStats(catalog,error,nullptr);}
