// 层：数据
#include "catalogstore.h"
#include "../metadata/storeerrors_internal.h"

#include "../metadata/atomicfile.h"
#include "../metadata/metastore.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace
{
using paleo::store_detail::setError;

  // catalog.sqlite 的 user_version 版本域独立于 project.sqlite 的
  // MetaStore::kUserVersion：早期构建复用 project 版本门，把 catalog 文件
  // 标成了 1/2——兼容常量取 2 放行存量库。今后 project schema 推进不再
  // 影响 catalog；catalog 自身的 schema 演进以 meta 表 schema_epoch 为准。
  constexpr int kCatalogUserVersion = 2;

  QString catalogConnectionName(const CatalogStore *self, const QString &path, bool readOnly)
  {
    return QStringLiteral("paleo_catalog_%1_%2%3")
        .arg(QString::number(qHash(QFileInfo(path).absoluteFilePath()), 16),
             QString::number(reinterpret_cast<quintptr>(self), 16),
             readOnly ? QStringLiteral("_ro") : QString());
  }

  // 未填的 QString 是 null。绑进 NOT NULL TEXT 会变成 SQL NULL 并被拒。
  QVariant textArg(const QString &s)
  {
    return s.isNull() ? QVariant(QStringLiteral("")) : QVariant(s);
  }

  bool execSql(QSqlDatabase &db, const QString &sql, QString *error)
  {
    QSqlQuery q(db);
    if (!q.exec(sql))
    {
      setError(error, q.lastError().text().isEmpty() ? sql : q.lastError().text());
      return false;
    }
    return true;
  }

  QString mapToJson(const QVariantMap &map)
  {
    if (map.isEmpty())
      return QStringLiteral("{}");
    return QString::fromUtf8(
        QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Compact));
  }

  QVariantMap mapFromJson(const QString &text)
  {
    if (text.isEmpty() || text == QLatin1String("{}"))
      return {};
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    return doc.isObject() ? doc.object().toVariantMap() : QVariantMap();
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

  QString parentsToJson(const QStringList &ids)
  {
    return QString::fromUtf8(
        QJsonDocument(QJsonArray::fromStringList(ids)).toJson(QJsonDocument::Compact));
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

  const char *kSchemaSql =
      "CREATE TABLE IF NOT EXISTS entities ("
      " id TEXT PRIMARY KEY NOT NULL,"
      " entity_type TEXT NOT NULL DEFAULT '',"
      " name TEXT NOT NULL DEFAULT '',"
      " surface_x REAL NOT NULL DEFAULT 0,"
      " surface_y REAL NOT NULL DEFAULT 0,"
      " has_surface INTEGER NOT NULL DEFAULT 0,"
      " kb REAL NOT NULL DEFAULT 0,"
      " td REAL NOT NULL DEFAULT 0,"
      " coordinate_status TEXT NOT NULL DEFAULT '',"
      " inline_min REAL NOT NULL DEFAULT 0,"
      " inline_max REAL NOT NULL DEFAULT 0,"
      " xline_min REAL NOT NULL DEFAULT 0,"
      " xline_max REAL NOT NULL DEFAULT 0,"
      " sample_interval_us REAL NOT NULL DEFAULT 0,"
      " start_time_ms REAL NOT NULL DEFAULT 0,"
      " corners_json TEXT NOT NULL DEFAULT '[]',"
      " extra_json TEXT NOT NULL DEFAULT '{}'"
      ");"
      "CREATE INDEX IF NOT EXISTS idx_entities_type_name ON entities(entity_type, name);"
      "CREATE TABLE IF NOT EXISTS assets ("
      " id TEXT PRIMARY KEY NOT NULL,"
      " type TEXT NOT NULL DEFAULT '',"
      " format TEXT NOT NULL DEFAULT '',"
      " display_name TEXT NOT NULL DEFAULT ''"
      ");"
      "CREATE TABLE IF NOT EXISTS versions ("
      " id TEXT PRIMARY KEY NOT NULL,"
      " asset_id TEXT NOT NULL,"
      " stage TEXT NOT NULL DEFAULT '',"
      " version_number INTEGER NOT NULL DEFAULT 1,"
      " managed INTEGER NOT NULL DEFAULT 1,"
      " path TEXT NOT NULL DEFAULT '',"
      " source_uri TEXT NOT NULL DEFAULT '',"
      " sha256 TEXT NOT NULL DEFAULT '',"
      " file_name TEXT NOT NULL DEFAULT '',"
      " parent_version_ids_json TEXT NOT NULL DEFAULT '[]',"
      " extra_json TEXT NOT NULL DEFAULT '{}'"
      ");"
      "CREATE INDEX IF NOT EXISTS idx_versions_asset ON versions(asset_id);"
      "CREATE TABLE IF NOT EXISTS entity_asset_links ("
      " ord INTEGER PRIMARY KEY NOT NULL,"
      " entity_type TEXT NOT NULL DEFAULT '',"
      " entity_id TEXT NOT NULL DEFAULT '',"
      " asset_id TEXT NOT NULL DEFAULT '',"
      " role TEXT NOT NULL DEFAULT '',"
      " is_primary INTEGER NOT NULL DEFAULT 1,"
      " unresolved INTEGER NOT NULL DEFAULT 0,"
      " ordinal INTEGER NOT NULL DEFAULT 0,"
      " note TEXT NOT NULL DEFAULT ''"
      ");"
      "CREATE INDEX IF NOT EXISTS idx_links_entity_role "
      " ON entity_asset_links(entity_type, entity_id, role);"
      "CREATE INDEX IF NOT EXISTS idx_links_asset ON entity_asset_links(asset_id);"
      "CREATE TABLE IF NOT EXISTS catalog_meta ("
      " key TEXT PRIMARY KEY NOT NULL,"
      " value TEXT NOT NULL"
      ");";

  bool isSchemaReject(const QString &error)
  {
    return error.contains(QStringLiteral("newer than this build")) ||
           error.contains(QStringLiteral("unsupported catalog schema"));
  }

  void forgetConnection(const QString &name)
  {
    if (name.isEmpty() || !QSqlDatabase::contains(name))
      return;
    {
      QSqlDatabase db = QSqlDatabase::database(name, false);
      if (db.isValid())
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
  }

  bool execScript(QSqlDatabase &db, const char *sql, QString *error)
  {
    const QStringList parts = QString::fromUtf8(sql).split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (QString part : parts)
    {
      part = part.trimmed();
      if (part.isEmpty())
        continue;
      if (!execSql(db, part, error))
        return false;
    }
    return true;
  }

  bool applyWritablePragmas(QSqlDatabase &db, QString *error)
  {
    // WAL 是 catalog.sqlite 的例外（project.sqlite 仍不采用 WAL）。
    // synchronous=FULL：崩溃安全。禁止 synchronous=OFF。
    if (!execSql(db, QStringLiteral("PRAGMA busy_timeout = 5000"), error))
      return false;
    if (!execSql(db, QStringLiteral("PRAGMA journal_mode = WAL"), error))
      return false;
    if (!execSql(db, QStringLiteral("PRAGMA synchronous = FULL"), error))
      return false;
    return true;
  }

  bool readEpoch(QSqlDatabase &db, int *epoch, QString *error)
  {
    *epoch = 1; // 缺表 / 缺键 = 1，与缺 JSON schema_version 同口径
    QSqlQuery exists(db);
    if (!exists.exec(QStringLiteral(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name='catalog_meta'")))
    {
      setError(error, exists.lastError().text());
      return false;
    }
    if (!exists.next())
      return true;
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT value FROM catalog_meta WHERE key='schema_epoch'")))
    {
      setError(error, q.lastError().text());
      return false;
    }
    if (!q.next())
      return true;
    bool ok = false;
    const int v = q.value(0).toString().toInt(&ok);
    *epoch = (ok && v > 0) ? v : 1;
    return true;
  }

  bool integrityOk(QSqlDatabase &db, QString *detail)
  {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("PRAGMA integrity_check")))
    {
      setError(detail, q.lastError().text().isEmpty() ? QStringLiteral("integrity_check failed")
                                                      : q.lastError().text());
      return false;
    }
    bool any = false;
    while (q.next())
    {
      any = true;
      const QString row = q.value(0).toString();
      if (row.compare(QStringLiteral("ok"), Qt::CaseInsensitive) != 0)
      {
        setError(detail, row.isEmpty() ? QStringLiteral("integrity_check failed") : row);
        return false;
      }
    }
    if (!any)
    {
      setError(detail, QStringLiteral("integrity_check failed"));
      return false;
    }
    return true;
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

  // #79：只在 integrity 已通过、连接已关闭之后调用。损坏主文件不得进 .bak。
  bool rotateFiles(const QString &primary, int keep, QString *error)
  {
    if (!QFile::exists(primary))
      return true;
    const QString bak = primary + QStringLiteral(".bak");
    keep = qBound(1, keep, 9);
    if (keep == 1)
    {
      for (int g = 2; g <= 9; ++g)
        QFile::remove(bak + QStringLiteral(".%1").arg(g));
    }
    else
    {
      QFile::remove(bak + QStringLiteral(".%1").arg(keep));
      for (int gen = keep - 1; gen >= 2; --gen)
      {
        const QString src = bak + QStringLiteral(".%1").arg(gen);
        if (QFile::exists(src))
          paleoReplaceFile(src, bak + QStringLiteral(".%1").arg(gen + 1));
      }
      if (QFile::exists(bak))
        paleoReplaceFile(bak, bak + QStringLiteral(".2"));
    }
    const QString bakTmp = bak + QStringLiteral(".tmp");
    QFile::remove(bakTmp);
    if (!QFile::copy(primary, bakTmp))
    {
      setError(error, QStringLiteral("cannot copy %1 to backup tmp %2").arg(primary, bakTmp));
      return false;
    }
    if (!paleoReplaceFile(bakTmp, bak))
    {
      QFile::remove(bakTmp);
      setError(error, QStringLiteral("cannot rotate %1 to %2").arg(primary, bak));
      return false;
    }
    return true;
  }

  void removeSqliteFamily(const QString &path)
  {
    QFile::remove(path);
    QFile::remove(path + QStringLiteral("-wal"));
    QFile::remove(path + QStringLiteral("-shm"));
  }

  enum class JsonRead
  {
    Ok,
    Unsupported,
    Corrupt
  };

  JsonRead readJsonFile(const QString &path, CatalogStore::Tables *out, QString *error,
                        QString *parseDetail)
  {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
    {
      const QString why = f.errorString();
      if (parseDetail)
        *parseDetail = why;
      setError(error, QStringLiteral("cannot open catalog %1").arg(path));
      return JsonRead::Corrupt;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject())
    {
      if (parseDetail)
        *parseDetail = pe.errorString();
      setError(error, pe.errorString());
      return JsonRead::Corrupt;
    }
    QString ferr;
    if (!CatalogStore::fromJson(doc.object(), out, &ferr))
    {
      setError(error, ferr);
      if (parseDetail)
        *parseDetail = ferr;
      if (ferr.contains(QStringLiteral("unsupported catalog schema")))
        return JsonRead::Unsupported;
      return JsonRead::Corrupt;
    }
    return JsonRead::Ok;
  }

  QString migratedDest(const QString &jsonPath)
  {
    const QString base = jsonPath + QStringLiteral(".migrated");
    if (!QFileInfo::exists(base))
      return base;
    for (int i = 2; i < 100; ++i)
    {
      const QString alt = jsonPath + QStringLiteral(".migrated.%1").arg(i);
      if (!QFileInfo::exists(alt))
        return alt;
    }
    return jsonPath + QStringLiteral(".migrated-") +
           QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmsszzzZ"));
  }

  bool renameAside(const QString &src, const QString &dest)
  {
    if (!QFileInfo::exists(src))
      return true;
    if (QFile::rename(src, dest))
      return true;
    if (!QFile::copy(src, dest))
      return false;
    return QFile::remove(src);
  }
} // namespace

CatalogStore::~CatalogStore()
{
  close();
}

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

void CatalogStore::close()
{
  if (m_open && m_writable && !m_connectionName.isEmpty())
  {
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (db.isValid() && db.isOpen())
    {
      QSqlQuery q(db);
      q.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));
    }
  }
  m_inTxn = false;
  m_open = false;
  m_writable = false;
  if (m_connectionName.isEmpty())
    return;
  {
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (db.isValid())
      db.close();
  }
  if (QSqlDatabase::contains(m_connectionName))
    QSqlDatabase::removeDatabase(m_connectionName);
  m_connectionName.clear();
}

bool CatalogStore::begin(QString *error)
{
  if (m_inTxn)
    return true;
  if (!m_open || !m_writable)
  {
    setError(error, QStringLiteral("catalog sqlite is not open for write"));
    return false;
  }
  QSqlDatabase db = QSqlDatabase::database(m_connectionName);
  if (!execSql(db, QStringLiteral("BEGIN IMMEDIATE"), error))
    return false;
  m_inTxn = true;
  return true;
}

bool CatalogStore::commit(QString *error)
{
  if (!m_inTxn)
    return true;
  QSqlDatabase db = QSqlDatabase::database(m_connectionName);
  if (!execSql(db, QStringLiteral("COMMIT"), error))
    return false;
  m_inTxn = false;
  return true;
}

void CatalogStore::rollback()
{
  if (!m_inTxn || m_connectionName.isEmpty())
  {
    m_inTxn = false;
    return;
  }
  QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
  if (db.isValid() && db.isOpen())
  {
    QSqlQuery q(db);
    q.exec(QStringLiteral("ROLLBACK"));
  }
  m_inTxn = false;
}

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

bool CatalogStore::attachWritable(QString *error)
{
  if (m_sqlitePath.isEmpty())
  {
    setError(error, QStringLiteral("catalog sqlite path is empty"));
    return false;
  }
  m_connectionName = catalogConnectionName(this, m_sqlitePath, false);
  bool opened = false;
  {
    QSqlDatabase db = MetaStore::openConnection(m_sqlitePath, m_connectionName, error, false,
                                                kCatalogUserVersion);
    opened = db.isValid() && db.isOpen();
    if (opened && !applyWritablePragmas(db, error))
      opened = false;
    else if (opened && !execScript(db, kSchemaSql, error))
      opened = false;
  }
  if (!opened)
  {
    forgetConnection(m_connectionName);
    m_connectionName.clear();
    m_open = false;
    m_writable = false;
    return false;
  }
  m_open = true;
  m_writable = true;
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

bool CatalogStore::connectPrimary(bool readOnly, QString *error)
{
  m_connectionName = catalogConnectionName(this, m_sqlitePath, readOnly);
  QString local;
  bool opened = false;
  {
    QSqlDatabase db = MetaStore::openConnection(m_sqlitePath, m_connectionName, &local,
                                                readOnly, kCatalogUserVersion);
    opened = db.isValid() && db.isOpen();
  }
  if (!opened)
  {
    forgetConnection(m_connectionName);
    m_connectionName.clear();
    setError(error, local.isEmpty() ? QStringLiteral("cannot open catalog sqlite") : local);
    return false;
  }
  return true;
}

bool CatalogStore::createEmpty(Tables *out, QString *error)
{
  Tables tables;
  tables.meta.revision = 1;
  tables.meta.backupKeep = 3;
  if (!attachWritable(error))
  {
    removeSqliteFamily(m_sqlitePath);
    return false;
  }
  if (!replaceAll(tables, error))
  {
    close();
    removeSqliteFamily(m_sqlitePath);
    return false;
  }
  *out = tables;
  return true;
}

bool CatalogStore::migrateFromJson(Tables *out, QString *error)
{
  const QString jsonPath = jsonPathFor(m_projectDir);
  Tables parsed;
  QString parseDetail;
  QString perr;
  const JsonRead kind = readJsonFile(jsonPath, &parsed, &perr, &parseDetail);
  if (kind == JsonRead::Unsupported)
  {
    // 未来 JSON 不是损坏：不建 sqlite，不回退 .bak。
    setError(error, perr.contains(QStringLiteral("unsupported catalog schema"))
                        ? perr
                        : QStringLiteral("unsupported catalog schema"));
    return false;
  }

  bool fromBak = false;
  if (kind == JsonRead::Corrupt)
  {
    QString detail;
    bool got = false;
    for (int gen = 1; gen <= 9 && !got; ++gen)
    {
      const QString candidate = gen == 1 ? jsonPath + QStringLiteral(".bak")
                                         : jsonPath + QStringLiteral(".bak.%1").arg(gen);
      if (!QFileInfo::exists(candidate))
        continue;
      Tables loaded;
      QString berr;
      QString bdetail;
      const JsonRead br = readJsonFile(candidate, &loaded, &berr, &bdetail);
      if (br != JsonRead::Ok)
      {
        detail += QStringLiteral("%1: corrupt backup: %2; ")
                      .arg(candidate, bdetail.isEmpty() ? berr : bdetail);
        continue;
      }
      parsed = loaded;
      got = true;
    }
    if (!got)
    {
      setError(error, QStringLiteral("corrupt catalog %1: %2 (no usable %3[.2..9]: %4)")
                          .arg(jsonPath, parseDetail.isEmpty() ? perr : parseDetail,
                               jsonPath + QStringLiteral(".bak"),
                               detail.isEmpty() ? QStringLiteral("backup missing") : detail));
      return false;
    }
    fromBak = true;
    m_recovered = true;
    m_recoveryReason = QStringLiteral("%1: %2").arg(jsonPath, parseDetail);
  }

  if (!attachWritable(error))
  {
    removeSqliteFamily(m_sqlitePath);
    return false;
  }
  if (!replaceAll(parsed, error))
  {
    close();
    removeSqliteFamily(m_sqlitePath);
    return false;
  }

  if (fromBak)
  {
    const QString dest =
        jsonPath + QStringLiteral(".corrupt-") +
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmsszzzZ"));
    if (!renameAside(jsonPath, dest))
      qWarning("catalog: cannot quarantine corrupt json %s", qPrintable(jsonPath));
  }
  else
  {
    const QString dest = migratedDest(jsonPath);
    if (!renameAside(jsonPath, dest))
      qWarning("catalog: cannot rename %s to %s", qPrintable(jsonPath), qPrintable(dest));
    else
      m_migratedFromJson = true;
  }
  *out = parsed;
  return true;
}

bool CatalogStore::recover(const QString &primaryError, Tables *out, QString *error)
{
  // 连接不得留在 .bak 上。只读打开备份，装进内存后立刻关掉。
  QString detail;
  bool anyBak = false;
  for (int gen = 1; gen <= 9; ++gen)
  {
    const QString candidate = gen == 1 ? m_sqlitePath + QStringLiteral(".bak")
                                       : m_sqlitePath + QStringLiteral(".bak.%1").arg(gen);
    if (!QFileInfo::exists(candidate))
      continue;
    anyBak = true;
    // 只读打开 WAL 库会要 -shm 写权限，等于改备份现场。拷到临时文件再读，
    // .bak 本体不打开、不改。
    const QString tmp = candidate + QStringLiteral(".read.tmp");
    removeSqliteFamily(tmp);
    if (!QFile::copy(candidate, tmp))
    {
      detail += QStringLiteral("%1: corrupt backup: cannot copy; ").arg(candidate);
      continue;
    }
    const QString name = catalogConnectionName(this, tmp, false);
    QString localErr;
    bool loadedOk = false;
    Tables loaded;
    {
      QSqlDatabase db = MetaStore::openConnection(tmp, name, &localErr, false,
                                                  kCatalogUserVersion);
      if (!db.isValid() || !db.isOpen())
      {
        // 打开失败：下面 forget。localErr 已填。
      }
      else
      {
        int epoch = 1;
        QString integ;
        if (!readEpoch(db, &epoch, &localErr))
          loadedOk = false;
        else if (epoch > kSchemaEpoch)
        {
          localErr = QStringLiteral("unsupported catalog schema epoch %1").arg(epoch);
          loadedOk = false;
        }
        else if (!integrityOk(db, &integ))
        {
          localErr = integ.isEmpty() ? QStringLiteral("integrity_check failed") : integ;
          loadedOk = false;
        }
        else
          loadedOk = loadTables(db, &loaded, &localErr);
      }
    }
    forgetConnection(name);
    removeSqliteFamily(tmp);
    if (!loadedOk)
    {
      detail += QStringLiteral("%1: corrupt backup: %2; ").arg(candidate, localErr);
      continue;
    }
    *out = loaded;
    m_recovered = true;
    m_recoveryReason = QStringLiteral("%1: %2").arg(m_sqlitePath, primaryError);
    m_open = false;
    m_writable = false;
    m_connectionName.clear();
    return true;
  }

  // 主库和 sqlite.bak 都不可用时，最后试 catalog.json / catalog.json.migrated。
  // 迁完后、下一次 open 快照之前崩溃，这两份还是上一代可读文本。
  const QString jsonPath = jsonPathFor(m_projectDir);
  const QStringList jsons{jsonPath, jsonPath + QStringLiteral(".migrated")};
  for (const QString &jp : jsons)
  {
    if (!QFileInfo::exists(jp))
      continue;
    Tables loaded;
    QString jerr;
    if (readJsonFile(jp, &loaded, &jerr, nullptr) != JsonRead::Ok)
      continue;
    *out = loaded;
    m_recovered = true;
    m_recoveryReason = QStringLiteral("%1: %2").arg(m_sqlitePath, primaryError);
    m_open = false;
    m_writable = false;
    m_connectionName.clear();
    return true;
  }

  setError(error, QStringLiteral("corrupt catalog %1: %2 (no usable %3[.2..9]: %4)")
                      .arg(m_sqlitePath, primaryError, m_sqlitePath + QStringLiteral(".bak"),
                           (!anyBak || detail.isEmpty()) ? QStringLiteral("backup missing")
                                                         : detail));
  return false;
}

bool CatalogStore::openExisting(bool readOnly, Tables *out, QString *error)
{
  QString openErr;
  if (!connectPrimary(readOnly, &openErr))
  {
    if (isSchemaReject(openErr))
    {
      setError(error, openErr);
      return false;
    }
    return recover(openErr, out, error);
  }

  enum class Step
  {
    Ready,
    Schema,
    Corrupt,
    Failed
  };
  Step step = Step::Ready;
  QString stepErr;
  int keep = 3;
  bool doRotate = false;
  {
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    int epoch = 1;
    if (!readEpoch(db, &epoch, &stepErr))
      step = Step::Corrupt;
    else if (epoch > kSchemaEpoch)
    {
      step = Step::Schema;
      stepErr = QStringLiteral("unsupported catalog schema epoch %1").arg(epoch);
    }
    else if (!readOnly && !applyWritablePragmas(db, &stepErr))
      step = Step::Failed;
    else if (!readOnly && !execScript(db, kSchemaSql, &stepErr))
      step = Step::Failed;
    else if (!integrityOk(db, &stepErr))
      step = Step::Corrupt;
    else if (!readOnly)
    {
      {
        QSqlQuery q(db);
        if (q.exec(QStringLiteral(
                "SELECT value FROM catalog_meta WHERE key='backup_keep'")) &&
            q.next())
          keep = qBound(1, q.value(0).toString().toInt(), 9);
      }
      bool ckptOk = false;
      {
        QSqlQuery cq(db);
        if (cq.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)")) && cq.next())
          ckptOk = cq.value(0).toInt() == 0;
      }
      if (!ckptOk)
        qWarning("catalog: wal_checkpoint failed for %s; skipping backup copy",
                 qPrintable(m_sqlitePath));
      else
        doRotate = true;
      if (!doRotate)
      {
        if (!loadTables(db, out, &stepErr))
          step = Step::Failed;
        else
        {
          m_open = true;
          m_writable = true;
        }
      }
    }
    else if (!loadTables(db, out, &stepErr))
      step = Step::Failed;
    else
    {
      m_open = true;
      m_writable = false;
    }
  }

  if (step == Step::Schema)
  {
    close();
    setError(error, stepErr);
    return false;
  }
  if (step == Step::Corrupt)
  {
    close();
    return recover(stepErr.isEmpty() ? QStringLiteral("integrity_check failed") : stepErr, out,
                   error);
  }
  if (step == Step::Failed)
  {
    close();
    setError(error, stepErr);
    return false;
  }
  if (!doRotate)
    return m_open;

  // 拷贝前必须关掉连接（Windows 共享锁）。integrity 已通过，坏库不会进 .bak。
  // 本次 open 新建的库不走这里（create / JSON 迁移直接返回）。
  const QString path = m_sqlitePath;
  close();
  QString rotErr;
  if (!rotateFiles(path, keep, &rotErr))
    qWarning("catalog: backup copy failed for %s: %s", qPrintable(path), qPrintable(rotErr));
  if (!attachWritable(error))
    return false;
  bool loaded = false;
  {
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    loaded = loadTables(db, out, error);
  }
  if (!loaded)
  {
    close();
    return false;
  }
  return true;
}

bool CatalogStore::openProject(const QString &projectDir, bool readOnly, Tables *out,
                               QString *error)
{
  close();
  m_recovered = false;
  m_recoveryReason.clear();
  m_migratedFromJson = false;
  m_projectDir = projectDir.trimmed();
  if (!out)
  {
    setError(error, QStringLiteral("catalog destination is null"));
    return false;
  }
  *out = Tables();
  if (m_projectDir.isEmpty())
  {
    setError(error, QStringLiteral("project directory is empty"));
    return false;
  }
  m_sqlitePath = sqlitePathFor(m_projectDir);
  const QString jsonPath = jsonPathFor(m_projectDir);
  const bool haveSqlite = QFileInfo::exists(m_sqlitePath);
  const bool haveJson = QFileInfo::exists(jsonPath);

  if (!haveSqlite)
  {
    if (readOnly)
    {
      if (!haveJson)
        return true; // #80：不建文件，查询为空
      QString parseDetail;
      QString perr;
      const JsonRead kind = readJsonFile(jsonPath, out, &perr, &parseDetail);
      if (kind == JsonRead::Ok)
        return true;
      if (kind == JsonRead::Unsupported)
      {
        setError(error, perr.contains(QStringLiteral("unsupported catalog schema"))
                            ? perr
                            : QStringLiteral("unsupported catalog schema"));
        *out = Tables();
        return false;
      }
      // 只读：可读 .bak 进内存，不隔离、不建 sqlite。
      QString detail;
      for (int gen = 1; gen <= 9; ++gen)
      {
        const QString candidate = gen == 1 ? jsonPath + QStringLiteral(".bak")
                                           : jsonPath + QStringLiteral(".bak.%1").arg(gen);
        if (!QFileInfo::exists(candidate))
          continue;
        Tables loaded;
        QString berr;
        QString bdetail;
        if (readJsonFile(candidate, &loaded, &berr, &bdetail) != JsonRead::Ok)
        {
          detail += QStringLiteral("%1: corrupt backup: %2; ")
                        .arg(candidate, bdetail.isEmpty() ? berr : bdetail);
          continue;
        }
        *out = loaded;
        m_recovered = true;
        m_recoveryReason = QStringLiteral("%1: %2").arg(jsonPath, parseDetail);
        return true;
      }
      *out = Tables();
      setError(error, QStringLiteral("corrupt catalog %1: %2 (no usable %3[.2..9]: %4)")
                          .arg(jsonPath, parseDetail.isEmpty() ? perr : parseDetail,
                               jsonPath + QStringLiteral(".bak"),
                               detail.isEmpty() ? QStringLiteral("backup missing") : detail));
      return false;
    }
    if (haveJson)
      return migrateFromJson(out, error);
    return createEmpty(out, error);
  }
  return openExisting(readOnly, out, error);
}

bool CatalogStore::rewritePrimary(const Tables &tables, bool quarantineCorrupt, QString *error)
{
  const QString path = m_sqlitePath;
  if (path.isEmpty())
  {
    setError(error, QStringLiteral("catalog sqlite path is empty"));
    return false;
  }
  close();
  if (quarantineCorrupt)
  {
    const QString stamp =
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmsszzzZ"));
    const QStringList sources{path, path + QStringLiteral("-wal"), path + QStringLiteral("-shm")};
    for (const QString &src : sources)
    {
      if (!QFileInfo::exists(src))
        continue;
      QString dest;
      if (src.endsWith(QStringLiteral("-wal")))
        dest = path + QStringLiteral("-wal.corrupt-") + stamp;
      else if (src.endsWith(QStringLiteral("-shm")))
        dest = path + QStringLiteral("-shm.corrupt-") + stamp;
      else
        dest = path + QStringLiteral(".corrupt-") + stamp;
      if (!QFileInfo::exists(dest) && !QFile::copy(src, dest))
      {
        setError(error, QStringLiteral("cannot quarantine %1").arg(src));
        return false;
      }
      if (!QFile::remove(src))
      {
        setError(error, QStringLiteral("cannot remove quarantined %1").arg(src));
        return false;
      }
    }
  }
  m_sqlitePath = path;
  if (!attachWritable(error))
    return false;
  if (!replaceAll(tables, error))
    return false;
  m_recovered = false;
  m_recoveryReason.clear();
  return true;
}
