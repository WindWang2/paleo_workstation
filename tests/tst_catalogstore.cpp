#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "../src/catalog/catalogstore.h"
#include "../src/metadata/metastore.h"

// catalog.sqlite 持久层。不改 PRAGMA synchronous（跟着进程环境，期望 FULL = 2）。

namespace
{

QString uniqueConn(const char *prefix)
{
  static int n = 0;
  return QStringLiteral("tst_catalogstore_%1_%2").arg(QLatin1String(prefix)).arg(++n);
}

bool writeMetadataFile(const QString &projectDir, const QString &fileName, const QByteArray &bytes,
                       QString *error)
{
  const QString dirPath = QDir(projectDir).filePath(QStringLiteral("artifacts/metadata"));
  if (!QDir().mkpath(dirPath))
  {
    if (error)
      *error = QStringLiteral("mkpath failed: %1").arg(dirPath);
    return false;
  }
  QFile f(QDir(dirPath).filePath(fileName));
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    if (error)
      *error = f.errorString();
    return false;
  }
  if (f.write(bytes) != static_cast<qint64>(bytes.size()))
  {
    if (error)
      *error = f.errorString();
    return false;
  }
  if (!f.flush())
  {
    if (error)
      *error = f.errorString();
    return false;
  }
  return true;
}

QByteArray readAllBytes(const QString &path, bool *ok)
{
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
  {
    if (ok)
      *ok = false;
    return {};
  }
  if (ok)
    *ok = true;
  return f.readAll();
}

bool hasEntityId(const CatalogStore::Tables &tables, const QString &id)
{
  for (const CatalogEntity &e : tables.entities)
    if (e.id == id)
      return true;
  return false;
}

bool hasVersionId(const CatalogStore::Tables &tables, const QString &id)
{
  for (const CatalogVersion &v : tables.versions)
    if (v.id == id)
      return true;
  return false;
}

QStringList filesNamed(const QString &sqlitePath, const QString &pattern)
{
  return QFileInfo(sqlitePath).dir().entryList(QStringList{pattern}, QDir::Files, QDir::Name);
}

// removeDatabase 只能发生在 QSqlDatabase / QSqlQuery 都析构之后。
bool execSql(const QString &path, const QString &sql, QString *error)
{
  const QString conn = uniqueConn("exec");
  bool ok = false;
  {
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
    db.setDatabaseName(path);
    if (!db.open())
    {
      if (error)
        *error = db.lastError().text();
    }
    else
    {
      QSqlQuery q(db);
      ok = q.exec(sql);
      if (!ok && error)
        *error = q.lastError().text().isEmpty() ? sql : q.lastError().text();
      q.finish();
      db.close();
    }
  }
  if (QSqlDatabase::contains(conn))
    QSqlDatabase::removeDatabase(conn);
  return ok;
}

bool sqlScalar(const QString &path, const QString &sql, QVariant *out, QString *error)
{
  const QString conn = uniqueConn("scalar");
  bool ok = false;
  {
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
    db.setDatabaseName(path);
    if (!db.open())
    {
      if (error)
        *error = db.lastError().text();
    }
    else
    {
      QSqlQuery q(db);
      if (!q.exec(sql) || !q.next())
      {
        if (error)
          *error = q.lastError().text().isEmpty() ? sql : q.lastError().text();
      }
      else
      {
        if (out)
          *out = q.value(0);
        ok = true;
      }
      q.finish();
      db.close();
    }
  }
  if (QSqlDatabase::contains(conn))
    QSqlDatabase::removeDatabase(conn);
  return ok;
}

struct SqliteProbe
{
  int entities = -1;
  int assets = -1;
  int versions = -1;
  int links = -1;
  QString journalMode;
  int synchronous = -1;
  int userVersion = -1;
  bool ok = false;
  QString error;
};

SqliteProbe probeDb(const QString &path)
{
  SqliteProbe p;
  const QString conn = uniqueConn("probe");
  {
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
    db.setDatabaseName(path);
    if (!db.open())
    {
      p.error = db.lastError().text();
    }
    else
    {
      auto one = [&](const QString &sql, QVariant *out) -> bool {
        QSqlQuery q(db);
        if (!q.exec(sql) || !q.next())
        {
          p.error = q.lastError().text().isEmpty() ? sql : q.lastError().text();
          return false;
        }
        *out = q.value(0);
        return true;
      };
      QVariant v;
      bool ok = true;
      ok = ok && one(QStringLiteral("SELECT COUNT(*) FROM entities"), &v);
      if (ok)
        p.entities = v.toInt();
      ok = ok && one(QStringLiteral("SELECT COUNT(*) FROM assets"), &v);
      if (ok)
        p.assets = v.toInt();
      ok = ok && one(QStringLiteral("SELECT COUNT(*) FROM versions"), &v);
      if (ok)
        p.versions = v.toInt();
      ok = ok && one(QStringLiteral("SELECT COUNT(*) FROM entity_asset_links"), &v);
      if (ok)
        p.links = v.toInt();
      ok = ok && one(QStringLiteral("PRAGMA journal_mode"), &v);
      if (ok)
        p.journalMode = v.toString();
      ok = ok && one(QStringLiteral("PRAGMA synchronous"), &v);
      if (ok)
        p.synchronous = v.toInt();
      ok = ok && one(QStringLiteral("PRAGMA user_version"), &v);
      if (ok)
        p.userVersion = v.toInt();
      p.ok = ok;
      db.close();
    }
  }
  if (QSqlDatabase::contains(conn))
    QSqlDatabase::removeDatabase(conn);
  return p;
}

bool setUserVersion(const QString &path, int version, QString *error)
{
  return execSql(path, QStringLiteral("PRAGMA user_version = %1").arg(version), error);
}

bool forceSchemaEpoch(const QString &path, const QString &epoch, QString *error)
{
  const QString conn = uniqueConn("epoch");
  bool ok = false;
  {
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
    db.setDatabaseName(path);
    if (!db.open())
    {
      if (error)
        *error = db.lastError().text();
    }
    else
    {
      QSqlQuery sel(db);
      const bool selected = sel.exec(QStringLiteral(
          "SELECT value FROM catalog_meta WHERE key='schema_epoch'"));
      const QString selectError = sel.lastError().text();
      const bool exists = selected && sel.next();
      sel.finish();
      if (!selected)
      {
        if (error)
          *error = selectError;
      }
      else if (exists)
      {
        QSqlQuery upd(db);
        upd.prepare(QStringLiteral(
            "UPDATE catalog_meta SET value=? WHERE key='schema_epoch'"));
        upd.addBindValue(epoch);
        ok = upd.exec();
        if (!ok && error)
          *error = upd.lastError().text();
        upd.finish();
      }
      else
      {
        QSqlQuery ins(db);
        ins.prepare(QStringLiteral(
            "INSERT INTO catalog_meta (key, value) VALUES ('schema_epoch', ?)"));
        ins.addBindValue(epoch);
        ok = ins.exec();
        if (!ok && error)
          *error = ins.lastError().text();
        ins.finish();
      }
      db.close();
    }
  }
  if (QSqlDatabase::contains(conn))
    QSqlDatabase::removeDatabase(conn);
  return ok;
}

QByteArray goldenCatalogJson()
{
  QJsonObject well;
  well.insert(QStringLiteral("id"), QStringLiteral("well-A1"));
  well.insert(QStringLiteral("entity_type"), QStringLiteral("well"));
  well.insert(QStringLiteral("name"), QStringLiteral("A1"));
  well.insert(QStringLiteral("surface_x"), 5288.67);
  well.insert(QStringLiteral("surface_y"), 100.5);
  well.insert(QStringLiteral("has_surface"), true);
  well.insert(QStringLiteral("kb"), 12.25);
  well.insert(QStringLiteral("td"), 3200);
  well.insert(QStringLiteral("coordinate_status"), QStringLiteral("ok"));
  QJsonObject corner;
  corner.insert(QStringLiteral("x"), 1.5);
  corner.insert(QStringLiteral("y"), 2.5);
  QJsonArray corners;
  corners.append(corner);
  well.insert(QStringLiteral("corners"), corners);
  QJsonObject wellExtra;
  wellExtra.insert(QStringLiteral("marker"), QStringLiteral("red"));
  wellExtra.insert(QStringLiteral("depth"), 5288.67);
  well.insert(QStringLiteral("extra"), wellExtra);

  QJsonObject survey;
  survey.insert(QStringLiteral("id"), QStringLiteral("sv-1"));
  survey.insert(QStringLiteral("entity_type"), QStringLiteral("seismic_survey"));
  survey.insert(QStringLiteral("name"), QStringLiteral("S"));
  survey.insert(QStringLiteral("inline_min"), 1);
  survey.insert(QStringLiteral("inline_max"), 10);
  survey.insert(QStringLiteral("xline_min"), 2);
  survey.insert(QStringLiteral("xline_max"), 20);
  survey.insert(QStringLiteral("sample_interval_us"), 4000);
  survey.insert(QStringLiteral("start_time_ms"), 0);

  QJsonObject asset;
  asset.insert(QStringLiteral("id"), QStringLiteral("ast-7"));
  asset.insert(QStringLiteral("type"), QStringLiteral("well_log"));
  asset.insert(QStringLiteral("format"), QStringLiteral("las"));
  asset.insert(QStringLiteral("display_name"), QStringLiteral("A1.las"));

  QJsonObject ver3;
  ver3.insert(QStringLiteral("id"), QStringLiteral("ver-3"));
  ver3.insert(QStringLiteral("asset_id"), QStringLiteral("ast-7"));
  ver3.insert(QStringLiteral("stage"), QStringLiteral("RAW"));
  ver3.insert(QStringLiteral("version_number"), 2);
  ver3.insert(QStringLiteral("managed"), true);
  ver3.insert(QStringLiteral("path"), QStringLiteral("raw/ast-7/ver-3/A1.las"));
  ver3.insert(QStringLiteral("source_uri"), QStringLiteral("file:///tmp/A1.las"));
  ver3.insert(QStringLiteral("sha256"), QString(64, QLatin1Char('0')));
  ver3.insert(QStringLiteral("file_name"), QStringLiteral("A1.las"));
  QJsonArray parents;
  parents.append(QStringLiteral("ver-1"));
  ver3.insert(QStringLiteral("parent_version_ids"), parents);
  QJsonObject verExtra;
  verExtra.insert(QStringLiteral("n"), 1);
  ver3.insert(QStringLiteral("extra"), verExtra);

  // fromJson 丢掉含 ".." 段的受管路径。迁入库里也不该出现。
  QJsonObject ver9;
  ver9.insert(QStringLiteral("id"), QStringLiteral("ver-9"));
  ver9.insert(QStringLiteral("asset_id"), QStringLiteral("ast-7"));
  ver9.insert(QStringLiteral("stage"), QStringLiteral("RAW"));
  ver9.insert(QStringLiteral("version_number"), 1);
  ver9.insert(QStringLiteral("managed"), true);
  ver9.insert(QStringLiteral("path"), QStringLiteral("../escape.las"));
  ver9.insert(QStringLiteral("file_name"), QStringLiteral("ok.las"));

  QJsonObject link0;
  link0.insert(QStringLiteral("entity_type"), QStringLiteral("well"));
  link0.insert(QStringLiteral("entity_id"), QStringLiteral("well-A1"));
  link0.insert(QStringLiteral("asset_id"), QStringLiteral("ast-7"));
  link0.insert(QStringLiteral("role"), QStringLiteral("well_log"));
  link0.insert(QStringLiteral("is_primary"), true);
  link0.insert(QStringLiteral("unresolved"), false);
  link0.insert(QStringLiteral("ordinal"), 1);
  link0.insert(QStringLiteral("note"), QStringLiteral("曲线；备注"));

  // 未决链接：实体 id 空。entity_type / asset_id 规格没写，按井曲线挂到 ast-7。
  QJsonObject link1;
  link1.insert(QStringLiteral("entity_type"), QStringLiteral("well"));
  link1.insert(QStringLiteral("entity_id"), QString());
  link1.insert(QStringLiteral("asset_id"), QStringLiteral("ast-7"));
  link1.insert(QStringLiteral("role"), QStringLiteral("well_log"));
  link1.insert(QStringLiteral("is_primary"), false);
  link1.insert(QStringLiteral("unresolved"), true);
  link1.insert(QStringLiteral("ordinal"), 0);
  link1.insert(QStringLiteral("note"), QStringLiteral("未匹配"));

  QJsonArray entities;
  entities.append(well);
  entities.append(survey);
  QJsonArray assets;
  assets.append(asset);
  QJsonArray versions;
  versions.append(ver3);
  versions.append(ver9);
  QJsonArray links;
  links.append(link0);
  links.append(link1);

  QJsonObject root;
  root.insert(QStringLiteral("schema_version"), 1);
  root.insert(QStringLiteral("catalog_revision"), 4);
  root.insert(QStringLiteral("entities"), entities);
  root.insert(QStringLiteral("assets"), assets);
  root.insert(QStringLiteral("versions"), versions);
  root.insert(QStringLiteral("entity_asset_links"), links);
  return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

#define CHECK_EQ(got, exp)                                                                         \
  do                                                                                               \
  {                                                                                                \
    if (!QTest::qCompare((got), (exp), #got, #exp, __FILE__, __LINE__))                           \
      return false;                                                                                \
  } while (false)

bool sameEntity(const CatalogEntity &got, const CatalogEntity &exp)
{
  CHECK_EQ(got.id, exp.id);
  CHECK_EQ(got.entityType, exp.entityType);
  CHECK_EQ(got.name, exp.name);
  CHECK_EQ(got.surfaceX, exp.surfaceX);
  CHECK_EQ(got.surfaceY, exp.surfaceY);
  CHECK_EQ(got.hasSurface, exp.hasSurface);
  CHECK_EQ(got.kb, exp.kb);
  CHECK_EQ(got.td, exp.td);
  CHECK_EQ(got.coordinateStatus, exp.coordinateStatus);
  CHECK_EQ(got.inlineMin, exp.inlineMin);
  CHECK_EQ(got.inlineMax, exp.inlineMax);
  CHECK_EQ(got.xlineMin, exp.xlineMin);
  CHECK_EQ(got.xlineMax, exp.xlineMax);
  CHECK_EQ(got.sampleIntervalUs, exp.sampleIntervalUs);
  CHECK_EQ(got.startTimeMs, exp.startTimeMs);
  CHECK_EQ(got.corners.size(), exp.corners.size());
  for (int i = 0; i < got.corners.size(); ++i)
  {
    const double gotX = got.corners.at(i).first;
    const double gotY = got.corners.at(i).second;
    const double expX = exp.corners.at(i).first;
    const double expY = exp.corners.at(i).second;
    CHECK_EQ(gotX, expX);
    CHECK_EQ(gotY, expY);
  }
  CHECK_EQ(got.extra, exp.extra);
  return true;
}

bool sameAsset(const CatalogAsset &got, const CatalogAsset &exp)
{
  CHECK_EQ(got.id, exp.id);
  CHECK_EQ(got.type, exp.type);
  CHECK_EQ(got.format, exp.format);
  CHECK_EQ(got.displayName, exp.displayName);
  return true;
}

bool sameVersion(const CatalogVersion &got, const CatalogVersion &exp)
{
  CHECK_EQ(got.id, exp.id);
  CHECK_EQ(got.assetId, exp.assetId);
  CHECK_EQ(got.stage, exp.stage);
  CHECK_EQ(got.versionNumber, exp.versionNumber);
  CHECK_EQ(got.managed, exp.managed);
  CHECK_EQ(got.path, exp.path);
  CHECK_EQ(got.sourceUri, exp.sourceUri);
  CHECK_EQ(got.sha256, exp.sha256);
  CHECK_EQ(got.fileName, exp.fileName);
  CHECK_EQ(got.parentVersionIds, exp.parentVersionIds);
  CHECK_EQ(got.extra, exp.extra);
  return true;
}

bool sameLink(const EntityAssetLink &got, const EntityAssetLink &exp)
{
  CHECK_EQ(got.entityType, exp.entityType);
  CHECK_EQ(got.entityId, exp.entityId);
  CHECK_EQ(got.assetId, exp.assetId);
  CHECK_EQ(got.role, exp.role);
  CHECK_EQ(got.isPrimary, exp.isPrimary);
  CHECK_EQ(got.unresolved, exp.unresolved);
  CHECK_EQ(got.ordinal, exp.ordinal);
  CHECK_EQ(got.note, exp.note);
  return true;
}

bool sameTables(const CatalogStore::Tables &got, const CatalogStore::Tables &exp)
{
  CHECK_EQ(got.entities.size(), exp.entities.size());
  for (int i = 0; i < got.entities.size(); ++i)
    if (!sameEntity(got.entities.at(i), exp.entities.at(i)))
      return false;
  CHECK_EQ(got.assets.size(), exp.assets.size());
  for (int i = 0; i < got.assets.size(); ++i)
    if (!sameAsset(got.assets.at(i), exp.assets.at(i)))
      return false;
  CHECK_EQ(got.versions.size(), exp.versions.size());
  for (int i = 0; i < got.versions.size(); ++i)
    if (!sameVersion(got.versions.at(i), exp.versions.at(i)))
      return false;
  CHECK_EQ(got.links.size(), exp.links.size());
  for (int i = 0; i < got.links.size(); ++i)
    if (!sameLink(got.links.at(i), exp.links.at(i)))
      return false;
  return true;
}

#undef CHECK_EQ

CatalogEntity wellNamed(const QString &id, const QString &name)
{
  CatalogEntity e;
  e.id = id;
  e.entityType = QStringLiteral("well");
  e.name = name;
  return e;
}

bool commitEntity(CatalogStore *store, const CatalogEntity &e, QString *error)
{
  if (!store->begin(error))
    return false;
  if (!store->upsertEntity(e, error))
    return false;
  return store->commit(error);
}

} // namespace

class TestCatalogStore : public QObject
{
  Q_OBJECT

private slots:
  void goldenJsonMigratesFieldForField();
  void readOnlyMissingCreatesNothing();
  void unsupportedJsonSchemaDoesNotCreateSqlite();
  void corruptJsonWithoutBackupDoesNotCreateSqlite();
  void secondOpenSnapshotsBakAndRecoverySkipsNewerCommits();
  void futureUserVersionRefusesWithoutBak();
  void futureSchemaEpochRefusesWithoutBak();
  void unsafeVersionInsertedLaterIsSkippedButKept();
  void rollbackLeavesSqliteUnchanged();
};

void TestCatalogStore::goldenJsonMigratesFieldForField()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QByteArray original = goldenCatalogJson();
  QString err;
  QVERIFY2(writeMetadataFile(dir.path(), QStringLiteral("catalog.json"), original, &err),
           qPrintable(err));

  const QString jsonPath = CatalogStore::jsonPathFor(dir.path());
  const QString migratedPath = jsonPath + QStringLiteral(".migrated");
  const QString sqlitePath = CatalogStore::sqlitePathFor(dir.path());
  const QString bakPath = sqlitePath + QStringLiteral(".bak");

  {
    CatalogStore store;
    CatalogStore::Tables tables;
    QVERIFY2(store.openProject(dir.path(), false, &tables, &err), qPrintable(err));
    QVERIFY(store.migratedFromJson());
    QVERIFY(!store.recovered());
    QVERIFY(store.isWritable());
    QVERIFY(!store.needsPrimaryRewrite());
    QVERIFY(!QFile::exists(jsonPath));
    QVERIFY(QFile::exists(migratedPath));
    QVERIFY(QFile::exists(sqlitePath));
    QVERIFY(!QFile::exists(bakPath));
    store.close();
  }

  bool migratedOk = false;
  const QByteArray migratedBytes = readAllBytes(migratedPath, &migratedOk);
  QVERIFY(migratedOk);
  QCOMPARE(migratedBytes, original);

  QJsonParseError parseError;
  const QJsonDocument doc = QJsonDocument::fromJson(original, &parseError);
  QCOMPARE(parseError.error, QJsonParseError::NoError);
  QVERIFY(doc.isObject());
  CatalogStore::Tables expected;
  QVERIFY2(CatalogStore::fromJson(doc.object(), &expected, &err), qPrintable(err));
  QCOMPARE(expected.versions.size(), 1);
  QCOMPARE(expected.versions.at(0).id, QStringLiteral("ver-3"));
  QVERIFY(!hasVersionId(expected, QStringLiteral("ver-9")));
  // ver-9 在 fromJson 里被丢掉，不参与 versionSeq。
  QCOMPARE(expected.meta.versionSeq, 3);
  QCOMPARE(expected.meta.assetSeq, 7);
  QCOMPARE(expected.meta.revision, 4);

  {
    CatalogStore store;
    CatalogStore::Tables got;
    QVERIFY2(store.openProject(dir.path(), false, &got, &err), qPrintable(err));
    QCOMPARE(got.entities.size(), 2);
    QCOMPARE(got.assets.size(), 1);
    QCOMPARE(got.versions.size(), 1);
    QCOMPARE(got.links.size(), 2);
    QCOMPARE(got.entities.at(0).id, QStringLiteral("well-A1"));
    QCOMPARE(got.entities.at(0).surfaceX, 5288.67);
    QCOMPARE(got.entities.at(0).extra.value(QStringLiteral("marker")).toString(),
             QStringLiteral("red"));
    QCOMPARE(got.entities.at(0).extra.value(QStringLiteral("depth")).toDouble(), 5288.67);
    QCOMPARE(got.versions.at(0).id, QStringLiteral("ver-3"));
    QVERIFY(!hasVersionId(got, QStringLiteral("ver-9")));
    QCOMPARE(got.links.at(0).note, QStringLiteral("曲线；备注"));
    QVERIFY(!got.links.at(0).unresolved);
    QVERIFY(got.links.at(1).unresolved);
    QCOMPARE(got.links.at(1).entityId, QString());
    QCOMPARE(got.links.at(1).note, QStringLiteral("未匹配"));
    QCOMPARE(got.meta.revision, 4);
    // 数值 meta 跟 fromJson。hasMutationSeq / hasBackupKeep 是「键在不在」，
    // JSON 路径强制 false，sqlite 读回可能因 writeMeta 写过键而为 true，不断言。
    QCOMPARE(got.meta.mutationSeq, expected.meta.mutationSeq);
    QCOMPARE(got.meta.assetSeq, expected.meta.assetSeq);
    QCOMPARE(got.meta.versionSeq, expected.meta.versionSeq);
    QCOMPARE(got.meta.backupKeep, expected.meta.backupKeep);
    QVERIFY(sameTables(got, expected));
    store.close();
  }

  const SqliteProbe probe = probeDb(sqlitePath);
  QVERIFY2(probe.ok, qPrintable(probe.error));
  QCOMPARE(probe.entities, 2);
  QCOMPARE(probe.assets, 1);
  QCOMPARE(probe.versions, 1);
  QCOMPARE(probe.links, 2);
  QCOMPARE(probe.journalMode.trimmed().toLower(), QStringLiteral("wal"));
  QCOMPARE(probe.synchronous, 2);
  QCOMPARE(probe.userVersion, 1);

  {
    CatalogStore store;
    CatalogStore::Tables tables;
    QVERIFY2(store.openProject(dir.path(), false, &tables, &err), qPrintable(err));
    QVERIFY(store.isWritable());
    QVERIFY2(commitEntity(&store, wellNamed(QStringLiteral("well-B"), QStringLiteral("B")), &err),
             qPrintable(err));
    store.close();
  }

  bool afterOk = false;
  const QByteArray after = readAllBytes(migratedPath, &afterOk);
  QVERIFY(afterOk);
  QCOMPARE(after, migratedBytes);
  QVERIFY(!QFile::exists(jsonPath));
}

void TestCatalogStore::readOnlyMissingCreatesNothing()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QString err;
  CatalogStore store;
  CatalogStore::Tables tables;
  QVERIFY2(store.openProject(dir.path(), true, &tables, &err), qPrintable(err));
  QCOMPARE(tables.entities.size(), 0);
  QCOMPARE(tables.assets.size(), 0);
  QCOMPARE(tables.versions.size(), 0);
  QCOMPARE(tables.links.size(), 0);
  QVERIFY(!store.isWritable());
  QVERIFY(!QFile::exists(CatalogStore::sqlitePathFor(dir.path())));
  QVERIFY(!QFile::exists(CatalogStore::jsonPathFor(dir.path())));
  store.close();

  const QString missing = dir.path() + QStringLiteral("/missing/nested/project.sqlite");
  QString roErr;
  bool valid = true;
  bool opened = true;
  {
    const QSqlDatabase db = MetaStore::openConnection(
        missing, QStringLiteral("tst_ro_missing"), &roErr, true);
    valid = db.isValid();
    opened = db.isOpen();
  }
  if (QSqlDatabase::contains(QStringLiteral("tst_ro_missing")))
    QSqlDatabase::removeDatabase(QStringLiteral("tst_ro_missing"));
  QVERIFY(!valid);
  QVERIFY(!opened);
  QVERIFY2(roErr.contains(QStringLiteral("does not exist")), qPrintable(roErr));
  QVERIFY(!QFileInfo::exists(missing));
  QVERIFY(!QDir(dir.path() + QStringLiteral("/missing")).exists());
}

void TestCatalogStore::unsupportedJsonSchemaDoesNotCreateSqlite()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QJsonObject root;
  root.insert(QStringLiteral("schema_version"), 999);
  root.insert(QStringLiteral("catalog_revision"), 7);
  const QByteArray original = QJsonDocument(root).toJson(QJsonDocument::Compact);
  QString err;
  QVERIFY2(writeMetadataFile(dir.path(), QStringLiteral("catalog.json"), original, &err),
           qPrintable(err));

  const QString jsonPath = CatalogStore::jsonPathFor(dir.path());
  CatalogStore store;
  CatalogStore::Tables tables;
  const bool opened = store.openProject(dir.path(), false, &tables, &err);
  QVERIFY2(!opened, qPrintable(err));
  QVERIFY2(err.contains(QStringLiteral("unsupported catalog schema")), qPrintable(err));
  QVERIFY(!QFile::exists(CatalogStore::sqlitePathFor(dir.path())));
  bool ok = false;
  QCOMPARE(readAllBytes(jsonPath, &ok), original);
  QVERIFY(ok);
  store.close();
}

void TestCatalogStore::corruptJsonWithoutBackupDoesNotCreateSqlite()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QByteArray original("{ not json");
  QString err;
  QVERIFY2(writeMetadataFile(dir.path(), QStringLiteral("catalog.json"), original, &err),
           qPrintable(err));

  const QString jsonPath = CatalogStore::jsonPathFor(dir.path());
  CatalogStore store;
  CatalogStore::Tables tables;
  const bool opened = store.openProject(dir.path(), false, &tables, &err);
  QVERIFY2(!opened, qPrintable(err));
  QVERIFY2(err.contains(QStringLiteral("corrupt catalog")), qPrintable(err));
  QVERIFY(!QFile::exists(CatalogStore::sqlitePathFor(dir.path())));
  bool ok = false;
  QCOMPARE(readAllBytes(jsonPath, &ok), original);
  QVERIFY(ok);
  store.close();
}

void TestCatalogStore::secondOpenSnapshotsBakAndRecoverySkipsNewerCommits()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString sqlitePath = CatalogStore::sqlitePathFor(dir.path());
  const QString bakPath = sqlitePath + QStringLiteral(".bak");
  QString err;

  {
    CatalogStore store;
    CatalogStore::Tables tables;
    QVERIFY2(store.openProject(dir.path(), false, &tables, &err), qPrintable(err));
    QCOMPARE(tables.meta.revision, 1);
    QVERIFY(QFile::exists(sqlitePath));
    QVERIFY(!QFile::exists(bakPath));
    QVERIFY(store.isWritable());
    QVERIFY(!store.recovered());
    QVERIFY2(commitEntity(&store, wellNamed(QStringLiteral("well-A"), QStringLiteral("A")), &err),
             qPrintable(err));
    store.close();
  }

  {
    CatalogStore store;
    CatalogStore::Tables tables;
    QVERIFY2(store.openProject(dir.path(), false, &tables, &err), qPrintable(err));
    QVERIFY(QFile::exists(bakPath));
    QVERIFY(hasEntityId(tables, QStringLiteral("well-A")));
    QVERIFY(!store.recovered());
    QVERIFY(store.isWritable());
    store.close();
  }

  {
    CatalogStore store;
    CatalogStore::Tables tables;
    QVERIFY2(store.openProject(dir.path(), false, &tables, &err), qPrintable(err));
    const QStringList bakBefore = filesNamed(sqlitePath, QStringLiteral("catalog.sqlite.bak*"));
    const qint64 bakBytes = QFileInfo(bakPath).size();
    QVERIFY(!bakBefore.isEmpty());
    QVERIFY2(commitEntity(&store, wellNamed(QStringLiteral("well-B"), QStringLiteral("B")), &err),
             qPrintable(err));
    store.close();
    // 快照发生在 open，不在这次 commit。
    QCOMPARE(filesNamed(sqlitePath, QStringLiteral("catalog.sqlite.bak*")), bakBefore);
    QCOMPARE(QFileInfo(bakPath).size(), bakBytes);
  }

  QFile::remove(sqlitePath + QStringLiteral("-wal"));
  QFile::remove(sqlitePath + QStringLiteral("-shm"));
  {
    QFile f(sqlitePath);
    QVERIFY2(f.open(QIODevice::WriteOnly | QIODevice::Truncate), qPrintable(f.errorString()));
    const QByteArray junk("not a database!!!!");
    QCOMPARE(f.write(junk), static_cast<qint64>(junk.size()));
    QVERIFY(f.flush());
  }

  CatalogStore::Tables recoveredTables;
  {
    CatalogStore store;
    QVERIFY2(store.openProject(dir.path(), false, &recoveredTables, &err), qPrintable(err));
    QVERIFY(store.recovered());
    QVERIFY(store.needsPrimaryRewrite());
    QVERIFY(!store.isWritable());
    QVERIFY2(store.recoveryReason().contains(QStringLiteral("catalog.sqlite")),
             qPrintable(store.recoveryReason()));
    QVERIFY(hasEntityId(recoveredTables, QStringLiteral("well-A")));
    QVERIFY(!hasEntityId(recoveredTables, QStringLiteral("well-B")));
    QVERIFY(QFile::exists(bakPath));
    const qint64 bakBytes = QFileInfo(bakPath).size();
    QVERIFY(bakBytes > 0);
    QVERIFY2(store.rewritePrimary(recoveredTables, true, &err), qPrintable(err));
    QVERIFY(QFile::exists(sqlitePath));
    QCOMPARE(filesNamed(sqlitePath, QStringLiteral("catalog.sqlite.corrupt-*")).size(), 1);
    QCOMPARE(QFileInfo(bakPath).size(), bakBytes);
    store.close();
  }

  {
    CatalogStore store;
    CatalogStore::Tables tables;
    QVERIFY2(store.openProject(dir.path(), false, &tables, &err), qPrintable(err));
    QVERIFY(store.isWritable());
    QVERIFY(!store.recovered());
    QVERIFY(hasEntityId(tables, QStringLiteral("well-A")));
    QVERIFY(!hasEntityId(tables, QStringLiteral("well-B")));
    store.close();
  }
}

void TestCatalogStore::futureUserVersionRefusesWithoutBak()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString sqlitePath = CatalogStore::sqlitePathFor(dir.path());
  const QString bakPath = sqlitePath + QStringLiteral(".bak");
  QString err;

  {
    CatalogStore store;
    CatalogStore::Tables tables;
    QVERIFY2(store.openProject(dir.path(), false, &tables, &err), qPrintable(err));
    QVERIFY2(commitEntity(&store, wellNamed(QStringLiteral("well-A"), QStringLiteral("A")), &err),
             qPrintable(err));
    store.close();
  }
  {
    CatalogStore store;
    CatalogStore::Tables tables;
    QVERIFY2(store.openProject(dir.path(), false, &tables, &err), qPrintable(err));
    QVERIFY(QFile::exists(bakPath));
    QVERIFY(hasEntityId(tables, QStringLiteral("well-A")));
    store.close();
  }

  QVERIFY2(setUserVersion(sqlitePath, 99, &err), qPrintable(err));
  const qint64 bakBytes = QFileInfo(bakPath).size();

  CatalogStore store;
  CatalogStore::Tables tables;
  const bool opened = store.openProject(dir.path(), false, &tables, &err);
  QVERIFY2(!opened, qPrintable(err));
  QVERIFY2(err.contains(QStringLiteral("newer than this build")), qPrintable(err));
  QVERIFY(!store.recovered());
  QVERIFY(!hasEntityId(tables, QStringLiteral("well-A")));
  QVERIFY(QFile::exists(bakPath));
  QVERIFY(QFile::exists(sqlitePath));
  QCOMPARE(QFileInfo(bakPath).size(), bakBytes);

  QVariant userVersion;
  QVERIFY2(sqlScalar(sqlitePath, QStringLiteral("PRAGMA user_version"), &userVersion, &err),
           qPrintable(err));
  QCOMPARE(userVersion.toInt(), 99);
  store.close();
}

void TestCatalogStore::futureSchemaEpochRefusesWithoutBak()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString sqlitePath = CatalogStore::sqlitePathFor(dir.path());
  const QString bakPath = sqlitePath + QStringLiteral(".bak");
  QString err;

  {
    CatalogStore store;
    CatalogStore::Tables tables;
    QVERIFY2(store.openProject(dir.path(), false, &tables, &err), qPrintable(err));
    QVERIFY2(commitEntity(&store, wellNamed(QStringLiteral("well-A"), QStringLiteral("A")), &err),
             qPrintable(err));
    store.close();
  }
  {
    CatalogStore store;
    CatalogStore::Tables tables;
    QVERIFY2(store.openProject(dir.path(), false, &tables, &err), qPrintable(err));
    QVERIFY(QFile::exists(bakPath));
    store.close();
  }

  QVERIFY2(forceSchemaEpoch(sqlitePath, QStringLiteral("99"), &err), qPrintable(err));
  const qint64 bakBytes = QFileInfo(bakPath).size();

  CatalogStore store;
  CatalogStore::Tables tables;
  const bool opened = store.openProject(dir.path(), false, &tables, &err);
  QVERIFY2(!opened, qPrintable(err));
  QVERIFY2(err.contains(QStringLiteral("unsupported catalog schema")), qPrintable(err));
  QVERIFY(!store.recovered());
  QVERIFY(!hasEntityId(tables, QStringLiteral("well-A")));
  QVERIFY(QFile::exists(bakPath));
  QVERIFY(QFile::exists(sqlitePath));
  QCOMPARE(QFileInfo(bakPath).size(), bakBytes);

  QVariant epoch;
  QVERIFY2(sqlScalar(sqlitePath,
                     QStringLiteral("SELECT value FROM catalog_meta WHERE key='schema_epoch'"),
                     &epoch, &err),
           qPrintable(err));
  QCOMPARE(epoch.toString(), QStringLiteral("99"));
  store.close();
}

void TestCatalogStore::unsafeVersionInsertedLaterIsSkippedButKept()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString sqlitePath = CatalogStore::sqlitePathFor(dir.path());
  QString err;

  {
    CatalogStore store;
    CatalogStore::Tables tables;
    QVERIFY2(store.openProject(dir.path(), false, &tables, &err), qPrintable(err));
    CatalogVersion v;
    v.id = QStringLiteral("ver-1");
    v.assetId = QStringLiteral("ast-1");
    v.stage = QStringLiteral("RAW");
    v.versionNumber = 1;
    v.managed = true;
    v.path = QStringLiteral("raw/ast-1/ver-1/a.las");
    v.fileName = QStringLiteral("a.las");
    QVERIFY2(store.begin(&err), qPrintable(err));
    QVERIFY2(store.upsertVersion(v, &err), qPrintable(err));
    QVERIFY2(store.commit(&err), qPrintable(err));
    store.close();
  }

  const QString insertBad = QStringLiteral(
      "INSERT INTO versions (id, asset_id, stage, version_number, managed, path, source_uri, "
      "sha256, file_name, parent_version_ids_json, extra_json) VALUES ('ver-bad', 'ast-1', 'RAW', "
      "1, 1, '../x', '', '', 'a.las', '[]', '{}')");
  QVERIFY2(execSql(sqlitePath, insertBad, &err), qPrintable(err));

  {
    CatalogStore store;
    CatalogStore::Tables tables;
    QVERIFY2(store.openProject(dir.path(), false, &tables, &err), qPrintable(err));
    QCOMPARE(tables.versions.size(), 1);
    QCOMPARE(tables.versions.at(0).id, QStringLiteral("ver-1"));
    QCOMPARE(tables.versions.at(0).path, QStringLiteral("raw/ast-1/ver-1/a.las"));
    QCOMPARE(tables.versions.at(0).fileName, QStringLiteral("a.las"));
    QVERIFY(!hasVersionId(tables, QStringLiteral("ver-bad")));
    store.close();
  }

  QVariant count;
  QVERIFY2(sqlScalar(sqlitePath, QStringLiteral("SELECT COUNT(*) FROM versions"), &count, &err),
           qPrintable(err));
  QCOMPARE(count.toInt(), 2);
}

void TestCatalogStore::rollbackLeavesSqliteUnchanged()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QString err;

  {
    CatalogStore store;
    CatalogStore::Tables tables;
    QVERIFY2(store.openProject(dir.path(), false, &tables, &err), qPrintable(err));
    QVERIFY2(commitEntity(&store, wellNamed(QStringLiteral("well-A"), QStringLiteral("A")), &err),
             qPrintable(err));
    store.close();
  }
  {
    CatalogStore store;
    CatalogStore::Tables tables;
    QVERIFY2(store.openProject(dir.path(), false, &tables, &err), qPrintable(err));
    QVERIFY(hasEntityId(tables, QStringLiteral("well-A")));
    QVERIFY2(store.begin(&err), qPrintable(err));
    QVERIFY(store.inTransaction());
    QVERIFY2(store.upsertEntity(wellNamed(QStringLiteral("well-B"), QStringLiteral("B")), &err),
             qPrintable(err));
    store.rollback();
    QVERIFY(!store.inTransaction());
    store.close();
  }
  {
    CatalogStore store;
    CatalogStore::Tables tables;
    QVERIFY2(store.openProject(dir.path(), false, &tables, &err), qPrintable(err));
    QCOMPARE(tables.entities.size(), 1);
    QVERIFY(hasEntityId(tables, QStringLiteral("well-A")));
    QVERIFY(!hasEntityId(tables, QStringLiteral("well-B")));
    store.close();
  }
}

QTEST_MAIN(TestCatalogStore)
#include "tst_catalogstore.moc"
