#include <QtTest>
#include <QFutureWatcher>
#include <QSemaphore>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtConcurrent>
#include <atomic>
#include "../src/catalog/datacatalog.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/workflow/projectlayerrefresh.h"
#include "../src/workflow/sectionworkbench.h"
#include "../src/io/perffixtures.h"
#include "../src/workflow/xmlpreviewsession.h"

class TestBackgroundOpen : public QObject {
  Q_OBJECT
private slots:
  void sectionPreviewReadsLasInPoolAndCancels() {
    QTemporaryDir directory;
    const auto path = directory.filePath("big.las");
    QVERIFY(PerfFixtures::makeSyntheticLas(path, 740000));
    DataCatalog catalog; QVERIFY(catalog.open(directory.path()));
    CatalogEntity well; well.id = "w1"; well.entityType = "well"; well.name = "A1";
    well.hasSurface = true; well.coordinateStatus = "untransformed"; well.surfaceX = 12; well.surfaceY = 34;
    QVERIFY(catalog.addEntity(well));
    CatalogAsset asset; asset.id = "a1"; asset.type = "well_log"; asset.format = "las"; asset.displayName = "big.las";
    QVERIFY(catalog.addAsset(asset));
    CatalogVersion version; version.id = catalog.nextVersionId(); version.assetId = asset.id;
    version.stage = "RAW"; version.managed = false; version.path = path; version.fileName = "big.las";
    QVERIFY(catalog.addVersion(version));
    EntityAssetLink link; link.entityType = "well"; link.entityId = well.id; link.assetId = asset.id; link.role = "well_log";
    QVERIFY(catalog.addLink(link));
    SectionWorkbench work(&catalog);
    int ticks = 0; QTimer heartbeat; heartbeat.setInterval(2);
    connect(&heartbeat, &QTimer::timeout, this, [&] { ++ticks; }); heartbeat.start();
    bool ready = false; SectionWorkbench::PreviewData result;
    QElapsedTimer elapsed; elapsed.start();
    work.requestPreviewData([&](const auto &data) { result = data; ready = true; });
    QVERIFY2(elapsed.elapsed() < 50, "Section well preparation blocked the owner thread");
    QVERIFY(!ready);
    QTRY_VERIFY_WITH_TIMEOUT(ready, 30000);
    QVERIFY(ticks > 2); QCOMPARE(result.wells.size(), 1);
    QCOMPARE(result.sectionWells.size(), size_t(1));
    QVERIFY(!result.sectionWells[0].curves.empty());
    ready = false; work.requestPreviewData([&](const auto &) { ready = true; }); work.cancelPreviewData();
    const auto idle = [&work] {
      for (auto *watcher : work.findChildren<QFutureWatcherBase *>())
        if (watcher->isRunning()) return false;
      return true;
    };
    QTRY_VERIFY_WITH_TIMEOUT(idle(), 30000);
    QTest::qWait(50); QVERIFY(!ready);
    QCOMPARE(DataCatalog::threadViolationCount(), 0);
  }
  void xmlPreviewIsDeferredAndGenericListIsBounded() {
    QTemporaryDir directory;
    const auto path = directory.filePath("generic.xml");
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("<reference>");
    for (int i = 0; i < 1500; ++i) file.write("<value key=\"row\">42</value>");
    file.write("</reference>"); file.close();
    XmlPreviewSession session; QSignalSpy ready(&session, &XmlPreviewSession::ready);
    session.open(path); QCOMPARE(ready.count(), 0);
    QTRY_COMPARE(ready.count(), 1);
    const auto data = qvariant_cast<XmlPreviewData>(ready.first().first());
    QVERIFY(!data.chartOk); QVERIFY(data.table.ok);
    QCOMPARE(data.table.sheets.first().rows.size(), 1000);
    QVERIFY(!data.table.issues.isEmpty());
    const auto badPath = directory.filePath("bad.xml");
    QFile bad(badPath); QVERIFY(bad.open(QIODevice::WriteOnly)); bad.write("<root><bad>"); bad.close();
    ready.clear(); session.open(badPath); session.open(path);
    QTRY_COMPARE(ready.count(), 1); QVERIFY(qvariant_cast<XmlPreviewData>(ready.first().first()).table.ok);
    ready.clear(); session.open(path, QString(64, QLatin1Char('0')));
    QTRY_COMPARE(ready.count(), 1);
    const auto mismatch = qvariant_cast<XmlPreviewData>(ready.first().first());
    QVERIFY(!mismatch.chartOk); QVERIFY(!mismatch.table.ok);
    QVERIFY(mismatch.table.error.contains("SHA-256"));
  }
  void layerRefreshUsesLatestSnapshotAndCanCancel() {
    QTemporaryDir directory;
    DataCatalog catalog; QVERIFY(catalog.open(directory.path()));
    ProjectLayerRefreshWorkflow refresh;
    QSignalSpy ready(&refresh, &ProjectLayerRefreshWorkflow::prepared);
    refresh.request(&catalog, directory.path());
    CatalogEntity entity; entity.id = "well-1"; entity.entityType = "well"; entity.name = "A1";
    entity.hasSurface = true; entity.surfaceX = 12; entity.surfaceY = 34;
    QVERIFY(catalog.addEntity(entity));
    refresh.request(&catalog, directory.path());
    QTRY_COMPARE(ready.count(), 1);
    QCOMPARE(ready.first().at(1).toULongLong(), catalog.mutationSeq());
    const auto data = qvariant_cast<ProjectLayerData>(ready.first().at(2));
    QVERIFY(data.wellsPrepared); QVERIFY(data.wellsBytes.contains("well-1"));
    QVERIFY(!QFileInfo::exists(directory.filePath("artifacts/layers/wells.geojson")));
    ready.clear(); refresh.request(&catalog, directory.path()); refresh.cancel();
    QTest::qWait(100); QCOMPARE(ready.count(), 0);
  }
  void preparedCatalogRetainsOwnerAndCanSave() {
    QTemporaryDir directory;
    { DataCatalog source; QVERIFY(source.open(directory.path()));
      CatalogEntity entity; entity.id = "well-1"; entity.entityType = "well"; entity.name = "A1";
      QVERIFY(source.addEntity(entity)); }
    QFutureWatcher<std::shared_ptr<DataCatalog>> watcher;
    watcher.setFuture(QtConcurrent::run([path = directory.path()] { return DataCatalog::prepareOpen(path, false); }));
    QTRY_VERIFY(watcher.isFinished());
    auto prepared = watcher.result(); QVERIFY(prepared->isOpen()); QVERIFY(!prepared->thread());
    DataCatalog live; QSignalSpy changed(&live, &DataCatalog::changed);
    QString error;
    QVERIFY2(live.adoptPrepared(prepared, &error), qPrintable(error));
    QCOMPARE(live.thread(), QThread::currentThread()); QCOMPARE(changed.count(), 1);
    QCOMPARE(live.entities("well").size(), 1);
    CatalogEntity entity; entity.id = "well-2"; entity.entityType = "well"; entity.name = "A2";
    QVERIFY2(live.addEntity(entity, &error), qPrintable(error));
    DataCatalog reopened; QVERIFY(reopened.open(directory.path())); QCOMPARE(reopened.entities("well").size(), 2);
    QVERIFY(!live.adoptPrepared(prepared, &error)); // snapshots are consumed once
  }
  void cancelBetweenGuiHandoffPhasesDropsLateAssembly() {
    QTemporaryDir directory;
    QgisProjectService service;
    const auto path = directory.filePath("phased.qgz");
    QVERIFY(service.createProject(path)); service.closeProject();
    QSignalSpy opened(&service, &QgisProjectService::projectOpened);
    QSignalSpy finished(&service, &QgisProjectService::openFinished);
    connect(&service, &QgisProjectService::openProgress, this, [&service](int percent, const QString &) {
      if (percent == 85) QTimer::singleShot(0, &service, &QgisProjectService::cancelOpen);
    });
    QVERIFY(service.openProjectAsync(path));
    QTRY_COMPARE(finished.count(), 1);
    QTest::qWait(50);
    QCOMPARE(opened.count(), 0); QVERIFY(service.projectPath().isEmpty());
    QVERIFY(service.lastOpenCancelled()); QVERIFY(!service.isOpening());
    QVERIFY(!service.writeProject());
  }
  void readOnlyPreparationCreatesNoStore() {
    QTemporaryDir directory;
    auto future = QtConcurrent::run([path = directory.path()] { return DataCatalog::prepareOpen(path, true); });
    future.waitForFinished();
    DataCatalog live; live.setLockedReadOnly(true); QString error;
    QVERIFY2(live.adoptPrepared(future.result(), &error), qPrintable(error));
    QVERIFY(live.refusesWrites());
    QVERIFY(!QFileInfo::exists(live.sqliteCatalogPath()));
  }
  void changedDiskRevisionRefusesStaleSnapshot() {
    QTemporaryDir directory;
    auto future = QtConcurrent::run([path = directory.path()] { return DataCatalog::prepareOpen(path, false); });
    future.waitForFinished();
    { DataCatalog concurrent; QVERIFY(concurrent.open(directory.path()));
      CatalogEntity entity; entity.id = "new"; entity.entityType = "well"; entity.name = "new";
      QVERIFY(concurrent.addEntity(entity)); }
    DataCatalog live; QString error;
    QVERIFY(!live.adoptPrepared(future.result(), &error));
    QVERIFY(error.contains(QStringLiteral("已变化"))); QVERIFY(live.refusesWrites());
  }
  void backgroundPreparationKeepsEventLoopAliveAndCancels() {
    QTemporaryDir directory;
    QgisProjectService service;
    const QString path = directory.filePath("project.qgz"); QVERIFY(service.createProject(path));
    auto entered = std::make_shared<std::atomic_bool>(false);
    auto workerThread = std::make_shared<std::atomic_bool>(false);
    auto release = std::make_shared<QSemaphore>();
    const auto releaseOnExit = qScopeGuard([release] { release->release(); });
    QThread *owner = QThread::currentThread();
    service.setOpenPreparation([=](const QString &) {
      return [=] { workerThread->store(QThread::currentThread() != owner); entered->store(true); release->acquire(); };
    });
    QSignalSpy opened(&service, &QgisProjectService::projectOpened);
    QSignalSpy drained(&service, &QgisProjectService::openWorkerFinished);
    int ticks = 0; QTimer heartbeat; heartbeat.setInterval(5);
    connect(&heartbeat, &QTimer::timeout, this, [&] { ++ticks; }); heartbeat.start();
    QVERIFY(service.openProjectAsync(path));
    QTRY_VERIFY_WITH_TIMEOUT(entered->load(), 10000);
    QTRY_VERIFY(ticks > 5); QVERIFY(workerThread->load());
    service.cancelOpen();
    QVERIFY(service.isBackgroundOpenRunning());
    QVERIFY(!service.openProject(path));
    release->release();
    QTRY_COMPARE(drained.count(), 1);
    QCOMPARE(opened.count(), 0); QVERIFY(!service.isOpening()); QCOMPARE(service.projectPath(), path);
  }
};
int main(int argc, char **argv) {
  if (!QgisRuntime::initialize("/usr")) qFatal("QGIS init failed");
  TestBackgroundOpen test; const int result = QTest::qExec(&test, argc, argv);
  QgisRuntime::shutdown(); return result;
}
#include "tst_background_open.moc"
