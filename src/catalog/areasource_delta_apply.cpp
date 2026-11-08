// 层：数据
#include "areasource_delta_internal.h"
#include "../domain/projectclassifier.h"
#include "../domain/shaequals.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <cmath>

namespace paleo::area_projection {
std::shared_ptr<Cache> cacheFor(DataCatalog &catalog,const QString &generation,AreaSourceProjectionStats *stats) {
  static QMutex mutex;
  static QHash<DataCatalog*,std::shared_ptr<Cache>> caches;
  QMutexLocker lock(&mutex);
  for(auto it=caches.begin();it!=caches.end();) {if(!it.value()->owner)it=caches.erase(it);else ++it;}
  auto &entry=caches[&catalog];
  if(entry && entry->owner==&catalog && entry->generation==generation && entry->path==catalog.projectDir() && entry->mutationSequence==catalog.mutationSeq())return entry;
  entry=std::make_shared<Cache>();entry->owner=&catalog;entry->generation=generation;entry->path=catalog.projectDir();
  for(const auto &version:catalog.versions()) {
    const auto id=version.extra.value("areaModelSourceId").toLongLong();
    if(id>0 && version.stage==QLatin1String("RAW") && version.extra.value("areaModelGeneration").toString()==generation)entry->sources.insert(id,version.id);
  }
  for(const auto &entity:catalog.storedEntities()) {
    if(entity.extra.value("areaModelGeneration").toString()!=generation)continue;
    if(entity.entityType==QLatin1String("well"))entry->wells.insert(entity.extra.value("areaModelWellId").toLongLong(),entity.id);
    if(entity.entityType==QLatin1String("seismic_survey"))entry->surveys.insert(entity.extra.value("areaModelVolumeId").toLongLong(),entity.id);
  }
  entry->mutationSequence=catalog.mutationSeq();if(stats)++stats->cacheBuilds;
  return entry;
}
namespace {
QString text(const std::string &value){return QString::fromUtf8(value);}
bool reject(QString *error,const QString &message){if(error)*error=message;return false;}
qint64 id(const QJsonValue &value) {bool ok=false;if(!value.isString())return -1;const auto result=value.toString().toLongLong(&ok);return ok?result:-1;}
QString role(const QString &kind) {
  static const QHash<QString,QString> roles{{"curves","well_log"},{"intervals","tops"},{"time_depth","time_depth"},
    {"trajectories","trajectory"},{"core_photos","core"},{"samples","lab_analysis"},{"attachments","reference"},{"markers","interpretation"}};
  return roles.value(kind);
}
ProjectClassification classification(const QJsonObject &source,const QJsonArray &objects) {
  QSet<QString> kinds;for(const auto &object:objects)kinds.insert(object.toObject().value("kind").toString());
  auto result=classifyProjectPath(source.value("path").toString());
  for(const auto &pair:QVector<QPair<QString,QString>>{{"volumes","seismic"},{"horizons","horizon"},{"curves","well_log"},
      {"intervals","well_stratification"},{"time_depth","time_depth"},{"trajectories","well_deviation"},{"core_photos","image_reference"}})
    if(kinds.contains(pair.first)){result.type=pair.second;break;}
  if(result.format.isEmpty())result.format=QFileInfo(source.value("path").toString()).suffix().toLower();
  return result;
}
bool ensureWell(DataCatalog &catalog,Cache &cache,qint64 modelId,const QString &name,QString *error) {
  if(modelId<=0)return reject(error,QStringLiteral("日志井标识无效"));
  if(cache.wells.contains(modelId))return true;
  CatalogEntity entity;entity.id=catalog.nextEntityId("well");entity.entityType="well";entity.name=name;
  entity.kb=qQNaN();entity.td=qQNaN();entity.hasSurface=false;entity.coordinateStatus="missing";
  entity.extra={{"areaModelWellId",modelId},{"areaModelGeneration",cache.generation}};
  if(!catalog.addEntity(entity,error))return false;
  cache.wells.insert(modelId,entity.id);return true;
}
bool addLink(DataCatalog &catalog,const CatalogVersion &version,const QString &entityId,const QString &roleName,QString *error) {
  if(entityId.isEmpty() || roleName.isEmpty())return true;
  bool primary=true;int ordinal=-1;
  for(const auto &link:catalog.linksForEntity(entityId)) {
    if(link.assetId==version.assetId && link.role==roleName && !link.unresolved)return true;
    if(link.role==roleName){primary=primary && !(link.isPrimary && !link.unresolved);ordinal=qMax(ordinal,link.ordinal);}
  }
  EntityAssetLink link;link.entityId=entityId;link.entityType=catalog.storedEntityById(entityId).entityType;
  link.assetId=version.assetId;link.role=roleName;link.isPrimary=primary;link.ordinal=ordinal+1;
  return catalog.addLink(link,error);
}
bool applyWell(DataCatalog &catalog,Cache &cache,const QJsonObject &image,bool deleted,QString *error) {
  const auto modelId=id(image.value("id"));
  if(modelId<=0)return reject(error,QStringLiteral("井日志缺少精确标识"));
  if(deleted) {
    if(!cache.wells.contains(modelId))return true;
    auto entity=catalog.storedEntityById(cache.wells.value(modelId));entity.extra["areaModelWellActive"]=false;
    return catalog.updateEntity(entity,error);
  }
  const auto value=image.value("value").toObject();
  if(!ensureWell(catalog,cache,modelId,value.value("name").toString(),error))return false;
  auto entity=catalog.storedEntityById(cache.wells.value(modelId));
  entity.name=value.value("name").toString();entity.hasSurface=value.value("x").isDouble() && value.value("y").isDouble();
  entity.surfaceX=value.value("x").toDouble();entity.surfaceY=value.value("y").toDouble();entity.coordinateStatus=entity.hasSurface?"untransformed":"missing";
  entity.td=value.value("totalDepth").toDouble(qQNaN());
  entity.extra["areaModelWellActive"]=true;entity.extra["wellCoordinatesCrs"]=value.value("crs").toString();
  entity.extra["wellCoordinateUnit"]=value.value("unit").toString();entity.extra["wellType"]=value.value("wellType").toString();
  entity.extra["fieldName"]=value.value("fieldName").toString();entity.extra["wellRemark"]=value.value("remark").toString();
  return catalog.updateEntity(entity,error);
}
bool ensureSurvey(DataCatalog &catalog,Cache &cache,const QJsonObject &row,QString *error) {
  const auto modelId=id(row.value("id"));if(modelId<=0)return reject(error,QStringLiteral("测区日志标识无效"));
  auto entity=catalog.storedEntityById(cache.surveys.value(modelId));const bool exists=!entity.id.isEmpty();
  if(!exists){entity.id=catalog.nextEntityId("survey");entity.entityType="seismic_survey";}
  entity.name=row.value("name").toString();entity.extra["areaModelVolumeId"]=modelId;entity.extra["areaModelGeneration"]=cache.generation;
  const auto geometry=QJsonDocument::fromJson(row.value("geometry").toString().toUtf8()).object();
  const auto sampling=QJsonDocument::fromJson(row.value("sampling").toString().toUtf8()).object();
  entity.inlineMin=geometry.value("inlineMin").toDouble();entity.inlineMax=geometry.value("inlineMax").toDouble();
  entity.xlineMin=geometry.value("crosslineMin").toDouble();entity.xlineMax=geometry.value("crosslineMax").toDouble();
  const auto unit=sampling.value("timeUnit").toString().toLower();
  const double factor=unit=="ms"?1000:unit=="us"?1:unit=="s"?1000000:0;
  entity.sampleIntervalUs=sampling.value("sampleInterval").toDouble()*factor;
  entity.startTimeMs=factor?sampling.value("firstTime").toDouble()*factor/1000:0;
  entity.extra["areaModelGeometry"]=geometry.toVariantMap();entity.extra["areaModelSampling"]=sampling.toVariantMap();
  if(!(exists?catalog.updateEntity(entity,error):catalog.addEntity(entity,error)))return false;
  cache.surveys.insert(modelId,entity.id);return true;
}
bool applySource(DataCatalog &catalog,Cache &cache,const paleo::data::ModelChangeItem &item,const QJsonObject &image,qint64 sequence,QString *error) {
  const auto source=image.value("source").toObject();const auto modelId=item.sourceId>0?item.sourceId:item.objectId;
  if(modelId<=0 || (!item.deleted && id(source.value("id"))!=modelId))return reject(error,QStringLiteral("来源日志身份不符"));
  auto previous=catalog.versionById(cache.sources.value(modelId));
  const bool active=!item.deleted && source.value("active").isBool() && source.value("active").toBool() &&
      source.value("status").toString()!=QLatin1String("failed") && source.value("status").toString()!=QLatin1String("ignored");
  QVariantMap observation{{"active",active},{"sequence",QString::number(sequence)},{"status",source.value("status").toString()},
      {"message",source.value("message").toString().left(4096)}};
  if(!active) {
    if(previous.id.isEmpty())return true;
    if(!catalog.updateVersionExtra(previous.id,"areaModelObservation",observation,error))return false;
    return catalog.currentVersion(previous.assetId).id!=previous.id || catalog.reviewAssetLinks(previous.assetId,error);
  }
  for(const auto &key:{"wells","objects","volumes","horizons"})
    if(!image.value(key).isArray())return reject(error,QStringLiteral("来源日志缺少完整元数据集合"));
  const auto path=source.value("path").toString(),sha=source.value("sha256").toString();
  auto stored=source.value("storedPath").toString();if(stored.isEmpty())stored=path;
  const auto root=QDir(catalog.projectDir()).absoluteFilePath("artifacts/metadata/area-model");
  if(!QFileInfo(stored).isAbsolute())stored=QDir(root).absoluteFilePath(stored);
  CatalogVersion version=previous;
  if(previous.id.isEmpty() || !(paleo::domain::shaEquals(previous.sha256,sha) || (previous.sha256.isEmpty() && sha.isEmpty())) || QDir::cleanPath(previous.path)!=QDir::cleanPath(stored)) {
    auto kinds=image.value("objects").toArray();
    for(const auto &kind:{"volumes","horizons"})if(!image.value(kind).toArray().isEmpty())kinds.append(QJsonObject{{"kind",kind}});
    const auto cls=classification(source,kinds);CatalogAsset asset;
    asset.id=previous.id.isEmpty()?catalog.nextAssetId():previous.assetId;asset.type=cls.type;asset.format=cls.format;asset.displayName=QFileInfo(path).fileName();
    if(previous.id.isEmpty() && !catalog.addAsset(asset,error))return false;
    version={};version.id=catalog.nextVersionId();version.assetId=asset.id;version.stage="RAW";version.managed=false;
    version.path=QDir::cleanPath(stored);version.sourceUri=path;version.fileName=QFileInfo(path).fileName();version.sha256=sha;
    version.extra={{"areaModelSourceId",qlonglong(modelId)},{"areaModelGeneration",cache.generation},{"areaModelSourceKind",source.value("kind").toString()},
      {"areaModelObservation",observation}};
    if(!previous.id.isEmpty()){version.versionNumber=catalog.currentVersion(asset.id).versionNumber+1;version.parentVersionIds={previous.id};}
    if(!catalog.addVersion(version,error))return false;cache.sources.insert(modelId,version.id);
  } else if(!catalog.updateVersionExtra(previous.id,"areaModelObservation",observation,error))return false;
  QSet<QString> available;
  for(const auto &well:image.value("wells").toArray())
    if(!ensureWell(catalog,cache,id(well.toObject().value("id")),well.toObject().value("name").toString(),error))return false;
  for(const auto &value:image.value("objects").toArray()) {
    const auto row=value.toObject();if(id(row.value("sourceId"))!=modelId)return reject(error,QStringLiteral("对象日志归属来源不符"));
    const auto wellId=id(row.value("wellId"));const auto roleName=role(row.value("kind").toString());
    if(wellId>0 && !roleName.isEmpty())available.insert(roleName+QLatin1Char('\x1f')+cache.wells.value(wellId));
    if(wellId>0 && !roleName.isEmpty() && (!cache.wells.contains(wellId) || !addLink(catalog,version,cache.wells.value(wellId),roleName,error)))
      return reject(error,error && !error->isEmpty()?*error:QStringLiteral("对象日志缺少所属井卡片"));
  }
  for(const auto &value:image.value("volumes").toArray()) {
    const auto row=value.toObject();if(id(row.value("sourceId"))!=modelId || !ensureSurvey(catalog,cache,row,error))return false;
    if(!addLink(catalog,version,cache.surveys.value(id(row.value("id"))),"seismic_volume",error))return false;
    available.insert(QStringLiteral("seismic_volume\x1f")+cache.surveys.value(id(row.value("id"))));
  }
  for(const auto &value:image.value("horizons").toArray()) {
    const auto row=value.toObject();if(id(row.value("sourceId"))!=modelId)return reject(error,QStringLiteral("层位日志归属来源不符"));
    const auto survey=cache.surveys.value(id(row.value("volumeId")));
    if(!survey.isEmpty() && !addLink(catalog,version,survey,"horizon",error))return false;
    if(!survey.isEmpty())available.insert(QStringLiteral("horizon\x1f")+survey);
  }
  return catalog.currentVersion(version.assetId).id!=version.id || catalog.reviewMissingModelLinks(version.assetId,available,error);
}
}
bool prepareItem(DataCatalog &catalog,Cache &cache,const paleo::data::ModelChangeItem &item,QString *error) {
  if(item.kind!="source" || item.deleted)return true;
  if(item.metadataJson.size()>8*1024*1024)return reject(error,QStringLiteral("来源日志卡片超过预算"));
  const auto document=QJsonDocument::fromJson(QByteArray::fromStdString(item.metadataJson));
  if(!document.isObject() || document.object().value("schema").toInt()!=1)return reject(error,QStringLiteral("来源日志格式无效"));
  for(const auto &value:document.object().value("volumes").toArray())
    if(!ensureSurvey(catalog,cache,value.toObject(),error))return false;
  return true;
}
bool applyItem(DataCatalog &catalog,Cache &cache,const paleo::data::ModelChangeItem &item,qint64 sequence,QString *error) {
  if(item.metadataJson.size()>8*1024*1024)return reject(error,QStringLiteral("来源日志卡片超过预算"));
  const auto document=QJsonDocument::fromJson(QByteArray::fromStdString(item.metadataJson));const auto image=document.object();
  if(!document.isObject() || !image.value("schema").isDouble() || image.value("schema").toDouble()!=1)
    return reject(error,QStringLiteral("来源日志元数据格式无效"));
  if(image.value("requiresRebuild").toBool())return reject(error,QStringLiteral("ModelProjectionRebuildRequired"));
  if(item.kind=="source")return applySource(catalog,cache,item,image,sequence,error);
  if(item.kind=="well")return applyWell(catalog,cache,image,item.deleted,error);
  if(item.sourceId>0)return true; // the batch must also carry its authoritative source image
  return reject(error,QStringLiteral("ModelProjectionRebuildRequired"));
}
}
