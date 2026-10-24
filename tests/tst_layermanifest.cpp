#include <QtTest>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <QSqlDatabase>
#include <thread>
#include <atomic>

#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"

class TestLayerManifest : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase()
  {
    QVERIFY(QCoreApplication::instance() != nullptr);
  }

  void upsertMainThreadSuccess()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("project.sqlite"));

    LayerManifest manifest(dbPath);
    QString err;
    QVERIFY2(manifest.open(&err), qPrintable(err));

    LayerDeclaration decl;
    decl.layerId = QStringLiteral("horizon.T1");
    decl.horizon = QStringLiteral("T1");
    decl.type = QStringLiteral("raster");
    decl.source = QStringLiteral("/path/to/t1.tif");
    decl.group = QStringLiteral("00_Data");

    QVERIFY2(manifest.upsert(decl, &err), qPrintable(err));
    const auto all = manifest.all();
    QCOMPARE(all.size(), 1);
    QCOMPARE(all.first().layerId, QStringLiteral("horizon.T1"));
  }

  void upsertWorkerThreadDirectWithoutQueueRejected()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("project.sqlite"));

    LayerManifest manifest(dbPath);
    QString err;
    QVERIFY2(manifest.open(&err), qPrintable(err));

    LayerDeclaration decl;
    decl.layerId = QStringLiteral("horizon.T2");
    decl.horizon = QStringLiteral("T2");
    decl.type = QStringLiteral("raster");

    std::atomic_bool threadFinished{false};
    bool ok = true;
    QString workerErr;

    std::thread worker([&]() {
      ok = manifest.upsert(decl, &workerErr);
      threadFinished.store(true);
    });
    worker.join();

    QVERIFY(threadFinished.load());
    // Direct write from worker thread must be rejected to protect SQLite thread affinity
    QVERIFY(!ok);
    QVERIFY2(workerErr.contains(QStringLiteral("LayerManifest 写入必须通过主线程或 PaleoProjectStore::enqueueWrite 调度")),
             qPrintable(workerErr));
  }

  void upsertWorkerThreadViaEnqueueWriteAllowed()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("project.sqlite"));

    PaleoProjectStore store;
    store.setProjectPaths(dir.filePath(QStringLiteral("proj.qgz")),
                          dir.filePath(QStringLiteral("proj.gpkg")),
                          dbPath);

    LayerManifest manifest(dbPath);
    QString err;
    QVERIFY2(manifest.open(&err), qPrintable(err));

    LayerDeclaration decl;
    decl.layerId = QStringLiteral("horizon.T3");
    decl.horizon = QStringLiteral("T3");
    decl.type = QStringLiteral("raster");

    std::atomic_bool threadFinished{false};
    PaleoProjectStore::WriteResult res;

    std::thread worker([&]() {
      res = store.enqueueWrite([&]() -> PaleoProjectStore::WriteResult {
        QString uerr;
        if (!manifest.upsert(decl, &uerr))
          return {false, uerr};
        return {true, {}};
      });
      threadFinished.store(true);
    });
    worker.join();

    QVERIFY(threadFinished.load());
    QVERIFY2(res.ok, qPrintable(res.error));

    // Confirm persisted on main thread
    const auto all = manifest.all();
    bool found = false;
    for (const auto &d : all)
      if (d.layerId == QStringLiteral("horizon.T3"))
        found = true;
    QVERIFY(found);
  }

  void upsertWorkerThreadWithEnforceDisabledAllowed()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("project.sqlite"));

    LayerManifest manifest(dbPath);
    QString err;
    QVERIFY2(manifest.open(&err), qPrintable(err));
    manifest.setWriteQueueEnforced(false);
    QVERIFY(!manifest.isWriteQueueEnforced());

    LayerDeclaration decl;
    decl.layerId = QStringLiteral("horizon.T4");
    decl.horizon = QStringLiteral("T4");
    decl.type = QStringLiteral("raster");

    bool ok = false;
    QString workerErr;
    std::thread worker([&]() {
      ok = manifest.upsert(decl, &workerErr);
    });
    worker.join();

    QVERIFY2(ok, qPrintable(workerErr));
  }

  void removeThreadAffinityEnforcement()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("project.sqlite"));

    PaleoProjectStore store;
    store.setProjectPaths(dir.filePath(QStringLiteral("proj.qgz")),
                          dir.filePath(QStringLiteral("proj.gpkg")),
                          dbPath);

    LayerManifest manifest(dbPath);
    QString err;
    QVERIFY2(manifest.open(&err), qPrintable(err));

    LayerDeclaration decl;
    decl.layerId = QStringLiteral("horizon.T5");
    decl.horizon = QStringLiteral("T5");
    decl.type = QStringLiteral("raster");
    QVERIFY(manifest.upsert(decl, &err));

    // Worker thread direct remove -> rejected
    bool okRemove = true;
    QString removeErr;
    std::thread worker1([&]() {
      okRemove = manifest.remove(QStringLiteral("horizon.T5"), &removeErr);
    });
    worker1.join();
    QVERIFY(!okRemove);
    QVERIFY(removeErr.contains(QStringLiteral("LayerManifest 写入必须通过主线程或 PaleoProjectStore::enqueueWrite 调度")));

    // Worker thread remove via enqueueWrite -> allowed
    PaleoProjectStore::WriteResult res;
    std::thread worker2([&]() {
      res = store.enqueueWrite([&]() -> PaleoProjectStore::WriteResult {
        QString rerr;
        if (!manifest.remove(QStringLiteral("horizon.T5"), &rerr))
          return {false, rerr};
        return {true, {}};
      });
    });
    worker2.join();
    QVERIFY2(res.ok, qPrintable(res.error));
  }

  // #409：removeMany/find 契约（#346 新公开面此前零覆盖）。
  void removeManyAndFind()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("project.sqlite"));

    LayerManifest manifest(dbPath);
    QString err;
    QVERIFY2(manifest.open(&err), qPrintable(err));

    const auto decl = [](const QString &id) {
      LayerDeclaration d;
      d.layerId = id;
      d.horizon = QStringLiteral("T1");
      d.type = QStringLiteral("raster");
      return d;
    };
    QVERIFY(manifest.upsert(decl(QStringLiteral("L1")), &err));
    QVERIFY(manifest.upsert(decl(QStringLiteral("L2")), &err));
    QVERIFY(manifest.upsert(decl(QStringLiteral("L3")), &err));

    // find：命中返回声明，未命中也 true（空声明），不吞错。
    LayerDeclaration out;
    QVERIFY2(manifest.find(QStringLiteral("L2"), &out, &err), qPrintable(err));
    QCOMPARE(out.layerId, QStringLiteral("L2"));
    QVERIFY(manifest.find(QStringLiteral("不存在"), &out, &err));
    QVERIFY(out.layerId.isEmpty());
    QVERIFY(!manifest.find(QStringLiteral("L1"), nullptr, &err));

    // removeMany：一次删多行，剩下的仍在。
    QVERIFY2(manifest.removeMany({QStringLiteral("L1"), QStringLiteral("L3")}, &err),
             qPrintable(err));
    const auto all = manifest.all();
    QCOMPARE(all.size(), 1);
    QCOMPARE(all.first().layerId, QStringLiteral("L2"));
    // 删不存在的 id：事务内循环照常提交（幂等），清单不变。
    QVERIFY(manifest.removeMany({QStringLiteral("不存在")}, &err));
    QCOMPARE(manifest.all().size(), 1);
  }

  // review M4：连接名不得以 QThread* 地址为键——短命线程退出后连接须移除
  // （不泄漏），后续线程（可能复用同一地址）读清单须成功。
  void shortLivedThreadsDoNotLeakOrReuseConnections()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("project.sqlite"));
    {
      LayerManifest manifest(dbPath);
      QString err;
      QVERIFY2(manifest.open(&err), qPrintable(err));
      LayerDeclaration decl;
      decl.layerId = QStringLiteral("L1");
      decl.type = QStringLiteral("raster");
      QVERIFY2(manifest.upsert(decl, &err), qPrintable(err));
    }
    auto manifestConnections = [] {
      int n = 0;
      for (const QString &c : QSqlDatabase::connectionNames())
        n += c.startsWith(QStringLiteral("paleo_layermanifest_")) ? 1 : 0;
      return n;
    };
    const int before = manifestConnections();
    for (int round = 0; round < 4; ++round)
    {
      int rows = -1;
      std::thread worker([&] { rows = LayerManifest(dbPath).all().size(); });
      worker.join();
      QCOMPARE(rows, 1);
    }
    QCOMPARE(manifestConnections(), before);
  }
};

QTEST_MAIN(TestLayerManifest)
#include "tst_layermanifest.moc"
