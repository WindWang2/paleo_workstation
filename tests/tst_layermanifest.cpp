#include <QtTest>
#include <QTemporaryDir>
#include <QCoreApplication>
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
};

QTEST_MAIN(TestLayerManifest)
#include "tst_layermanifest.moc"
