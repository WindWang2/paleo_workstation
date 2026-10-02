#include <QtTest>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>

#include "../src/catalog/catalogstore.h"
#include "../src/catalog/datacatalog.h"

#include <memory>

// DataCatalog 的 sqlite 崩溃面：注入中止发生在 endBatch 之前，所以
// catalog.sqlite / -wal / -shm 字节与存在性都不变。不 checkpoint，不调
// MetaStore::closeConnectionsFor。

namespace
{

struct FamilySnap
{
  struct File
  {
    bool present = false;
    QByteArray bytes;
    QDateTime written;
  };
  File db;
  File wal;
  File shm;
};

QString captureOne(const QString &path, FamilySnap::File *slot)
{
  slot->present = false;
  slot->bytes.clear();
  slot->written = {};
  const QFileInfo info(path);
  if (!info.exists())
    return {};
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return QStringLiteral("cannot read %1: %2").arg(path, f.errorString());
  slot->bytes = f.readAll();
  slot->present = true;
  slot->written = info.lastModified();
  return {};
}

QString captureFamily(const QString &sqlite, FamilySnap *out)
{
  if (const QString err = captureOne(sqlite, &out->db); !err.isEmpty())
    return err;
  if (const QString err = captureOne(sqlite + QStringLiteral("-wal"), &out->wal); !err.isEmpty())
    return err;
  return captureOne(sqlite + QStringLiteral("-shm"), &out->shm);
}

QString familyDiff(const FamilySnap &a, const FamilySnap &b, bool times)
{
  const auto cmp = [&](const char *name, const FamilySnap::File &x,
                       const FamilySnap::File &y) -> QString {
    if (x.present != y.present)
      return QStringLiteral("%1 present %2 vs %3")
          .arg(QLatin1String(name))
          .arg(x.present)
          .arg(y.present);
    if (!x.present)
      return {};
    if (x.bytes != y.bytes)
      return QStringLiteral("%1 bytes differ (%2 vs %3)")
          .arg(QLatin1String(name))
          .arg(x.bytes.size())
          .arg(y.bytes.size());
    if (times && x.written != y.written)
      return QStringLiteral("%1 mtime %2 vs %3")
          .arg(QLatin1String(name), x.written.toString(Qt::ISODateWithMs),
               y.written.toString(Qt::ISODateWithMs));
    return {};
  };
  if (const QString err = cmp("sqlite", a.db, b.db); !err.isEmpty())
    return err;
  if (const QString err = cmp("wal", a.wal, b.wal); !err.isEmpty())
    return err;
  return cmp("shm", a.shm, b.shm);
}

CatalogEntity wellEntity(const QString &id, const QString &name)
{
  CatalogEntity e;
  e.id = id;
  e.entityType = QStringLiteral("well");
  e.name = name;
  return e;
}

bool stageThree(DataCatalog &live, const QString &entityId, const QString &assetId,
                QVector<CatalogOp> *ops, QString *error)
{
  std::unique_ptr<DataCatalog> st = live.createStagingCopy(QString());
  if (!st->addEntity(wellEntity(entityId, entityId), error))
    return false;
  CatalogAsset a;
  a.id = assetId;
  a.type = QStringLiteral("well_log");
  a.format = QStringLiteral("las");
  if (!st->addAsset(a, error))
    return false;
  EntityAssetLink l;
  l.entityType = QStringLiteral("well");
  l.entityId = entityId;
  l.assetId = assetId;
  l.role = QStringLiteral("well_log");
  l.isPrimary = true;
  if (!st->addLink(l, error))
    return false;
  if (st->journal().size() != 3)
  {
    if (error)
      *error = QStringLiteral("journal size %1, expected 3").arg(st->journal().size());
    return false;
  }
  *ops = st->journal();
  return true;
}

bool witnessSees(const QString &projectDir, const QString &entityId, const QString &assetId,
                 QString *error)
{
  CatalogStore store;
  CatalogStore::Tables tables;
  if (!store.openProject(projectDir, false, &tables, error))
    return false;
  bool entity = false;
  bool asset = false;
  bool link = false;
  for (const CatalogEntity &e : tables.entities)
    if (e.id == entityId)
      entity = true;
  for (const CatalogAsset &a : tables.assets)
    if (a.id == assetId)
      asset = true;
  for (const EntityAssetLink &l : tables.links)
    if (l.entityId == entityId && l.assetId == assetId)
      link = true;
  store.close();
  if (!entity || !asset || !link)
  {
    if (error)
      *error = QStringLiteral("witness missing rows entity=%1 asset=%2 link=%3")
                   .arg(entity)
                   .arg(asset)
                   .arg(link);
    return false;
  }
  return true;
}

} // namespace

class TestDataCatalogSqlite : public QObject
{
  Q_OBJECT

private slots:
  void abortJournalAfterKLeavesSqliteBytesIdentical();
  void batchSaveLeavesSqliteUntouchedUntilFlush();
  void workerReadsDoNotTouchSqlite();
};

void TestDataCatalogSqlite::abortJournalAfterKLeavesSqliteBytesIdentical()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  DataCatalog live;
  QString err;
  QVERIFY2(live.open(tmp.path(), &err), qPrintable(err));
  QVERIFY2(live.addEntity(wellEntity(QStringLiteral("well-0"), QStringLiteral("A0")), &err),
           qPrintable(err));
  const QString sqlite = live.sqliteCatalogPath();
  QVERIFY2(QFileInfo::exists(sqlite), qPrintable(sqlite));
  const int rev0 = live.catalogRevision();

  FamilySnap snap;
  const QString snapCap = captureFamily(sqlite, &snap);
  QVERIFY2(snapCap.isEmpty(), qPrintable(snapCap));
  QVERIFY(snap.db.present);

  QVector<CatalogOp> ops;
  QVERIFY2(stageThree(live, QStringLiteral("well-1"), QStringLiteral("ast-1"), &ops, &err),
           qPrintable(err));
  QCOMPARE(ops.size(), 3);
  FamilySnap staged;
  const QString stagedCap = captureFamily(sqlite, &staged);
  QVERIFY2(stagedCap.isEmpty(), qPrintable(stagedCap));
  const QString stagedDiff = familyDiff(snap, staged, false);
  QVERIFY2(stagedDiff.isEmpty(), qPrintable(stagedDiff));

  live.debugAbortJournalAfter(1);
  QVERIFY(!live.applyJournal(ops, &err));
  QVERIFY2(err.contains(QStringLiteral("catalog 提交在第 1 个 op 后中止（测试注入）")),
           qPrintable(err));
  // 非零不自动清：同一 journal 再放仍在第 1 个 op 后中止，盘仍不动。
  QVERIFY(!live.applyJournal(ops, &err));
  QVERIFY2(err.contains(QStringLiteral("catalog 提交在第 1 个 op 后中止（测试注入）")),
           qPrintable(err));
  QVERIFY(live.hasEntity(QStringLiteral("well-0")));
  QVERIFY(!live.hasEntity(QStringLiteral("well-1")));
  QVERIFY(live.assetById(QStringLiteral("ast-1")).id.isEmpty());
  QCOMPARE(live.links().size(), 0);
  QCOMPARE(live.catalogRevision(), rev0);
  FamilySnap aborted;
  const QString abortCap = captureFamily(sqlite, &aborted);
  QVERIFY2(abortCap.isEmpty(), qPrintable(abortCap));
  const QString abortDiff = familyDiff(snap, aborted, false);
  QVERIFY2(abortDiff.isEmpty(), qPrintable(abortDiff));

  live.debugAbortJournalAfter(0);
  QVERIFY2(live.applyJournal(ops, &err), qPrintable(err));
  QCOMPARE(live.catalogRevision(), rev0 + 1);
  QVERIFY(live.hasEntity(QStringLiteral("well-1")));
  QCOMPARE(live.assetById(QStringLiteral("ast-1")).id, QStringLiteral("ast-1"));
  QCOMPARE(live.linksForAsset(QStringLiteral("ast-1")).size(), 1);
  QVERIFY2(witnessSees(tmp.path(), QStringLiteral("well-1"), QStringLiteral("ast-1"), &err),
           qPrintable(err));

  const QString afterCommitCap = captureFamily(sqlite, &snap);
  QVERIFY2(afterCommitCap.isEmpty(), qPrintable(afterCommitCap));
  const int rev1 = live.catalogRevision();

  QVERIFY2(stageThree(live, QStringLiteral("well-2"), QStringLiteral("ast-2"), &ops, &err),
           qPrintable(err));
  QCOMPARE(ops.size(), 3);
  FamilySnap beforeK2;
  const QString beforeK2Cap = captureFamily(sqlite, &beforeK2);
  QVERIFY2(beforeK2Cap.isEmpty(), qPrintable(beforeK2Cap));
  const QString beforeK2Diff = familyDiff(snap, beforeK2, false);
  QVERIFY2(beforeK2Diff.isEmpty(), qPrintable(beforeK2Diff));

  live.debugAbortJournalAfter(2);
  QVERIFY(!live.applyJournal(ops, &err));
  QVERIFY2(err.contains(QStringLiteral("catalog 提交在第 2 个 op 后中止（测试注入）")),
           qPrintable(err));
  QVERIFY(live.hasEntity(QStringLiteral("well-1")));
  QVERIFY(!live.hasEntity(QStringLiteral("well-2")));
  QVERIFY(live.assetById(QStringLiteral("ast-2")).id.isEmpty());
  QCOMPARE(live.catalogRevision(), rev1);
  FamilySnap abortedK2;
  const QString abortK2Cap = captureFamily(sqlite, &abortedK2);
  QVERIFY2(abortK2Cap.isEmpty(), qPrintable(abortK2Cap));
  const QString abortK2Diff = familyDiff(snap, abortedK2, false);
  QVERIFY2(abortK2Diff.isEmpty(), qPrintable(abortK2Diff));

  live.debugAbortJournalAfter(0);
  QVERIFY2(live.applyJournal(ops, &err), qPrintable(err));
  QCOMPARE(live.catalogRevision(), rev1 + 1);
  QVERIFY(live.hasEntity(QStringLiteral("well-2")));
  QCOMPARE(live.assetById(QStringLiteral("ast-2")).id, QStringLiteral("ast-2"));

  const QString afterK2Cap = captureFamily(sqlite, &snap);
  QVERIFY2(afterK2Cap.isEmpty(), qPrintable(afterK2Cap));
  const int rev2 = live.catalogRevision();

  QVERIFY2(stageThree(live, QStringLiteral("well-3"), QStringLiteral("ast-3"), &ops, &err),
           qPrintable(err));
  QCOMPARE(ops.size(), 3);
  live.debugAbortJournalAfter(ops.size());
  QVERIFY(!live.applyJournal(ops, &err));
  QVERIFY2(err.contains(QStringLiteral("catalog 提交在第 %1 个 op 后中止（测试注入）").arg(ops.size())),
           qPrintable(err));
  QVERIFY(live.hasEntity(QStringLiteral("well-2")));
  QVERIFY(!live.hasEntity(QStringLiteral("well-3")));
  QVERIFY(live.assetById(QStringLiteral("ast-3")).id.isEmpty());
  QCOMPARE(live.catalogRevision(), rev2);
  FamilySnap abortedLast;
  const QString abortLastCap = captureFamily(sqlite, &abortedLast);
  QVERIFY2(abortLastCap.isEmpty(), qPrintable(abortLastCap));
  const QString abortLastDiff = familyDiff(snap, abortedLast, false);
  QVERIFY2(abortLastDiff.isEmpty(), qPrintable(abortLastDiff));

  live.debugAbortJournalAfter(0);
  FamilySnap beforeEmpty;
  const QString beforeEmptyCap = captureFamily(sqlite, &beforeEmpty);
  QVERIFY2(beforeEmptyCap.isEmpty(), qPrintable(beforeEmptyCap));
  const int revEmpty = live.catalogRevision();
  QVERIFY(live.applyJournal({}, &err));
  QCOMPARE(live.catalogRevision(), revEmpty);
  FamilySnap afterEmpty;
  const QString afterEmptyCap = captureFamily(sqlite, &afterEmpty);
  QVERIFY2(afterEmptyCap.isEmpty(), qPrintable(afterEmptyCap));
  const QString emptyDiff = familyDiff(beforeEmpty, afterEmpty, false);
  QVERIFY2(emptyDiff.isEmpty(), qPrintable(emptyDiff));
}

void TestDataCatalogSqlite::batchSaveLeavesSqliteUntouchedUntilFlush()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  DataCatalog cat;
  QString err;
  QVERIFY2(cat.open(tmp.path(), &err), qPrintable(err));
  QSignalSpy spy(&cat, &DataCatalog::changed);
  QCOMPARE(spy.count(), 0);

  const QString sqlite = cat.sqliteCatalogPath();
  FamilySnap before;
  const QString beforeCap = captureFamily(sqlite, &before);
  QVERIFY2(beforeCap.isEmpty(), qPrintable(beforeCap));
  QVERIFY(before.db.present);

  {
    DataCatalog::BatchSave batch(&cat);
    QVERIFY2(cat.addEntity(wellEntity(QStringLiteral("well-b1"), QStringLiteral("B1")), &err),
             qPrintable(err));
    QVERIFY2(cat.addEntity(wellEntity(QStringLiteral("well-b2"), QStringLiteral("B2")), &err),
             qPrintable(err));
    QCOMPARE(spy.count(), 0);
    FamilySnap during;
    const QString duringCap = captureFamily(sqlite, &during);
    QVERIFY2(duringCap.isEmpty(), qPrintable(duringCap));
    const QString duringDiff = familyDiff(before, during, true);
    QVERIFY2(duringDiff.isEmpty(), qPrintable(duringDiff));
    QVERIFY(cat.hasEntity(QStringLiteral("well-b1")));
    QVERIFY(cat.hasEntity(QStringLiteral("well-b2")));
    QVERIFY2(batch.flush(&err), qPrintable(err));
    QCOMPARE(spy.count(), 1);
  }
  QCOMPARE(spy.count(), 1);

  DataCatalog again;
  QVERIFY2(again.open(tmp.path(), &err), qPrintable(err));
  QVERIFY(again.hasEntity(QStringLiteral("well-b1")));
  QVERIFY(again.hasEntity(QStringLiteral("well-b2")));
  QCOMPARE(again.entities().size(), 2);
}

void TestDataCatalogSqlite::workerReadsDoNotTouchSqlite()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  DataCatalog cat;
  QString err;
  QVERIFY2(cat.open(tmp.path(), &err), qPrintable(err));
  QVERIFY2(cat.addEntity(wellEntity(QStringLiteral("well-w"), QStringLiteral("W")), &err),
           qPrintable(err));

  const QString sqlite = cat.sqliteCatalogPath();
  FamilySnap before;
  const QString beforeCap = captureFamily(sqlite, &before);
  QVERIFY2(beforeCap.isEmpty(), qPrintable(beforeCap));
  QVERIFY(before.db.present);

  int rows = -1;
  QString seen;
  const QString id = QStringLiteral("well-w");
  QThread *worker = QThread::create([&] {
    rows = cat.entities().size();
    seen = cat.entityById(id).id;
  });
  worker->start();
  const bool joined = worker->wait(30000);
  delete worker;
  QVERIFY(joined);
  QCOMPARE(rows, 1);
  QCOMPARE(seen, id);

  FamilySnap after;
  const QString afterCap = captureFamily(sqlite, &after);
  QVERIFY2(afterCap.isEmpty(), qPrintable(afterCap));
  const QString diff = familyDiff(before, after, false);
  QVERIFY2(diff.isEmpty(), qPrintable(diff));
}

QTEST_MAIN(TestDataCatalogSqlite)
#include "tst_datacatalog_sqlite.moc"
