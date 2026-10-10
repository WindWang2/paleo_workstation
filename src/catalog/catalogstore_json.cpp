// 层：数据
#include "catalogstore.h"
#include "catalogstore_internal.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>

namespace
{
using paleo::store_detail::setError;

QVariantMap mapFromJson(const QString &text)
{
  if (text.isEmpty() || text == QLatin1String("{}"))
    return {};
  const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
  return doc.isObject() ? doc.object().toVariantMap() : QVariantMap();
}

QVector<QPair<double, double>> cornersFromJsonText(const QString &text)
{
  QVector<QPair<double, double>> cs;
  const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
  if (!doc.isArray())
    return cs;
  for (const auto &v : doc.array())
  {
    const QJsonObject o = v.toObject();
    cs.append({o.value(QStringLiteral("x")).toDouble(), o.value(QStringLiteral("y")).toDouble()});
  }
  return cs;
}

QStringList parentsFromJson(const QString &text)
{
  QStringList ids;
  const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
  if (!doc.isArray())
    return ids;
  for (const auto &v : doc.array())
    ids.append(v.toString());
  return ids;
}

QString unsafeVersionReason(const CatalogVersion &v)
{
  if (!v.fileName.isEmpty() && !DataCatalog::isSafePathSegment(v.fileName))
    return QStringLiteral("file name: %1").arg(v.fileName);
  if (!v.stage.isEmpty() && !DataCatalog::isSafePathSegment(v.stage))
    return QStringLiteral("stage: %1").arg(v.stage);
  if (v.managed && !v.path.isEmpty())
    for (const QString &seg : v.path.split(QLatin1Char('/')))
      if (!DataCatalog::isSafePathSegment(seg))
        return QStringLiteral("managed path segment: %1").arg(seg);
  return QString();
}

void absorbSeqs(CatalogStore::Tables *t)
{
  for (const CatalogAsset &a : t->assets)
  {
    bool ok = false;
    const int n = QString(a.id).mid(4).toInt(&ok);
    if (ok)
      t->meta.assetSeq = qMax(t->meta.assetSeq, n);
  }
  for (const CatalogVersion &v : t->versions)
  {
    bool ok = false;
    const int n = v.id.startsWith(QStringLiteral("ver-")) ? v.id.mid(4).toInt(&ok) : 0;
    if (ok && n > 0)
      t->meta.versionSeq = qMax(t->meta.versionSeq, n);
  }
}

QJsonArray cornersToJsonArray(const QVector<QPair<double, double>> &cs)
{
  QJsonArray a;
  for (const auto &c : cs)
  {
    QJsonObject o;
    o.insert(QStringLiteral("x"), c.first);
    o.insert(QStringLiteral("y"), c.second);
    a.append(o);
  }
  return a;
}

bool tableExists(QSqlDatabase &db, const QString &name)
{
  QSqlQuery q(db);
  q.prepare(QStringLiteral(
      "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?"));
  q.addBindValue(name);
  return q.exec() && q.next();
}

void absorbMetaRow(CatalogStore::Meta *meta, const QString &key, const QString &value)
{
  if (key == QLatin1String("catalog_revision"))
    meta->revision = value.toInt();
  else if (key == QLatin1String("mutation_seq"))
  {
    meta->mutationSeq = value.toULongLong();
    meta->hasMutationSeq = true;
  }
  else if (key == QLatin1String("asset_seq"))
    meta->assetSeq = qMax(meta->assetSeq, value.toInt());
  else if (key == QLatin1String("version_seq"))
    meta->versionSeq = qMax(meta->versionSeq, value.toInt());
  else if (key == QLatin1String("backup_keep"))
  {
    meta->backupKeep = value.toInt();
    meta->hasBackupKeep = true;
  }
}
} // namespace

// JSON 编解码 + SQL 装载域（方向 99 拆分）：toJson/fromJson（catalog.json
// 迁移输入与快照导出的编解码面）、路径静态、loadTables（四表 + meta
// ORDER BY rowid 装载——「表序最先」语义的事实源）。

namespace paleo::catalog_detail
{

bool loadTables(QSqlDatabase &db, CatalogStore::Tables *out, QString *error)
{
  *out = CatalogStore::Tables();
  if (!tableExists(db, QStringLiteral("entities")))
    return true;
  {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT key, value FROM catalog_meta")))
    {
      setError(error, q.lastError().text());
      return false;
    }
    while (q.next())
      absorbMetaRow(&out->meta, q.value(0).toString(), q.value(1).toString());
  }
  {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT id, entity_type, name, surface_x, surface_y, has_surface, kb, td,"
            " coordinate_status, inline_min, inline_max, xline_min, xline_max,"
            " sample_interval_us, start_time_ms, corners_json, extra_json"
            " FROM entities ORDER BY rowid")))
    {
      setError(error, q.lastError().text());
      return false;
    }
    while (q.next())
    {
      CatalogEntity e;
      e.id = q.value(0).toString();
      e.entityType = q.value(1).toString();
      e.name = q.value(2).toString();
      e.surfaceX = q.value(3).toDouble();
      e.surfaceY = q.value(4).toDouble();
      e.hasSurface = q.value(5).toInt() != 0;
      e.kb = q.value(6).toDouble();
      e.td = q.value(7).toDouble();
      e.coordinateStatus = q.value(8).toString();
      e.inlineMin = q.value(9).toDouble();
      e.inlineMax = q.value(10).toDouble();
      e.xlineMin = q.value(11).toDouble();
      e.xlineMax = q.value(12).toDouble();
      e.sampleIntervalUs = q.value(13).toDouble();
      e.startTimeMs = q.value(14).toDouble();
      e.corners = cornersFromJsonText(q.value(15).toString());
      e.extra = mapFromJson(q.value(16).toString());
      out->entities.append(e);
    }
  }
  {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT id, type, format, display_name FROM assets ORDER BY rowid")))
    {
      setError(error, q.lastError().text());
      return false;
    }
    while (q.next())
    {
      out->assets.append(CatalogAsset{q.value(0).toString(), q.value(1).toString(),
                                       q.value(2).toString(), q.value(3).toString()});
    }
  }
  {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT id, asset_id, stage, version_number, managed, path, source_uri, sha256,"
            " file_name, parent_version_ids_json, extra_json FROM versions ORDER BY rowid")))
    {
      setError(error, q.lastError().text());
      return false;
    }
    while (q.next())
    {
      CatalogVersion v;
      v.id = q.value(0).toString();
      v.assetId = q.value(1).toString();
      v.stage = q.value(2).toString();
      v.versionNumber = q.value(3).toInt();
      v.managed = q.value(4).toInt() != 0;
      v.path = q.value(5).toString();
      v.sourceUri = q.value(6).toString();
      v.sha256 = q.value(7).toString();
      v.fileName = q.value(8).toString();
      v.parentVersionIds = parentsFromJson(q.value(9).toString());
      v.extra = mapFromJson(q.value(10).toString());
      const QString bad = unsafeVersionReason(v);
      if (!bad.isEmpty())
      {
        // 已落盘的坏段留在库里，只从内存跳过。增量保存不会 DELETE 它。
        qWarning("catalog: skipping version %s with unsafe path segment: %s", qPrintable(v.id),
                 qPrintable(bad));
        continue;
      }
      out->versions.append(v);
    }
  }
  {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT ord, entity_type, entity_id, asset_id, role, is_primary, unresolved,"
            " ordinal, note FROM entity_asset_links ORDER BY ord")))
    {
      setError(error, q.lastError().text());
      return false;
    }
    while (q.next())
    {
      EntityAssetLink l;
      l.entityType = q.value(1).toString();
      l.entityId = q.value(2).toString();
      l.assetId = q.value(3).toString();
      l.role = q.value(4).toString();
      l.isPrimary = q.value(5).toInt() != 0;
      l.unresolved = q.value(6).toInt() != 0;
      l.ordinal = q.value(7).toInt();
      l.note = q.value(8).toString();
      const int ord = q.value(0).toInt();
      if (ord == out->links.size())
        out->links.append(l);
      else if (ord >= 0)
      {
        if (ord > out->links.size())
          out->links.resize(ord + 1);
        out->links[ord] = l;
      }
    }
  }
  absorbSeqs(out);
  return true;
}

} // namespace paleo::catalog_detail

QString CatalogStore::sqlitePathFor(const QString &projectDir)
{
  return QDir(projectDir).filePath(QStringLiteral("artifacts/metadata/catalog.sqlite"));
}

QString CatalogStore::jsonPathFor(const QString &projectDir)
{
  return QDir(projectDir).filePath(QStringLiteral("artifacts/metadata/catalog.json"));
}

QJsonObject CatalogStore::toJson(const Tables &tables)
{
  QJsonObject root;
  root.insert(QStringLiteral("schema_version"), kJsonSchemaVersion);
  root.insert(QStringLiteral("catalog_revision"), tables.meta.revision);
  QJsonArray ents, asts, vers, lnks;
  for (const CatalogEntity &e : tables.entities)
  {
    QJsonObject o;
    o.insert(QStringLiteral("id"), e.id);
    o.insert(QStringLiteral("entity_type"), e.entityType);
    o.insert(QStringLiteral("name"), e.name);
    o.insert(QStringLiteral("surface_x"), e.surfaceX);
    o.insert(QStringLiteral("surface_y"), e.surfaceY);
    o.insert(QStringLiteral("has_surface"), e.hasSurface);
    o.insert(QStringLiteral("kb"), e.kb);
    o.insert(QStringLiteral("td"), e.td);
    o.insert(QStringLiteral("coordinate_status"), e.coordinateStatus);
    o.insert(QStringLiteral("inline_min"), e.inlineMin);
    o.insert(QStringLiteral("inline_max"), e.inlineMax);
    o.insert(QStringLiteral("xline_min"), e.xlineMin);
    o.insert(QStringLiteral("xline_max"), e.xlineMax);
    o.insert(QStringLiteral("sample_interval_us"), e.sampleIntervalUs);
    o.insert(QStringLiteral("start_time_ms"), e.startTimeMs);
    o.insert(QStringLiteral("corners"), cornersToJsonArray(e.corners));
    if (!e.extra.isEmpty())
      o.insert(QStringLiteral("extra"), QJsonObject::fromVariantMap(e.extra));
    ents.append(o);
  }
  for (const CatalogAsset &a : tables.assets)
  {
    QJsonObject o;
    o.insert(QStringLiteral("id"), a.id);
    o.insert(QStringLiteral("type"), a.type);
    o.insert(QStringLiteral("format"), a.format);
    o.insert(QStringLiteral("display_name"), a.displayName);
    asts.append(o);
  }
  for (const CatalogVersion &v : tables.versions)
  {
    QJsonObject o;
    o.insert(QStringLiteral("id"), v.id);
    o.insert(QStringLiteral("asset_id"), v.assetId);
    o.insert(QStringLiteral("stage"), v.stage);
    o.insert(QStringLiteral("version_number"), v.versionNumber);
    o.insert(QStringLiteral("managed"), v.managed);
    o.insert(QStringLiteral("path"), v.path);
    o.insert(QStringLiteral("source_uri"), v.sourceUri);
    o.insert(QStringLiteral("sha256"), v.sha256);
    o.insert(QStringLiteral("file_name"), v.fileName);
    o.insert(QStringLiteral("parent_version_ids"), QJsonArray::fromStringList(v.parentVersionIds));
    if (!v.extra.isEmpty())
      o.insert(QStringLiteral("extra"), QJsonObject::fromVariantMap(v.extra));
    vers.append(o);
  }
  for (const EntityAssetLink &l : tables.links)
  {
    QJsonObject o;
    o.insert(QStringLiteral("entity_type"), l.entityType);
    o.insert(QStringLiteral("entity_id"), l.entityId);
    o.insert(QStringLiteral("asset_id"), l.assetId);
    o.insert(QStringLiteral("role"), l.role);
    o.insert(QStringLiteral("is_primary"), l.isPrimary);
    o.insert(QStringLiteral("unresolved"), l.unresolved);
    o.insert(QStringLiteral("ordinal"), l.ordinal);
    o.insert(QStringLiteral("note"), l.note);
    lnks.append(o);
  }
  root.insert(QStringLiteral("entities"), ents);
  root.insert(QStringLiteral("assets"), asts);
  root.insert(QStringLiteral("versions"), vers);
  root.insert(QStringLiteral("entity_asset_links"), lnks);
  return root;
}

bool CatalogStore::fromJson(const QJsonObject &root, Tables *out, QString *error)
{
  if (!out)
  {
    setError(error, QStringLiteral("catalog json destination is null"));
    return false;
  }
  *out = Tables();
  const QString schemaKey = QStringLiteral("schema_version");
  if (root.contains(schemaKey) && root.value(schemaKey).toInt() != kJsonSchemaVersion)
  {
    setError(error, QStringLiteral("unsupported catalog schema"));
    return false;
  }
  out->meta.revision = root.value(QStringLiteral("catalog_revision")).toInt();
  for (const auto &v : root.value(QStringLiteral("entities")).toArray())
  {
    const QJsonObject o = v.toObject();
    CatalogEntity e;
    e.id = o.value(QStringLiteral("id")).toString();
    e.entityType = o.value(QStringLiteral("entity_type")).toString();
    e.name = o.value(QStringLiteral("name")).toString();
    e.surfaceX = o.value(QStringLiteral("surface_x")).toDouble();
    e.surfaceY = o.value(QStringLiteral("surface_y")).toDouble();
    e.hasSurface = o.value(QStringLiteral("has_surface")).toBool();
    e.kb = o.value(QStringLiteral("kb")).toDouble();
    e.td = o.value(QStringLiteral("td")).toDouble();
    e.coordinateStatus = o.value(QStringLiteral("coordinate_status")).toString();
    e.inlineMin = o.value(QStringLiteral("inline_min")).toDouble();
    e.inlineMax = o.value(QStringLiteral("inline_max")).toDouble();
    e.xlineMin = o.value(QStringLiteral("xline_min")).toDouble();
    e.xlineMax = o.value(QStringLiteral("xline_max")).toDouble();
    e.sampleIntervalUs = o.value(QStringLiteral("sample_interval_us")).toDouble();
    e.startTimeMs = o.value(QStringLiteral("start_time_ms")).toDouble();
    for (const auto &c : o.value(QStringLiteral("corners")).toArray())
    {
      const QJsonObject co = c.toObject();
      e.corners.append({co.value(QStringLiteral("x")).toDouble(),
                        co.value(QStringLiteral("y")).toDouble()});
    }
    e.extra = o.value(QStringLiteral("extra")).toObject().toVariantMap();
    out->entities.append(e);
  }
  for (const auto &v : root.value(QStringLiteral("assets")).toArray())
  {
    const QJsonObject o = v.toObject();
    out->assets.append(CatalogAsset{o.value(QStringLiteral("id")).toString(),
                                    o.value(QStringLiteral("type")).toString(),
                                    o.value(QStringLiteral("format")).toString(),
                                    o.value(QStringLiteral("display_name")).toString()});
  }
  for (const auto &v : root.value(QStringLiteral("versions")).toArray())
  {
    const QJsonObject o = v.toObject();
    CatalogVersion cv;
    cv.id = o.value(QStringLiteral("id")).toString();
    cv.assetId = o.value(QStringLiteral("asset_id")).toString();
    cv.stage = o.value(QStringLiteral("stage")).toString();
    cv.versionNumber = o.value(QStringLiteral("version_number")).toInt(1);
    cv.managed = o.value(QStringLiteral("managed")).toBool(true);
    cv.path = o.value(QStringLiteral("path")).toString();
    cv.sourceUri = o.value(QStringLiteral("source_uri")).toString();
    cv.sha256 = o.value(QStringLiteral("sha256")).toString();
    cv.fileName = o.value(QStringLiteral("file_name")).toString();
    for (const auto &p : o.value(QStringLiteral("parent_version_ids")).toArray())
      cv.parentVersionIds.append(p.toString());
    cv.extra = o.value(QStringLiteral("extra")).toObject().toVariantMap();
    const QString bad = unsafeVersionReason(cv);
    if (!bad.isEmpty())
    {
      qWarning("catalog: skipping version %s with unsafe path segment: %s", qPrintable(cv.id),
               qPrintable(bad));
      continue;
    }
    out->versions.append(cv);
  }
  for (const auto &v : root.value(QStringLiteral("entity_asset_links")).toArray())
  {
    const QJsonObject o = v.toObject();
    EntityAssetLink l;
    l.entityType = o.value(QStringLiteral("entity_type")).toString();
    l.entityId = o.value(QStringLiteral("entity_id")).toString();
    l.assetId = o.value(QStringLiteral("asset_id")).toString();
    l.role = o.value(QStringLiteral("role")).toString();
    l.isPrimary = o.value(QStringLiteral("is_primary")).toBool(true);
    l.unresolved = o.value(QStringLiteral("unresolved")).toBool(false);
    l.ordinal = o.value(QStringLiteral("ordinal")).toInt(0);
    l.note = o.value(QStringLiteral("note")).toString();
    out->links.append(l);
  }
  absorbSeqs(out);
  out->meta.hasMutationSeq = false;
  return true;
}
