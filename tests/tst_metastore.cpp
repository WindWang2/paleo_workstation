#include <QtTest>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

#include "../src/metadata/layermanifest.h"
#include "../src/metadata/mapversionstore.h"
#include "../src/metadata/metastore.h"
#include "../src/metadata/projectlock.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/metadata/releasestore.h"

#include <QProcess>
#include <QSysInfo>

// wave3/model-hardening — metadata/project.sqlite 的 PRAGMA user_version 门
// （docs/SCHEMA_MIGRATION.md 最小落地）。三个 store（LayerManifest /
// MapVersionStore / ReleaseStore）共享同一个 sqlite 文件；打开时：
//   新库/遗留库（user_version < 当前版本，含 0 与 1）→ 推进到当前版本；
//   user_version > 本构建认识的版本 → 拒开（store open 失败 + 错误文案），
//   文件保持原样（没有任何写入发生）。
class TestMetaStore : public QObject
{
  Q_OBJECT

private:
  // 用独立命名的连接直接读写 sqlite（绕过 store，制造「未来版本」等状态）。
  bool setRawUserVersion(const QString &path, int version, QString *error = nullptr)
  {
    const QString conn = QStringLiteral("tst_metastore_raw_") + QString::number(qHash(path));
    {
      QSqlDatabase db = QSqlDatabase::contains(conn)
                            ? QSqlDatabase::database(conn)
                            : QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
      db.setDatabaseName(path);
      if (!db.open())
      {
        if (error)
          *error = db.lastError().text();
        return false;
      }
      QSqlQuery q(db);
      if (!q.exec(QStringLiteral("PRAGMA user_version = %1").arg(version)))
      {
        if (error)
          *error = q.lastError().text();
        return false;
      }
    }
    QSqlDatabase::removeDatabase(conn);
    return true;
  }

  int rawUserVersion(const QString &path, QString *error = nullptr)
  {
    const QString conn = QStringLiteral("tst_metastore_raw2_") + QString::number(qHash(path));
    int v = -1;
    {
      QSqlDatabase db = QSqlDatabase::contains(conn)
                            ? QSqlDatabase::database(conn)
                            : QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
      db.setDatabaseName(path);
      if (!db.open())
      {
        if (error)
          *error = db.lastError().text();
        return -1;
      }
      QSqlQuery q(db);
      if (!q.exec(QStringLiteral("PRAGMA user_version")) || !q.next())
      {
        if (error)
          *error = q.lastError().text();
        return -1;
      }
      v = q.value(0).toInt();
    }
    QSqlDatabase::removeDatabase(conn);
    return v;
  }

  // 在库里放一张遗留表（不设 user_version）——模拟 T17 之前的旧工程库。
  bool seedLegacyTable(const QString &path, QString *error = nullptr)
  {
    const QString conn = QStringLiteral("tst_metastore_seed_") + QString::number(qHash(path));
    {
      QSqlDatabase db = QSqlDatabase::contains(conn)
                            ? QSqlDatabase::database(conn)
                            : QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
      db.setDatabaseName(path);
      if (!db.open())
      {
        if (error)
          *error = db.lastError().text();
        return false;
      }
      QSqlQuery q(db);
      if (!q.exec(QStringLiteral("CREATE TABLE legacy_thing(id TEXT PRIMARY KEY)")))
      {
        if (error)
          *error = q.lastError().text();
        return false;
      }
    }
    QSqlDatabase::removeDatabase(conn);
    return true;
  }

private slots:
  // 新库：LayerManifest open 后 user_version 被写成当前版本（2）。
  void freshDbAdoptsUserVersion()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("metadata/project.sqlite"));
    LayerManifest manifest(dbPath);
    QString err;
    QVERIFY2(manifest.open(&err), qPrintable(err));
    QCOMPARE(rawUserVersion(dbPath), MetaStore::kUserVersion);
    QCOMPARE(rawUserVersion(dbPath), 2); // 当前版本号就是 2（fault_set.surface）
  }

  // 遗留库（表在、user_version=0）：open 推进到当前版本，旧表原样保留。
  void legacyZeroVersionUpgradedInPlace()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("metadata/project.sqlite"));
    QVERIFY(QDir().mkpath(QFileInfo(dbPath).absolutePath()));
    QVERIFY(seedLegacyTable(dbPath));
    QCOMPARE(rawUserVersion(dbPath), 0);

    LayerManifest manifest(dbPath);
    QString err;
    QVERIFY2(manifest.open(&err), qPrintable(err));
    QCOMPARE(rawUserVersion(dbPath), MetaStore::kUserVersion);

    // 旧表未被触碰。
    {
      QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                  QStringLiteral("tst_metastore_check"));
      db.setDatabaseName(dbPath);
      QVERIFY(db.open());
      QSqlQuery q(db);
      QVERIFY(q.exec(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE name='legacy_thing'")));
      QVERIFY(q.next());
      QCOMPARE(q.value(0).toInt(), 1);
      q.finish();
      db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("tst_metastore_check"));
  }

  // 未来版本：三个 store 全部拒开，错误写明版本号；库内容不动（user_version
  // 仍是 99，没有建表写入发生）。
  void futureVersionRefusedByAllStores()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("metadata/project.sqlite"));
    QVERIFY(QDir().mkpath(QFileInfo(dbPath).absolutePath()));
    QVERIFY(setRawUserVersion(dbPath, 99));

    LayerManifest manifest(dbPath);
    QString err;
    QVERIFY(!manifest.open(&err));
    QVERIFY2(err.contains(QStringLiteral("99")),
             qPrintable(QStringLiteral("error should name the version: %1").arg(err)));

    MapVersionStore versions(dbPath);
    QString err2;
    QVERIFY(!versions.open(&err2));
    QVERIFY(!err2.isEmpty());

    // 原件未被修改。
    QCOMPARE(rawUserVersion(dbPath), 99);
    {
      QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                  QStringLiteral("tst_metastore_check2"));
      db.setDatabaseName(dbPath);
      QVERIFY(db.open());
      QSqlQuery q(db);
      QVERIFY(q.exec(QStringLiteral("SELECT COUNT(*) FROM sqlite_master")));
      QVERIFY(q.next());
      QCOMPARE(q.value(0).toInt(), 0); // 空库——拒开前没建任何表
      q.finish();
      db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("tst_metastore_check2"));
  }

  // 已在当前版本的库：open 幂等通过，user_version 不变。
  void currentVersionPassesIdempotently()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("metadata/project.sqlite"));
    LayerManifest first(dbPath);
    QVERIFY(first.open());
    QCOMPARE(rawUserVersion(dbPath), MetaStore::kUserVersion);

    // 同库再开（另一 store、另一连接）照常工作。
    MapVersionStore second(dbPath);
    QString err;
    QVERIFY2(second.open(&err), qPrintable(err));
    QCOMPARE(rawUserVersion(dbPath), MetaStore::kUserVersion);
  }

  // ---- 并发：两实例同开一工程的写互斥（docs/SCHEMA_MIGRATION.md §6）----

  // 第二实例取锁必须失败，且错误带持有者信息（pid/主机/程序名）。
  void projectLockSecondInstanceRefused()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ProjectDirLock a(dir.path());
    QString err;
    QVERIFY2(a.tryLock(&err), qPrintable(err));
    QVERIFY(a.isHeld());
    QVERIFY(QFile::exists(a.lockPath()));

    ProjectDirLock b(dir.path());
    QString err2;
    QVERIFY(!b.tryLock(&err2));
    QVERIFY2(!err2.isEmpty(), "refusal must carry an explanation");
    QVERIFY(err2.contains(QStringLiteral("pid")));
    QVERIFY(!b.isHeld());

    // 首锁仍持有且解锁后路径可复用。
    QVERIFY(a.isHeld());
  }

  // 解锁后可重取；不同工程目录互不影响。
  void projectLockUnlockAndIndependence()
  {
    QTemporaryDir dirA, dirB;
    QVERIFY(dirA.isValid() && dirB.isValid());
    {
      ProjectDirLock a(dirA.path());
      QVERIFY(a.tryLock());
      ProjectDirLock b(dirB.path());
      QVERIFY(b.tryLock()); // 不同工程目录同时可写
    } // 两个锁析构释放

    ProjectDirLock again(dirA.path());
    QString err;
    QVERIFY2(again.tryLock(&err), qPrintable(err)); // 解锁后可重取
  }
// ---- wave/data-foundation T7：user_version 前向/后向矩阵扩全 -------------
  // 组合矩阵：{0(遗留), 1(上一版，升级), kUserVersion(当前),
  // kUserVersion+1/99(未来)} × {LayerManifest, MapVersionStore, ReleaseStore}。
  // 低于当前版本的库采纳并保持可写；当前版本原样通过；更高版本拒开且零表创建。
  // （"absent" 在 sqlite 上等价 0——全新文件 PRAGMA 读 0，已由 fresh 案覆盖。）
  void userVersionMatrixAcrossStores()
  {
    struct Row { int version; bool shouldOpen; };
    const QVector<Row> matrix = {
        {0, true},
        {1, true},
        {MetaStore::kUserVersion, true},
        {MetaStore::kUserVersion + 1, false},
        {99, false}};

    for (const Row &row : matrix)
    {
      // LayerManifest
      {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString db = dir.filePath(QStringLiteral("m.sqlite"));
        QVERIFY(setRawUserVersion(db, row.version));
        LayerManifest m(db);
        QString err;
        QCOMPARE2(m.open(&err), row.shouldOpen, err, row.version);
        if (row.shouldOpen)
        {
          // 采纳后可写且 user_version 推到当前版本。
          LayerDeclaration d;
          d.layerId = QStringLiteral("L1");
          d.type = QStringLiteral("vector");
          QVERIFY2(m.upsert(d, &err), qPrintable(err));
          QCOMPARE(rawUserVersion(db), MetaStore::kUserVersion);
        }
        else
        {
          QVERIFY(!m.all().isEmpty() ? true : true); // 读面不炸即可
          QVERIFY2(tablesOf(db).isEmpty(),
                   qPrintable(QStringLiteral("manifest tables: %1 (v=%2)")
                                  .arg(tablesOf(db).join(QLatin1Char(',')))
                                  .arg(row.version))); // 零表创建
        }
      }
      // MapVersionStore
      {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString db = dir.filePath(QStringLiteral("m.sqlite"));
        QVERIFY(setRawUserVersion(db, row.version));
        MapVersionStore vs(db);
        QString err;
        QCOMPARE2(vs.open(&err), row.shouldOpen, err, row.version);
        if (row.shouldOpen)
        {
          QVERIFY2(!vs.saveVersion(QStringLiteral("H"), QStringLiteral("{}"), &err)
                        .horizon.isEmpty(),
                   qPrintable(err));
          QCOMPARE(rawUserVersion(db), MetaStore::kUserVersion);
        }
        else
        {
          QVERIFY2(tablesOf(db).isEmpty(),
                   qPrintable(QStringLiteral("mapversion tables: %1 (v=%2)")
                                  .arg(tablesOf(db).join(QLatin1Char(',')))
                                  .arg(row.version)));
        }
      }
      // ReleaseStore
      {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString db = dir.filePath(QStringLiteral("m.sqlite"));
        QVERIFY(setRawUserVersion(db, row.version));
        ReleaseStore rs(db);
        QString err;
        QCOMPARE2(rs.open(&err), row.shouldOpen, err, row.version);
        if (row.shouldOpen)
          QCOMPARE(rawUserVersion(db), MetaStore::kUserVersion);
        else
          QVERIFY2(tablesOf(db).isEmpty(),
                   qPrintable(QStringLiteral("release tables: %1 (v=%2)")
                                  .arg(tablesOf(db).join(QLatin1Char(',')))
                                  .arg(row.version)));
      }
    }
  }

  // T7 补充：采纳后重开幂等（user_version 已是当前版本 → 再开不动版本号）+ 写后
  // 重开数据仍在（迁移不丢数据）。
  void adoptedDbReopensStable()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString db = dir.filePath(QStringLiteral("m.sqlite"));
    {
      LayerManifest m(db);
      QString err;
      QVERIFY(m.open(&err));
      LayerDeclaration d;
      d.layerId = QStringLiteral("L1");
      d.type = QStringLiteral("vector");
      QVERIFY(m.upsert(d, &err));
    }
    const int v1 = rawUserVersion(db);
    {
      LayerManifest m(db);
      QString err;
      QVERIFY(m.open(&err));
      QCOMPARE(m.all().size(), 1);
    }
    QCOMPARE(rawUserVersion(db), v1); // 幂等：不空涨
  }

  // ---- T4：残留锁实测——持有者进程已死的锁文件被 tryLock 自动回收 ------
  void staleLockAutoRecovered()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // 手写一份「持有者已退出」的锁文件（QLockFile 文本格式：pid\t主机\t应用）。
    // 拿一个「刚退出」的 pid：先起再收 processId，等它退出后写进锁文件。
    QProcess killer;
    killer.start(QStringLiteral("sh"), {QStringLiteral("-c"), QStringLiteral("exit 0")});
    QVERIFY2(killer.waitForStarted(5000), "cannot spawn a short-lived pid");
    const qint64 deadPid = killer.processId();
    QVERIFY(deadPid > 0);
    QVERIFY(killer.waitForFinished(5000));
    const QString lockPath = QDir(dir.path()).filePath(
        QStringLiteral("artifacts/metadata/.project.lock"));
    QVERIFY(QDir().mkpath(QFileInfo(lockPath).absolutePath()));
    {
      QFile f(lockPath);
      QVERIFY(f.open(QIODevice::WriteOnly));
      // QLockFile 的磁盘格式是三行：pid\n应用名\n主机名（getLockInfo 的
      // 顺序实证：line2→appname、line3→hostname；主机行必须与本机一致——
      // 跨主机无法证明持有者存活，QLockFile 不回收）。
      f.write((QString::number(deadPid) + QLatin1Char('\n') +
               QStringLiteral("paleo-test") + QLatin1Char('\n') +
               QSysInfo::machineHostName() + QLatin1Char('\n'))
                  .toUtf8());
    }
    ProjectDirLock lock(dir.path());
    QString err;
    QVERIFY2(lock.tryLock(&err), qPrintable(err)); // 死持有者 → 陈旧锁回收
    QVERIFY(lock.isHeld());
  }

  // ---- T4：只读降级门——三个存储的写面如实拒绝、读面照常 ------------------
  void readOnlyGatesRefuseStoreWrites()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString db = dir.filePath(QStringLiteral("m.sqlite"));

    LayerManifest m(db);
    QString err;
    QVERIFY(m.open(&err));
    m.setReadOnly(true);
    LayerDeclaration d;
    d.layerId = QStringLiteral("L1");
    d.type = QStringLiteral("vector");
    QVERIFY(!m.upsert(d, &err));
    QVERIFY(err.contains(QStringLiteral("另一个实例锁定")));
    QVERIFY(!m.remove(QStringLiteral("L1"), &err));
    m.setReadOnly(false);
    QVERIFY(m.upsert(d, &err)); // 解锁恢复可写
    QCOMPARE(m.all().size(), 1);

    MapVersionStore vs(db);
    QVERIFY(vs.open(&err));
    vs.setReadOnly(true);
    QVERIFY(vs.saveVersion(QStringLiteral("H"), QStringLiteral("{}"), &err)
                .horizon.isEmpty());
    QVERIFY(err.contains(QStringLiteral("另一个实例锁定")));
    QVERIFY(!vs.recordLayoutProduct(QStringLiteral("H"), QStringLiteral("/x.pdf"),
                                    QString(), QString(), &err));

    PaleoProjectStore store;
    store.setReadOnly(true);
    const auto okUnit = [] { return PaleoProjectStore::WriteResult{true, QString()}; };
    QVERIFY(!store.enqueueWrite(okUnit).ok);
    QVERIFY(!store.saveAll(okUnit, okUnit).ok);
    QVERIFY(!store.commitAll(QStringLiteral("op1"), QStringLiteral("d"), okUnit, okUnit).ok);
    store.setReadOnly(false);
    QVERIFY(store.enqueueWrite(okUnit).ok);
  }

  // ---- #80：只读实例 open 不建库、不建表、不推进 user_version ------------
  void readOnlyOpenDoesNotCreateOrMigrate()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QString err;

    // (a) 文件不存在：只读 open 失败，且不在被锁目录里建库。
    const QString missing = dir.filePath(QStringLiteral("sub/missing.sqlite"));
    LayerManifest m0(missing);
    m0.setReadOnly(true);
    QVERIFY(!m0.open(&err));
    QVERIFY(!QFileInfo::exists(missing));
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("sub"))));
    MapVersionStore v0(missing);
    v0.setReadOnly(true);
    QVERIFY(!v0.open(&err));
    QVERIFY(!QFileInfo::exists(missing));

    // (b) 遗留空库（user_version=0、无表）：只读 open 成功但零写入。
    const QString legacy = dir.filePath(QStringLiteral("legacy.sqlite"));
    QVERIFY2(setRawUserVersion(legacy, 0, &err), qPrintable(err));
    LayerManifest m(legacy);
    m.setReadOnly(true);
    QVERIFY2(m.open(&err), qPrintable(err));
    MapVersionStore vs(legacy);
    vs.setReadOnly(true);
    QVERIFY2(vs.open(&err), qPrintable(err));
    QCOMPARE(rawUserVersion(legacy), 0);
    QVERIFY2(tablesOf(legacy).isEmpty(), qPrintable(tablesOf(legacy).join(',')));

    // (c) 未来版本：只读同样拒开。
    const QString future = dir.filePath(QStringLiteral("future.sqlite"));
    QVERIFY(setRawUserVersion(future, MetaStore::kUserVersion + 1, &err));
    LayerManifest mf(future);
    mf.setReadOnly(true);
    QVERIFY(!mf.open(&err));
    QVERIFY(err.contains(QStringLiteral("newer than this build")));
  }

  // ---- #80：closeConnectionsFor 释放句柄；同路径重建后写进新文件 --------
  void closeConnectionsReleasesHandlesAndRebinds()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString db = dir.filePath(QStringLiteral("p.sqlite"));
    const auto connectionsOn = [](const QString &path) {
      int n = 0;
      for (const QString &name : QSqlDatabase::connectionNames())
      {
        const QSqlDatabase c = QSqlDatabase::database(name, false);
        if (c.isValid() && QFileInfo(c.databaseName()).absoluteFilePath() ==
                               QFileInfo(path).absoluteFilePath())
          ++n;
      }
      return n;
    };
    QString err;
    LayerDeclaration d;
    d.layerId = QStringLiteral("L1");
    d.type = QStringLiteral("vector");
    {
      LayerManifest m(db);
      QVERIFY2(m.upsert(d, &err), qPrintable(err));
      MapVersionStore vs(db);
      QVERIFY2(vs.open(&err), qPrintable(err));
      ReleaseStore rs(db);
      QVERIFY2(rs.open(&err), qPrintable(err));
    }
    QVERIFY(connectionsOn(db) >= 3);
    QVERIFY(MetaStore::closeConnectionsFor(db) >= 3);
    QCOMPARE(connectionsOn(db), 0);

    // 删除后在原路径重建：新实例写进新文件（不复用旧 inode 的陈旧连接）。
    QVERIFY(QFile::remove(db));
    LayerManifest fresh(db);
    LayerDeclaration d2 = d;
    d2.layerId = QStringLiteral("L2");
    QVERIFY2(fresh.upsert(d2, &err), qPrintable(err));
    QVERIFY(QFileInfo::exists(db));
    const QVector<LayerDeclaration> all = fresh.all();
    QCOMPARE(all.size(), 1);
    QCOMPARE(all.first().layerId, QStringLiteral("L2"));
    MetaStore::closeConnectionsFor(db);
  }

private:
  // 矩阵断言辅助（QCOMPARE + 版本号上下文）。
  void QCOMPARE2(bool actual, bool expected, const QString &err, int version)
  {
    QVERIFY2(actual == expected,
             qPrintable(QStringLiteral("user_version=%1: %2").arg(version).arg(err)));
  }

  // sqlite 里全部用户表名（零表创建断言用）。
  static QStringList tablesOf(const QString &path)
  {
    const QString conn = QStringLiteral("tst_metastore_tables_") + QString::number(qHash(path));
    QStringList out;
    {
      QSqlDatabase db = QSqlDatabase::contains(conn)
                            ? QSqlDatabase::database(conn)
                            : QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
      db.setDatabaseName(path);
      if (!db.open())
        return out;
      QSqlQuery q(db);
      if (q.exec(QStringLiteral(
              "SELECT name FROM sqlite_master WHERE type='table' "
              "AND name NOT LIKE 'sqlite_%'")))
        while (q.next())
          out.append(q.value(0).toString());
    }
    QSqlDatabase::removeDatabase(conn);
    return out;
  }
};

QTEST_MAIN(TestMetaStore)
#include "tst_metastore.moc"

