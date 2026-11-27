#include <QtTest>
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QSemaphore>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtConcurrent>
#include <atomic>
#include <qgsfeature.h>
#include <qgsgeometry.h>
#include <qgslayertree.h>
#include <qgsmapthemecollection.h>
#include <qgsmarkersymbol.h>
#include <qgsprojectstylesettings.h>
#include <qgsstyle.h>
#include <qgspointxy.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>
#include "../src/catalog/datacatalog.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/services/paleotaskservice.h"
#include "../src/workflow/projectlayerrefresh.h"
#include "../src/workflow/analysisresultsync.h"
#include "../src/services/stratigraphicsubintervals.h"
#include <paleo_data/service.h>
#include "../src/workflow/sectionworkbench.h"
#include "../src/io/perffixtures.h"
#include "../src/workflow/xmlpreviewsession.h"
#include "sectionalignmentfixtures.h"

namespace {
bool namedWatcherBusy(const QObject *owner, const QString &name)
{
  for (auto *watcher : owner->findChildren<QFutureWatcherBase *>(name))
    if (!watcher->isFinished()) return true;
  return false;
}
}

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
    QVERIFY(addSectionTimeDepthFixture(catalog,well.id));
    PaleoTaskService tasks;
    SectionWorkbench work(&catalog);
    work.setTaskService(&tasks);
    int ticks = 0; QTimer heartbeat; heartbeat.setInterval(2);
    connect(&heartbeat, &QTimer::timeout, this, [&] { ++ticks; }); heartbeat.start();
    bool ready = false; SectionWorkbench::PreviewData result;
    work.requestPreviewData([&](const auto &data) { result = data; ready = true; });
    QVERIFY(!ready);
    const int ticksAtRequest = ticks;
    const auto previewName = QStringLiteral("sectionWellPreviewWatcher");
    QTRY_VERIFY_WITH_TIMEOUT((namedWatcherBusy(&work, previewName) && ticks > ticksAtRequest), 30000);
    QTRY_VERIFY_WITH_TIMEOUT(ready, 30000);
    QVERIFY(ticks > 2); QCOMPARE(result.wells.size(), 1);
    QCOMPARE(result.sectionWells.size(), size_t(1));
    QVERIFY(!result.sectionWells[0].curves.empty());
    ready = false; work.requestPreviewData([&](const auto &) { ready = true; });
    QVERIFY(work.findChild<QFutureWatcherBase *>(previewName));
    work.cancelPreviewData();
    QTRY_VERIFY_WITH_TIMEOUT((!namedWatcherBusy(&work, previewName)), 30000);
    QCoreApplication::processEvents();
    QVERIFY(!ready);

    // Cancel a queued snapshot and immediately ask for the same preview.
    // The old callback must be dropped and its empty data must not be reused.
    tasks.setMaxWorkerThreads(1);
    auto gate = std::make_shared<QSemaphore>();
    const auto blocker = QtConcurrent::run(tasks.workerPool(), [gate] { gate->acquire(); });
    bool staleReady = false;
    work.requestPreviewData([&](const auto &) { staleReady = true; }, false);
    work.cancelPreviewData();
    work.requestPreviewData([&](const auto &data) { result = data; ready = true; }, false);
    gate->release();
    QTRY_VERIFY_WITH_TIMEOUT(ready, 30000);
    QVERIFY(blocker.isFinished());
    QVERIFY(!staleReady);
    QCOMPARE(result.wells.size(), 1);
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
    ready.clear(); refresh.request(&catalog, directory.path());
    const auto refreshName = QStringLiteral("projectLayerRefreshWatcher");
    QVERIFY(refresh.findChild<QFutureWatcherBase *>(refreshName));
    refresh.cancel();
    QTRY_VERIFY_WITH_TIMEOUT((!namedWatcherBusy(&refresh, refreshName)), 30000);
    QCoreApplication::processEvents();
    QCOMPARE(ready.count(), 0);
  }
  void singleCatalogMutationPublishesCurrentSequence() {
    QTemporaryDir directory;
    DataCatalog catalog; QVERIFY(catalog.open(directory.path()));
    ProjectLayerRefreshWorkflow refresh;
    QSignalSpy ready(&refresh, &ProjectLayerRefreshWorkflow::prepared);
    connect(&catalog, &DataCatalog::changed, &refresh, [&] {
      refresh.request(&catalog, directory.path());
    }, Qt::DirectConnection);
    CatalogEntity entity; entity.id = "well-1"; entity.entityType = "well"; entity.name = "A1";
    entity.hasSurface = true; entity.surfaceX = 12; entity.surfaceY = 34;
    QVERIFY(catalog.addEntity(entity));
    const quint64 sequence = catalog.mutationSeq();
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 30000);
    const auto refreshName = QStringLiteral("projectLayerRefreshWatcher");
    QTRY_VERIFY_WITH_TIMEOUT((!namedWatcherBusy(&refresh, refreshName)), 30000);
    QCoreApplication::processEvents();
    QCOMPARE(ready.count(), 1);
    QCOMPARE(ready.first().at(1).toULongLong(), sequence);
    QCOMPARE(catalog.mutationSeq(), sequence);
    const auto data = qvariant_cast<ProjectLayerData>(ready.first().at(2));
    QVERIFY(data.wellsPrepared); QVERIFY(data.wellsBytes.contains("well-1"));
    QVERIFY(data.trajectoriesPrepared);
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
  void layerRefreshCommitsStaleJournalAndRetriesAfterCatalogFailure() {
    QTemporaryDir directory;DataCatalog catalog;QString error;
    QVERIFY(catalog.open(directory.path(),&error));
    paleo::data::DataService service;std::string detail;
    QVERIFY(service.createProject((directory.path()+"/artifacts/metadata/area-model").toStdString(),"refresh",detail));
    auto draft=paleo::strat::draftScheme("refresh","refresh",{"A","B"},{});
    paleo::data::StratigraphicScheme old,next;std::vector<paleo::data::FrameworkIssue> issues;
    QVERIFY(service.commitStratigraphicScheme(draft,{},old,issues,detail));
    const auto unit=paleo::strat::makeSelection(old,paleo::strat::selectorOptions(old).first().intervalId,2);
    CatalogAsset asset;asset.id="factor";asset.type="single_factor";QVERIFY(catalog.addAsset(asset));
    CatalogVersion version;version.id="factor-v1";version.assetId=asset.id;version.stage="DERIVED";
    version.extra.insert("unitSelection",paleo::strat::toMap(unit));QVERIFY(catalog.addVersion(version));
    QCOMPARE(syncAnalysisResults(&catalog,directory.path()),QString());
    auto edited=old;edited.divisions.front().fractions={.2,.4,.75};
    QVERIFY(service.commitStratigraphicScheme(edited,old.revision.value,next,issues,detail));
    ProjectLayerRefreshWorkflow refresh;QSignalSpy ready(&refresh,&ProjectLayerRefreshWorkflow::prepared);
    const auto violations=DataCatalog::threadViolationCount();
    catalog.debugAbortJournalAfter(1);refresh.request(&catalog,directory.path());
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(),1,30000);
    QVERIFY(!catalog.versionById(version.id).extra.value("stale").toBool());
    QVERIFY(!qvariant_cast<ProjectLayerData>(ready.first().at(2)).warnings.isEmpty());
    catalog.debugAbortJournalAfter(0);ready.clear();refresh.request(&catalog,directory.path());
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(),1,30000);
    QVERIFY(catalog.versionById(version.id).extra.value("stale").toBool());
    QCOMPARE(ready.first().at(1).toULongLong(),catalog.mutationSeq());
    QCOMPARE(DataCatalog::threadViolationCount(),violations);
    DataCatalog reopened;QVERIFY(reopened.open(directory.path(),&error));
    QVERIFY(reopened.versionById(version.id).extra.value("stale").toBool());
    QCOMPARE(reopened.versionById(version.id).extra.value("unitSelection").toMap(),paleo::strat::toMap(unit));
  }
  void newSdkRevisionCannotReuseFinishedRefreshWithSameCatalogSequence() {
    QTemporaryDir directory;DataCatalog catalog;QVERIFY(catalog.open(directory.path()));
    paleo::data::DataService service;std::string error;
    QVERIFY(service.createProject((directory.path()+"/artifacts/metadata/area-model").toStdString(),"refresh",error));
    auto draft=paleo::strat::draftScheme("generation","generation",{"A","B"},{});
    paleo::data::StratigraphicScheme old,next;std::vector<paleo::data::FrameworkIssue> issues;
    QVERIFY(service.commitStratigraphicScheme(draft,{},old,issues,error));
    const auto unit=paleo::strat::makeSelection(old,paleo::strat::selectorOptions(old).first().intervalId,2);
    CatalogAsset asset;asset.id="factor";asset.type="single_factor";QVERIFY(catalog.addAsset(asset));
    CatalogVersion version;version.id="factor-v1";version.assetId=asset.id;version.stage="DERIVED";
    version.extra.insert("unitSelection",paleo::strat::toMap(unit));QVERIFY(catalog.addVersion(version));
    QCOMPARE(syncAnalysisResults(&catalog,directory.path()),QString());
    ProjectLayerRefreshWorkflow refresh;QSignalSpy ready(&refresh,&ProjectLayerRefreshWorkflow::prepared);
    refresh.request(&catalog,directory.path());
    auto *watcher=refresh.findChild<QFutureWatcherBase *>("projectLayerRefreshWatcher");QVERIFY(watcher);
    // Finish the worker without dispatching its queued owner-thread callback.
    QElapsedTimer timer;timer.start();
    while(!watcher->isFinished() && timer.elapsed()<30000)QThread::msleep(1);
    QVERIFY(watcher->isFinished());QCOMPARE(ready.count(),0);
    const auto sequence=catalog.mutationSeq();auto changed=old;changed.divisions.front().fractions={.2,.4,.75};
    QVERIFY(service.commitStratigraphicScheme(changed,old.revision.value,next,issues,error));
    QCOMPARE(catalog.mutationSeq(),sequence);refresh.request(&catalog,directory.path());
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(),1,30000);
    QVERIFY(catalog.versionById(version.id).extra.value("stale").toBool());
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
  void contextBindingPrecedesProjectOpened() {
    QTemporaryDir directory;
    QgisProjectService service;
    const auto path = directory.filePath("context.qgz");
    QStringList events;
    connect(&service, &QgisProjectService::projectContextRequested, this, [&](const QString &boundPath) {
      QCOMPARE(boundPath, service.projectPath());
      events << "context";
      if (service.isOpening())
        QTimer::singleShot(0, &service, [&] { events << "event turn"; });
    });
    connect(&service, &QgisProjectService::projectOpened, this, [&] { events << "opened"; });
    const QStringList synchronous{"context", "opened"};
    QVERIFY(service.createProject(path));
    QCOMPARE(events, synchronous);
    service.closeProject();
    events.clear();
    QVERIFY(service.openProject(path));
    QCOMPARE(events, synchronous);
    service.closeProject();
    events.clear();
    QSignalSpy finished(&service, &QgisProjectService::openFinished);
    QVERIFY(service.openProjectAsync(path));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 30000);
    QVERIFY(finished.at(0).at(0).toBool());
    QCOMPARE(events, QStringList({"context", "event turn", "opened"}));
  }
  void cancelAfterContextBindingDoesNotAnnounceReplacedProject() {
    QTemporaryDir directory, replacement;
    QgisProjectService service;
    const auto path = directory.filePath("cancel.qgz");
    QVERIFY(service.createProject(path));
    service.closeProject();
    QSignalSpy opened(&service, &QgisProjectService::projectOpened);
    QSignalSpy finished(&service, &QgisProjectService::openFinished);
    QSignalSpy closing(&service, &QgisProjectService::projectAboutToClose);
    QSignalSpy closed(&service, &QgisProjectService::projectClosed);
    connect(&service, &QgisProjectService::projectContextRequested, this, [&] {
      if (service.isOpening()) QTimer::singleShot(0, &service, &QgisProjectService::cancelOpen);
    });
    QVERIFY(service.openProjectAsync(path));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 30000);
    QVERIFY(!finished.at(0).at(0).toBool());
    QCOMPARE(closing.count(), 1);
    QCOMPARE(closed.count(), 1);
    QCOMPARE(opened.count(), 0);
    QVERIFY(service.projectPath().isEmpty());
    const auto replacementPath = replacement.filePath("replacement.qgz");
    QVERIFY(service.createProject(replacementPath));
    QTest::qWait(50);
    QCOMPARE(opened.count(), 1);
    QCOMPARE(opened.at(0).at(0).toString(), replacementPath);
    QCOMPARE(service.projectPath(), replacementPath);
  }
  // (#483 P2) 升级 tripwire：后台 QgsProject 读层 + moveToThread/takeMapLayer
  // 接管依赖「层随 moveToThread 连 provider 亲和一起搬」的 QGIS 未书面契约。
  // vendored QGIS 升级若破坏该契约，此处在 CI 当场红掉，而不是用户侧拿到
  // 跨线程层。内存层随 .qgz 内嵌持久，无需外部数据源。
  void asyncOpenAdoptsLayersOnOwnerThread_data() {
    QTest::addColumn<bool>("extraAttachment");
    QTest::newRow("layers-and-project-style") << false;
    QTest::newRow("additional-attachment") << true;
  }
  void asyncOpenAdoptsLayersOnOwnerThread() {
    QFETCH(bool, extraAttachment);
    QTemporaryDir directory;
    QgisProjectService service;
    const QString path = directory.filePath("adopt.qgz");
    QVERIFY(service.createProject(path));
    QString layerId, hiddenId, attachmentId;
    const QString styleName = QStringLiteral("well marker");
    const QString themeName = QStringLiteral("well view");
    {
      auto *vl = new QgsVectorLayer(QStringLiteral("Polygon?field=id:int"),
                                    QStringLiteral("mem"), QStringLiteral("memory"));
      vl->setCrs(QgsCoordinateReferenceSystem::fromEpsgId(4326));
      QgsFeature f(vl->fields());
      f.setAttribute(QStringLiteral("id"), 1);
      f.setGeometry(QgsGeometry::fromWkt(QStringLiteral("POLYGON ((108 36,109 36,109 37,108 36))")));
      vl->dataProvider()->addFeature(f);
      vl->updateExtents();
      service.project()->addMapLayer(vl);
      layerId = vl->id();
      auto *previous = new QgsVectorLayer("Polygon", "previous map", "memory");
      service.project()->addMapLayer(previous);
      hiddenId = previous->id();
      auto *hidden = service.project()->layerTreeRoot()->findLayer(hiddenId);
      QVERIFY(hidden);
      hidden->setItemVisibilityChecked(false);
      auto *style = service.project()->styleSettings()->projectStyle();
      QVERIFY(style && style->addSymbol(styleName, new QgsMarkerSymbol(), true));
      QgsMapThemeCollection::MapThemeRecord theme;
      theme.addLayerRecord(QgsMapThemeCollection::MapThemeLayerRecord(vl));
      service.project()->mapThemeCollection()->insert(themeName, theme);
      service.project()->setAvoidIntersectionsLayers({vl});
      QCOMPARE(service.project()->avoidIntersectionsLayers(), QList<QgsVectorLayer *>{vl});
      auto snapping = service.project()->snappingConfig();
      snapping.setIndividualLayerSettings(vl, {true, Qgis::SnappingType::Vertex, 17, Qgis::MapToolUnit::Pixels});
      service.project()->setSnappingConfig(snapping);
      if (extraAttachment) {
        QFile file(service.project()->createAttachedFile("notes.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("field notes"), 11);
        attachmentId = service.project()->attachmentIdentifier(file.fileName());
      }
      QVERIFY(service.writeProject());
      service.closeProject();
    }
    QSignalSpy opened(&service, &QgisProjectService::projectOpened);
    QSignalSpy failed(&service, &QgisProjectService::openAborted);
    QSignalSpy loadingLayers(service.project(), &QgsProject::loadingLayer);
    QVERIFY(service.openProjectAsync(path));
    QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
    QCOMPARE(failed.count(), 0);
    if (!extraAttachment) QCOMPARE(loadingLayers.count(), 0);
    QCOMPARE(service.project()->fileName(), path);
    auto *style = service.project()->styleSettings()->projectStyle();
    QVERIFY(style && style->symbolNames().contains(styleName));
    auto *adopted = qobject_cast<QgsVectorLayer *>(service.project()->mapLayer(layerId));
    QVERIFY(adopted);
    const auto verifyVisibility = [&] {
      auto *tree = service.project()->layerTreeRoot();
      auto *current = tree->findLayer(layerId);
      auto *previous = tree->findLayer(hiddenId);
      QVERIFY(current && current->layer() == service.project()->mapLayer(layerId) && current->isVisible());
      QVERIFY(previous && previous->layer() && previous->layer() == service.project()->mapLayer(hiddenId));
      QVERIFY(!previous->isVisible());
      QCOMPARE(tree->findLayers().size(), 2);
    };
    verifyVisibility();
    QCOMPARE(service.project()->mapThemeCollection()->mapThemeVisibleLayers(themeName), QList<QgsMapLayer *>{adopted});
    QCOMPARE(service.project()->avoidIntersectionsLayers(), QList<QgsVectorLayer *>{adopted});
    QCOMPARE(service.project()->snappingConfig().individualLayerSettings(adopted).tolerance(), 17.0);
    if (extraAttachment) {
      QFile file(service.project()->resolveAttachmentIdentifier(attachmentId));
      QVERIFY(file.open(QIODevice::ReadOnly));
      QCOMPARE(file.readAll(), QByteArray("field notes"));
    }
    QThread *owner = QThread::currentThread();
    const auto layers = service.project()->mapLayers();
    QVERIFY2(layers.size() >= 1, "adopted project lost its layers");
    for (auto *layer : layers) {
      QCOMPARE(layer->thread(), owner); // 层已随接管搬到主线程
      QVERIFY(layer->dataProvider() != nullptr);
      QCOMPARE(layer->dataProvider()->thread(), owner); // provider 亲和同搬
      QVERIFY(layer->isValid());
    }
    QVERIFY(service.writeProject());
    service.closeProject();
    QVERIFY(service.openProject(path));
    verifyVisibility();
    auto *savedStyle = service.project()->styleSettings()->projectStyle();
    QVERIFY(savedStyle && savedStyle->symbolNames().contains(styleName));
    QCOMPARE(service.project()->mapThemeCollection()->mapThemeVisibleLayers(themeName),
             QList<QgsMapLayer *>{service.project()->mapLayer(layerId)});
    if (extraAttachment) {
      QFile file(service.project()->resolveAttachmentIdentifier(attachmentId));
      QVERIFY(file.open(QIODevice::ReadOnly));
      QCOMPARE(file.readAll(), QByteArray("field notes"));
    }
    service.closeProject();
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
