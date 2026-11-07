// 层：数据
#include "datacatalog.h"
#include "../metadata/storeerrors_internal.h"

QJsonObject CatalogProjectionCheckpoint::toJson() const {
  return {{"schema",1},{"storeId",storeId},{"generation",generation},{"sequence",QString::number(sequence)}};
}
bool CatalogProjectionCheckpoint::fromJson(const QJsonObject &json,CatalogProjectionCheckpoint &result) {
  result={};bool ok=false;
  if (!json.value("schema").isDouble() || json.value("schema").toDouble()!=1 ||
      !json.value("sequence").isString() || !json.value("storeId").isString() || !json.value("generation").isString())return false;
  result.storeId=json.value("storeId").toString();result.generation=json.value("generation").toString();
  result.sequence=json.value("sequence").toString().toLongLong(&ok);
  if (!ok || !result.valid() || result.storeId.size()>256 || result.generation.size()>256) {result={};return false;}
  return true;
}
bool validProjectionCheckpoints(const QJsonObject &json) {
  if (json.size()>32)return false;
  for(auto it=json.begin();it!=json.end();++it) {
    CatalogProjectionCheckpoint checkpoint;
    if(it.key().isEmpty() || it.key().size()>128 || !it.value().isObject() ||
       !CatalogProjectionCheckpoint::fromJson(it.value().toObject(),checkpoint))return false;
  }
  return true;
}
CatalogProjectionCheckpoint DataCatalog::projectionCheckpoint(const QString &projection) const {
  CatalogProjectionCheckpoint result;
  if(noteRead("projectionCheckpoint"))CatalogProjectionCheckpoint::fromJson(m_projectionCheckpoints.value(projection).toObject(),result);
  return result;
}
bool DataCatalog::setProjectionCheckpoint(const QString &projection,const CatalogProjectionCheckpoint &checkpoint,QString *error) {
  if(!checkWriteThread("setProjectionCheckpoint",error) || !ensureOpen(error))return false;
  const auto previous=m_projectionCheckpoints;
  auto candidate=previous;candidate.insert(projection,checkpoint.toJson());
  if(!validProjectionCheckpoints(candidate)) {paleo::store_detail::setError(error,QStringLiteral("投影水位格式无效"));return false;}
  const auto old=projectionCheckpoint(projection);
  if(old.valid() && old.storeId==checkpoint.storeId && old.generation==checkpoint.generation && checkpoint.sequence<old.sequence) {
    paleo::store_detail::setError(error,QStringLiteral("投影水位不能回退"));return false;
  }
  if(old==checkpoint)return true;
  m_projectionCheckpoints=candidate;
  if(!save(error)) {m_projectionCheckpoints=previous;return false;}
  CatalogOp operation;operation.kind=CatalogOp::Kind::SetProjectionCheckpoint;operation.id=projection;operation.extraValue=checkpoint.toJson().toVariantMap();recordOp(std::move(operation));
  return true;
}
QVector<CatalogEntity> DataCatalog::storedEntities(const QString &entityType) const {
  if(!noteRead("storedEntities"))return {};
  if(entityType.isEmpty())return m_entities;
  QVector<CatalogEntity> result;
  for(int row:m_idx.entityRowsByType(entityType))result.append(m_entities.at(row));
  return result;
}
bool DataCatalog::reviewAssetLinks(const QString &assetId,QString *error) {
  if(!checkWriteThread("reviewAssetLinks",error) || !ensureOpen(error))return false;
  for(int row:m_idx.linkRowsForAsset(assetId)) {
    auto link=m_links.at(row);
    if(link.unresolved)continue;
    link.note=QStringLiteral("模型来源已停用或删除，原关联需复核（%1）").arg(link.entityId);
    link.entityId.clear();link.unresolved=true;link.isPrimary=false;
    if(!updateLink(row,link,error))return false;
  }
  return true;
}
bool DataCatalog::reviewMissingModelLinks(const QString &assetId,const QSet<QString> &available,QString *error) {
  if(!checkWriteThread("reviewMissingModelLinks",error) || !ensureOpen(error))return false;
  static const QSet<QString> roles{"well_log","tops","time_depth","trajectory","core","lab_analysis","reference","interpretation","seismic_volume","horizon"};
  for(int row:m_idx.linkRowsForAsset(assetId)) {
    auto link=m_links.at(row);
    if(link.unresolved || !roles.contains(link.role) || available.contains(link.role+QLatin1Char('\x1f')+link.entityId))continue;
    link.note+=QStringLiteral("\n模型资料角色已移除，原关联需复核（%1）").arg(link.entityId);
    link.entityId.clear();link.unresolved=true;link.isPrimary=false;
    if(!updateLink(row,link,error))return false;
  }
  return true;
}
