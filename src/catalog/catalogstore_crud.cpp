// 层：数据
#include "catalogstore.h"
#include "catalogstore_internal.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>

namespace
{
using paleo::store_detail::setError;
using paleo::catalog_detail::execSql;

// 未填的 QString 是 null。绑进 NOT NULL TEXT 会变成 SQL NULL 并被拒。
QVariant textArg(const QString &s)
{
  return s.isNull() ? QVariant(QStringLiteral("")) : QVariant(s);
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

bool CatalogStore::upsertEntity(const CatalogEntity &e, QString *error)
{
  QSqlDatabase db = QSqlDatabase::database(m_connectionName);
  QSqlQuery q(db);
  q.prepare(QStringLiteral(
      "INSERT INTO entities (id, entity_type, name, surface_x, surface_y, has_surface, kb, td,"
      " coordinate_status, inline_min, inline_max, xline_min, xline_max, sample_interval_us,"
      " start_time_ms, corners_json, extra_json) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
      " ON CONFLICT(id) DO UPDATE SET"
      " entity_type=excluded.entity_type, name=excluded.name, surface_x=excluded.surface_x,"
      " surface_y=excluded.surface_y, has_surface=excluded.has_surface, kb=excluded.kb,"
      " td=excluded.td, coordinate_status=excluded.coordinate_status,"
      " inline_min=excluded.inline_min, inline_max=excluded.inline_max,"
      " xline_min=excluded.xline_min, xline_max=excluded.xline_max,"
      " sample_interval_us=excluded.sample_interval_us, start_time_ms=excluded.start_time_ms,"
      " corners_json=excluded.corners_json, extra_json=excluded.extra_json"));
  q.addBindValue(textArg(e.id));
  q.addBindValue(textArg(e.entityType));
  q.addBindValue(textArg(e.name));
  q.addBindValue(e.surfaceX);
  q.addBindValue(e.surfaceY);
  q.addBindValue(e.hasSurface ? 1 : 0);
  q.addBindValue(e.kb);
  q.addBindValue(e.td);
  q.addBindValue(textArg(e.coordinateStatus));
  q.addBindValue(e.inlineMin);
  q.addBindValue(e.inlineMax);
  q.addBindValue(e.xlineMin);
  q.addBindValue(e.xlineMax);
  q.addBindValue(e.sampleIntervalUs);
  q.addBindValue(e.startTimeMs);
  q.addBindValue(textArg(cornersToJson(e.corners)));
  q.addBindValue(textArg(mapToJson(e.extra)));
  if (!q.exec())
  {
    setError(error, q.lastError().text());
    return false;
  }
  return true;
}

bool CatalogStore::upsertAsset(const CatalogAsset &a, QString *error)
{
  QSqlDatabase db = QSqlDatabase::database(m_connectionName);
  QSqlQuery q(db);
  q.prepare(QStringLiteral(
      "INSERT INTO assets (id, type, format, display_name) VALUES (?,?,?,?)"
      " ON CONFLICT(id) DO UPDATE SET type=excluded.type, format=excluded.format,"
      " display_name=excluded.display_name"));
  q.addBindValue(textArg(a.id));
  q.addBindValue(textArg(a.type));
  q.addBindValue(textArg(a.format));
  q.addBindValue(textArg(a.displayName));
  if (!q.exec())
  {
    setError(error, q.lastError().text());
    return false;
  }
  return true;
}

bool CatalogStore::upsertVersion(const CatalogVersion &v, QString *error)
{
  QSqlDatabase db = QSqlDatabase::database(m_connectionName);
  QSqlQuery q(db);
  q.prepare(QStringLiteral(
      "INSERT INTO versions (id, asset_id, stage, version_number, managed, path, source_uri,"
      " sha256, file_name, parent_version_ids_json, extra_json) VALUES (?,?,?,?,?,?,?,?,?,?,?)"
      " ON CONFLICT(id) DO UPDATE SET asset_id=excluded.asset_id, stage=excluded.stage,"
      " version_number=excluded.version_number, managed=excluded.managed, path=excluded.path,"
      " source_uri=excluded.source_uri, sha256=excluded.sha256, file_name=excluded.file_name,"
      " parent_version_ids_json=excluded.parent_version_ids_json, extra_json=excluded.extra_json"));
  q.addBindValue(textArg(v.id));
  q.addBindValue(textArg(v.assetId));
  q.addBindValue(textArg(v.stage));
  q.addBindValue(v.versionNumber);
  q.addBindValue(v.managed ? 1 : 0);
  q.addBindValue(textArg(v.path));
  q.addBindValue(textArg(v.sourceUri));
  q.addBindValue(textArg(v.sha256));
  q.addBindValue(textArg(v.fileName));
  q.addBindValue(textArg(parentsToJson(v.parentVersionIds)));
  q.addBindValue(textArg(mapToJson(v.extra)));
  if (!q.exec())
  {
    setError(error, q.lastError().text());
    return false;
  }
  return true;
}

bool CatalogStore::upsertLink(int ord, const EntityAssetLink &l, QString *error)
{
  QSqlDatabase db = QSqlDatabase::database(m_connectionName);
  QSqlQuery q(db);
  q.prepare(QStringLiteral(
      "INSERT INTO entity_asset_links (ord, entity_type, entity_id, asset_id, role, is_primary,"
      " unresolved, ordinal, note) VALUES (?,?,?,?,?,?,?,?,?)"
      " ON CONFLICT(ord) DO UPDATE SET entity_type=excluded.entity_type,"
      " entity_id=excluded.entity_id, asset_id=excluded.asset_id, role=excluded.role,"
      " is_primary=excluded.is_primary, unresolved=excluded.unresolved,"
      " ordinal=excluded.ordinal, note=excluded.note"));
  q.addBindValue(ord);
  q.addBindValue(textArg(l.entityType));
  q.addBindValue(textArg(l.entityId));
  q.addBindValue(textArg(l.assetId));
  q.addBindValue(textArg(l.role));
  q.addBindValue(l.isPrimary ? 1 : 0);
  q.addBindValue(l.unresolved ? 1 : 0);
  q.addBindValue(l.ordinal);
  q.addBindValue(textArg(l.note));
  if (!q.exec())
  {
    setError(error, q.lastError().text());
    return false;
  }
  return true;
}

bool CatalogStore::deleteAsset(const QString &id, QString *error)
{
  QSqlDatabase db = QSqlDatabase::database(m_connectionName);
  QSqlQuery q(db);
  q.prepare(QStringLiteral("DELETE FROM assets WHERE id = ?"));
  q.addBindValue(textArg(id));
  if (!q.exec())
  {
    setError(error, q.lastError().text());
    return false;
  }
  return true;
}

bool CatalogStore::deleteVersion(const QString &id, QString *error)
{
  QSqlDatabase db = QSqlDatabase::database(m_connectionName);
  QSqlQuery q(db);
  q.prepare(QStringLiteral("DELETE FROM versions WHERE id = ?"));
  q.addBindValue(textArg(id));
  if (!q.exec())
  {
    setError(error, q.lastError().text());
    return false;
  }
  return true;
}

bool CatalogStore::replaceAllLinks(const QVector<EntityAssetLink> &links,
                                   QString *error)
{
  QSqlDatabase db = QSqlDatabase::database(m_connectionName);
  {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("DELETE FROM entity_asset_links")))
    {
      setError(error, q.lastError().text());
      return false;
    }
  }
  for (int ord = 0; ord < links.size(); ++ord)
    if (!upsertLink(ord, links.at(ord), error))
      return false;
  return true;
}

bool CatalogStore::writeMeta(const Meta &meta, QString *error)
{
  QSqlDatabase db = QSqlDatabase::database(m_connectionName);
  QSqlQuery q(db);
  q.prepare(QStringLiteral(
      "INSERT INTO catalog_meta (key, value) VALUES (?, ?)"
      " ON CONFLICT(key) DO UPDATE SET value=excluded.value"));
  const QList<QPair<QString, QString>> rows = {
      {QStringLiteral("schema_epoch"), QString::number(kSchemaEpoch)},
      {QStringLiteral("catalog_revision"), QString::number(meta.revision)},
      {QStringLiteral("mutation_seq"), QString::number(meta.mutationSeq)},
      {QStringLiteral("asset_seq"), QString::number(meta.assetSeq)},
      {QStringLiteral("version_seq"), QString::number(meta.versionSeq)},
      {QStringLiteral("backup_keep"), QString::number(meta.backupKeep)},
  };
  for (const auto &row : rows)
  {
    q.bindValue(0, row.first);
    q.bindValue(1, row.second);
    if (!q.exec())
    {
      setError(error, q.lastError().text());
      return false;
    }
  }
  return true;
}

bool CatalogStore::replaceAll(const Tables &tables, QString *error)
{
  if (!begin(error))
    return false;
  {
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    const char *deletes[] = {"DELETE FROM entity_asset_links", "DELETE FROM versions",
                             "DELETE FROM assets", "DELETE FROM entities"};
    for (const char *sql : deletes)
    {
      if (!execSql(db, QString::fromLatin1(sql), error))
      {
        rollback();
        return false;
      }
    }
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
