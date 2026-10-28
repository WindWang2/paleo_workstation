// 层：数据
#include "catalogstore.h"
#include "catalogstore_internal.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>

namespace
{
using paleo::store_detail::setError;

// 未填的 QString 是 null。绑进 NOT NULL TEXT 会变成 SQL NULL 并被拒。
std::string utf8(const QString &s)
{
  return s.toUtf8().toStdString();
}

bool finishDataCall(bool ok, const std::string &detail, QString *error)
{
  if (!ok) setError(error, QString::fromUtf8(detail.data(), qsizetype(detail.size())));
  return ok;
}

QString mapToJson(const QVariantMap &map)
{
  if (map.isEmpty())
    return QStringLiteral("{}");
  return QString::fromUtf8(
      QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Compact));
}

QString cornersToJson(const QVector<QPair<double, double>> &cs)
{
  QJsonArray a;
  for (const auto &c : cs)
  {
    QJsonObject o;
    o.insert(QStringLiteral("x"), c.first);
    o.insert(QStringLiteral("y"), c.second);
    a.append(o);
  }
  return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
}

QString parentsToJson(const QStringList &ids)
{
  return QString::fromUtf8(
      QJsonDocument(QJsonArray::fromStringList(ids)).toJson(QJsonDocument::Compact));
}
} // namespace

// CRUD 写域（方向 99 拆分）：四表 upsert + 增量删除 + 链接整表重写 +
// meta 写。事务语义：本域函数一律在调用方（DataCatalog::commitStore /
// replaceAll）的事务边界内执行——begin/commit/rollback 在 _schema TU。

bool CatalogStore::upsertEntity(const CatalogEntity &v, QString *error)
{
  paleo::data::CatalogStorage store(utf8(m_connectionName));
  std::string detail;
  paleo::data::CatalogEntity record;
  record.id = utf8(v.id);
  record.entityType = utf8(v.entityType);
  record.name = utf8(v.name);
  record.surfaceX = v.surfaceX;
  record.surfaceY = v.surfaceY;
  record.hasSurface = v.hasSurface;
  record.kb = v.kb;
  record.td = v.td;
  record.coordinateStatus = utf8(v.coordinateStatus);
  record.inlineMin = v.inlineMin;
  record.inlineMax = v.inlineMax;
  record.xlineMin = v.xlineMin;
  record.xlineMax = v.xlineMax;
  record.sampleIntervalUs = v.sampleIntervalUs;
  record.startTimeMs = v.startTimeMs;
  auto extra = v.extra;
  extra.insert(paleo::catalog_detail::kKbMissingKey, !std::isfinite(v.kb));
  extra.insert(paleo::catalog_detail::kTdMissingKey, !std::isfinite(v.td));
  record.kb = std::isfinite(v.kb) ? v.kb : 0.0;
  record.td = std::isfinite(v.td) ? v.td : 0.0;
  record.cornersJson = utf8(cornersToJson(v.corners));
  record.extraJson = utf8(mapToJson(extra));
  const bool ok = store.upsert(record, detail);
  return finishDataCall(ok, detail, error);
}

bool CatalogStore::upsertAsset(const CatalogAsset &v, QString *error)
{
  paleo::data::CatalogStorage store(utf8(m_connectionName));
  std::string detail;
  paleo::data::CatalogAsset record;
  record.id = utf8(v.id);
  record.type = utf8(v.type);
  record.format = utf8(v.format);
  record.displayName = utf8(v.displayName);
  const bool ok = store.upsert(record, detail);
  return finishDataCall(ok, detail, error);
}

bool CatalogStore::upsertVersion(const CatalogVersion &v, QString *error)
{
  paleo::data::CatalogStorage store(utf8(m_connectionName));
  std::string detail;
  paleo::data::CatalogVersion record;
  record.id = utf8(v.id);
  record.assetId = utf8(v.assetId);
  record.stage = utf8(v.stage);
  record.versionNumber = v.versionNumber;
  record.managed = v.managed;
  record.path = utf8(v.path);
  record.sourceUri = utf8(v.sourceUri);
  record.sha256 = utf8(v.sha256);
  record.fileName = utf8(v.fileName);
  record.parentVersionIdsJson = utf8(parentsToJson(v.parentVersionIds));
  record.extraJson = utf8(mapToJson(v.extra));
  const bool ok = store.upsert(record, detail);
  return finishDataCall(ok, detail, error);
}

bool CatalogStore::upsertLink(int ord, const EntityAssetLink &v, QString *error)
{
  paleo::data::CatalogStorage store(utf8(m_connectionName));
  std::string detail;
  paleo::data::CatalogLink record;
  record.ord = ord;
  record.entityType = utf8(v.entityType);
  record.entityId = utf8(v.entityId);
  record.assetId = utf8(v.assetId);
  record.role = utf8(v.role);
  record.isPrimary = v.isPrimary;
  record.unresolved = v.unresolved;
  record.ordinal = v.ordinal;
  record.note = utf8(v.note);
  const bool ok = store.upsert(record, detail);
  return finishDataCall(ok, detail, error);
}

bool CatalogStore::deleteAsset(const QString &id, QString *error)
{
  paleo::data::CatalogStorage store(utf8(m_connectionName));
  std::string detail;
  const bool ok = store.eraseAsset(utf8(id), detail);
  return finishDataCall(ok, detail, error);
}

bool CatalogStore::deleteVersion(const QString &id, QString *error)
{
  paleo::data::CatalogStorage store(utf8(m_connectionName));
  std::string detail;
  const bool ok = store.eraseVersion(utf8(id), detail);
  return finishDataCall(ok, detail, error);
}

bool CatalogStore::replaceAllLinks(const QVector<EntityAssetLink> &links, QString *error)
{
  Q_ASSERT(m_inTxn);
  if (!m_inTxn) {
    setError(error, QStringLiteral("replaceAllLinks must run inside a transaction"));
    return false;
  }
  paleo::data::CatalogStorage store(utf8(m_connectionName));
  std::string detail;
  if (!store.clearLinks(detail)) return finishDataCall(false, detail, error);
  for (int ord = 0; ord < links.size(); ++ord)
    if (!upsertLink(ord, links.at(ord), error)) return false;
  return true;
}

bool CatalogStore::writeMeta(const Meta &meta, QString *error)
{
  paleo::data::CatalogStorage store(utf8(m_connectionName));
  std::string detail;
  const std::vector<std::pair<std::string, std::string>> rows = {
      {"schema_epoch", std::to_string(kSchemaEpoch)},
      {"catalog_revision", std::to_string(meta.revision)},
      {"mutation_seq", std::to_string(meta.mutationSeq)},
      {"asset_seq", std::to_string(meta.assetSeq)},
      {"version_seq", std::to_string(meta.versionSeq)},
      {"backup_keep", std::to_string(meta.backupKeep)}};
  const bool ok = store.writeMeta(rows, detail);
  return finishDataCall(ok, detail, error);
}

bool CatalogStore::replaceAll(const Tables &tables, QString *error)
{
  if (!begin(error))
    return false;
  paleo::data::CatalogStorage store(utf8(m_connectionName));
  std::string detail;
  if (!store.clearRecords(detail)) {
    rollback();
    return finishDataCall(false, detail, error);
  }
  for (const CatalogEntity &e : tables.entities)
  {
    if (!upsertEntity(e, error))
    {
      rollback();
      return false;
    }
  }
  for (const CatalogAsset &a : tables.assets)
  {
    if (!upsertAsset(a, error))
    {
      rollback();
      return false;
    }
  }
  for (const CatalogVersion &v : tables.versions)
  {
    if (!upsertVersion(v, error))
    {
      rollback();
      return false;
    }
  }
  for (int i = 0; i < tables.links.size(); ++i)
  {
    if (!upsertLink(i, tables.links.at(i), error))
    {
      rollback();
      return false;
    }
  }
  if (!writeMeta(tables.meta, error))
  {
    rollback();
    return false;
  }
  if (!commit(error))
  {
    rollback();
    return false;
  }
  return true;
}
