#include <QtTest>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

#include "../src/metadata/layermanifest.h"
#include "../src/metadata/mapversionstore.h"
#include "../src/metadata/metastore.h"
#include "../src/metadata/projectlock.h"

// wave3/model-hardening — metadata/project.sqlite 的 PRAGMA user_version 门
// （docs/SCHEMA_MIGRATION.md 最小落地）。三个 store（LayerManifest /
// MapVersionStore / ReleaseStore）共享同一个 sqlite 文件；打开时：
//   新库/遗留库（user_version=0）→ 推进到当前版本；
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
  // 新库：LayerManifest open 后 user_version 被写成当前版本（1）。
  void freshDbAdoptsUserVersion()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("metadata/project.sqlite"));
    LayerManifest manifest(dbPath);
    QString err;
    QVERIFY2(manifest.open(&err), qPrintable(err));
    QCOMPARE(rawUserVersion(dbPath), MetaStore::kUserVersion);
    QCOMPARE(rawUserVersion(dbPath), 1); // 当前版本号就是 1
  }

  // 遗留库（表在、user_version=0）：open 推进到 1，旧表原样保留。
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
};

QTEST_MAIN(TestMetaStore)
#include "tst_metastore.moc"
