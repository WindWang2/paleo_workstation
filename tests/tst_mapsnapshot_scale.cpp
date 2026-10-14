#include <algorithm>
#include <QtTest>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QTimeZone>

#include <qgsapplication.h>
#include <qgscoordinatereferencesystem.h>
#include <qgscoordinatetransformcontext.h>
#include <qgsfeature.h>
#include <qgsfields.h>
#include <qgsgeometry.h>
#include <qgsproject.h>
#include <qgsvectorfilewriter.h>
#include <qgsvectorlayer.h>

#include "../src/catalog/datacatalog.h"
#include "../src/domain/interpretationevidence.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/mapversionstore.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/services/paleotaskservice.h"
#include "../src/workflow/mapversioncontroller.h"

// 地图快照规模记录。时间、字节和内存只写入 BASELINE 行，不断言未测过的墙钟阈值。
class TestMapSnapshotScale : public QObject {
  Q_OBJECT
  struct Fixture {
    QTemporaryDir dir;
    QgisProjectService projectSvc;
    PaleoProjectStore store;
    LayerManifest manifest{dir.filePath("project.sqlite")};
    QgisLayerService layers{&projectSvc, &manifest};
    MapVersionStore versions{dir.filePath("project.sqlite")};
    MapVersionController controller{&versions, &layers};
    bool init() {
      if (!dir.isValid() || !projectSvc.createProject(dir.filePath("proj.qgz")) || !manifest.open()) return false;
      store.setProjectPaths(dir.filePath("proj.qgz"), dir.filePath("proj.gpkg"), dir.filePath("project.sqlite"));
      controller.setProjectStore(&store);
      QString error;
      return versions.open(&error);
    }
  };
  static QString historyJson(int events) {
    QJsonArray history;
    for (int i = 0; i < events; ++i) {
      history.append(QJsonObject{
        {"event_id", QString("event-%1").arg(i)}, {"actor_id", "local:bench"},
        {"reason", "scale fixture interpretation"},
        {"timestamp", QDateTime::fromMSecsSinceEpoch(1'700'000'000'000LL + i, QTimeZone::utc()).toString(Qt::ISODateWithMs)},
        {"before_revision", i}, {"after_revision", i + 1}});
    }
    return QString::fromUtf8(QJsonDocument(history).toJson(QJsonDocument::Compact));
  }
  static QString interpretationJson(int index, const QString &horizon, int events, const QString &history) {
    QString json = QString("{\"id\":\"i-%1\",\"assertion_id\":\"a-%1\",\"feature_uid\":\"f-%1\","
                           "\"horizon\":\"%2\",\"status\":\"NeedsReview\",\"revision\":%3,\"history\":")
                       .arg(index).arg(horizon).arg(events);
    json += history;
    json += QLatin1Char('}');
    return json;
  }
  static bool writeFeatures(const QString &path, const QString &horizon, int count, int events, QString *error) {
    QgsFields fields;
    fields.append(QgsField("paleo_feature_uid", QMetaType::Type::QString));
    fields.append(QgsField("facies_code", QMetaType::Type::Int));
    fields.append(QgsField("paleo_interpretation", QMetaType::Type::QString));
    QgsVectorFileWriter::SaveVectorOptions options;
    options.driverName = QStringLiteral("GPKG");
    options.layerName = QStringLiteral("facies_polygons");
    options.fileEncoding = QStringLiteral("UTF-8");
    std::unique_ptr<QgsVectorFileWriter> writer(QgsVectorFileWriter::create(
        path, fields, Qgis::WkbType::Point, QgsCoordinateReferenceSystem(QStringLiteral("EPSG:3857")),
        QgsCoordinateTransformContext(), options));
    if (!writer || writer->hasError() != QgsVectorFileWriter::NoError) {
      if (error) *error = writer ? writer->errorMessage() : QStringLiteral("无法创建规模夹具");
      return false;
    }
    const auto history = historyJson(events);
    for (int n = 0; n < count; ++n) {
      QgsFeature feature(fields);
      feature.setAttribute(0, QString("f-%1").arg(n));
      feature.setAttribute(1, n % 7);
      feature.setAttribute(2, interpretationJson(n, horizon, events, history));
      feature.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(n % 1000, n / 1000)));
      if (!writer->addFeature(feature)) {
        if (error) *error = writer->lastError();
        return false;
      }
    }
    return true;
  }
  static qint64 peakKb() {
    QFile status("/proc/self/status");
    if (!status.open(QIODevice::ReadOnly)) return -1;
    for (const auto &line : QString::fromUtf8(status.readAll()).split('\n'))
      if (line.startsWith("VmHWM:")) return line.split(' ', Qt::SkipEmptyParts).value(1).toLongLong();
    return -1;
  }
  static bool waitIdle(MapVersionController &controller, int budgetMs) {
    QElapsedTimer timer; timer.start();
    while (controller.snapshotBusy()) {
      if (timer.elapsed() > budgetMs) return false;
      QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    QCoreApplication::processEvents();
    return true;
  }
private slots:
  void corruptFeatureRejectsSaveWithoutVersion() {
    Fixture fixture; QVERIFY(fixture.init()); QString error;
    const auto path = fixture.dir.filePath("corrupt.gpkg");
    QVERIFY2(writeFeatures(path, "D61", 500, 1, &error), qPrintable(error));
    QgsVectorLayer layer(path + "|layername=facies_polygons", "corrupt", "ogr");
    QVERIFY(layer.isValid()); QVERIFY(layer.startEditing());
    QgsFeature feature; QVERIFY(layer.getFeatures().nextFeature(feature));
    QVERIFY(layer.changeAttributeValue(feature.id(), layer.fields().indexOf("paleo_interpretation"), "{"));
    QVERIFY(layer.commitChanges());
    LayerDeclaration declaration; declaration.layerId = "facies.D61"; declaration.horizon = "D61";
    declaration.type = "vector"; declaration.source = layer.source();
    QVERIFY2(fixture.layers.declare(declaration, &error), qPrintable(error));
    QCOMPARE(fixture.controller.saveVersion("D61", {}, &error).version, 0);
    QVERIFY(error.contains(QStringLiteral("损坏")));
    QCOMPARE(fixture.versions.currentVersion("D61"), 0);
    QCOMPARE(QDir(fixture.dir.filePath("artifacts/map-versions")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
  }
  void measuredSavePublishAndReopen_data() {
    QTest::addColumn<int>("count"); QTest::addColumn<int>("events");
    QTest::newRow("10k-short") << 10000 << 1;
    QTest::newRow("10k-long") << 10000 << 16;
    QTest::newRow("100k-short") << 100000 << 1;
    QTest::newRow("100k-long") << 100000 << 16;
  }
  void measuredSavePublishAndReopen() {
    QFETCH(int, count); QFETCH(int, events);
    Fixture fixture; QVERIFY(fixture.init());
    PaleoTaskService tasks; tasks.setMaxWorkerThreads(1); fixture.controller.setTaskService(&tasks);
    QElapsedTimer setup; setup.start(); QString error;
    const auto mainPath = fixture.dir.filePath("main.gpkg");
    const auto otherPath = fixture.dir.filePath("other.gpkg");
    QVERIFY2(writeFeatures(mainPath, "D61", count, events, &error), qPrintable(error));
    QVERIFY2(writeFeatures(otherPath, "D62", 32, 1, &error), qPrintable(error));
    const auto pdf = fixture.dir.filePath("layout.pdf");
    QFile pdfFile(pdf); QVERIFY(pdfFile.open(QIODevice::WriteOnly)); QVERIFY(pdfFile.write("%PDF-1.4 scale\n") > 0); pdfFile.close();
    const auto pdfSha = DataCatalog::sha256FileHex(pdf);
    QCOMPARE(pdfSha.size(), 64);
    QVERIFY2(fixture.versions.recordLayoutProduct("D61", pdf, "scale-pdf", pdfSha, &error), qPrintable(error));
    LayerDeclaration main; main.layerId = "facies.D61"; main.horizon = "D61"; main.type = "vector";
    main.source = mainPath + "|layername=facies_polygons";
    LayerDeclaration other; other.layerId = "facies.D62"; other.horizon = "D62"; other.type = "vector";
    other.source = otherPath + "|layername=facies_polygons";
    QVERIFY2(fixture.layers.declare(main, &error), qPrintable(error));
    QVERIFY2(fixture.layers.declare(other, &error), qPrintable(error));
    const auto setupMs = setup.elapsed();

    auto run = [&](const QString &horizon, const QString &operation, qint64 *prepareMs, qint64 *workerMs, qint64 *uiGapMs) {
      QSignalSpy finished(&fixture.controller, &MapVersionController::snapshotFinished);
      qint64 gap = 0; QElapsedTimer gaps; gaps.start();
      QTimer pulse; pulse.setTimerType(Qt::PreciseTimer);
      connect(&pulse, &QTimer::timeout, &fixture.controller, [&] { gap = std::max(gap, gaps.restart()); });
      pulse.start(20);
      QElapsedTimer clock; clock.start();
      const bool started = operation == "publish" ? fixture.controller.startPublish(horizon,
          "{\"wells_total\":1,\"covered\":1,\"missing\":[],\"rows\":[{\"kind\":\"residual\",\"residual_ms\":1}]}", &error)
          : fixture.controller.startSaveVersion(horizon, {{"scale", true}, {"events", events}}, &error);
      *prepareMs = clock.elapsed();
      const bool idle = started && waitIdle(fixture.controller, 600000);
      *workerMs = clock.elapsed() - *prepareMs; *uiGapMs = gap; pulse.stop();
      return started && idle && finished.size() == 1 && finished.at(0).at(2).toBool();
    };
    qint64 prepareMs = 0, workerMs = 0, uiGapMs = 0;
    QVERIFY2(run("D61", "save", &prepareMs, &workerMs, &uiGapMs), qPrintable(error + fixture.controller.property("x").toString()));
    const auto first = fixture.versions.latest("D61");
    QCOMPARE(first.version, 1);
    const auto firstProvenance = first.provenance;
    const auto document = QJsonDocument::fromJson(firstProvenance.toUtf8()).object();
    QCOMPARE(document.value("feature_summary").toObject().value("feature_count").toInt(), count);
    qint64 bytes = 0; int reopened = 0;
    for (const auto &entry : document.value("saved_layers").toArray()) {
      const auto row = entry.toObject();
      const auto savedPath = fixture.dir.filePath(row.value("path").toString());
      bytes += QFileInfo(savedPath).size();
      QgsVectorLayer saved(savedPath + row.value("source_suffix").toString(), "saved", "ogr");
      QVERIFY2(saved.isValid(), qPrintable(savedPath));
      QCOMPARE(saved.featureCount(), qint64(count));
      QgsFeatureRequest request; request.setFilterExpression("paleo_feature_uid = 'f-0'"); request.setLimit(1);
      QgsFeature sample; QVERIFY(saved.getFeatures(request).nextFeature(sample));
      QVariantMap interpretation;
      QVERIFY2(paleo::evidence::decodeInterpretation(sample.attribute("paleo_interpretation").toString(), &interpretation, &error), qPrintable(error));
      QCOMPARE(interpretation.value("revision").toInt(), events);
      QCOMPARE(interpretation.value("feature_uid").toString(), QString("f-0"));
      ++reopened;
    }
    QCOMPARE(reopened, 1);

    auto *live = qobject_cast<QgsVectorLayer *>(fixture.layers.instantiate("facies.D61", &error));
    QVERIFY(live); QVERIFY(live->startEditing());
    QgsFeature editable; QVERIFY(live->getFeatures().nextFeature(editable));
    QVERIFY(live->changeAttributeValue(editable.id(), live->fields().indexOf("facies_code"), 99));
    qint64 secondPrepare = 0, secondWorker = 0, secondGap = 0;
    QVERIFY2(run("D61", "save", &secondPrepare, &secondWorker, &secondGap), qPrintable(error));
    const auto second = fixture.versions.latest("D61");
    QCOMPARE(second.version, 2);
    MapVersion persistedFirst;
    for (const auto &version : fixture.versions.versions("D61")) if (version.version == 1) persistedFirst = version;
    QCOMPARE(persistedFirst.provenance, firstProvenance);
    const auto secondDocument = QJsonDocument::fromJson(second.provenance.toUtf8()).object();
    QVERIFY(secondDocument.value("saved_layers").toArray().first().toObject().value("sha256") !=
            document.value("saved_layers").toArray().first().toObject().value("sha256"));

    qint64 otherPrepare = 0, otherWorker = 0, otherGap = 0;
    QVERIFY2(run("D62", "save", &otherPrepare, &otherWorker, &otherGap), qPrintable(error));
    QCOMPARE(fixture.versions.latest("D62").version, 1);
    qint64 publishPrepare = 0, publishWorker = 0, publishGap = 0;
    QVERIFY2(run("D61", "publish", &publishPrepare, &publishWorker, &publishGap), qPrintable(error));
    QVERIFY(fixture.versions.isPublished("D61"));
    QCOMPARE(fixture.versions.latest("D61").provenance, second.provenance);
    for (const auto &version : fixture.versions.versions("D61")) if (version.version == 1) QCOMPARE(version.provenance, firstProvenance);

    const auto beforeCancel = fixture.versions.currentVersion("D61");
    QElapsedTimer cancelClock; cancelClock.start();
    QVERIFY2(fixture.controller.startSaveVersion("D61", {{"scale", "cancel"}}, &error), qPrintable(error));
    fixture.controller.cancelSnapshot();
    QVERIFY(waitIdle(fixture.controller, 600000));
    const auto cancelMs = cancelClock.elapsed();
    const auto afterCancel = fixture.versions.currentVersion("D61");

    qInfo("BASELINE mapsnapshot_features = %d", count);
    qInfo("BASELINE mapsnapshot_history_events = %d", events);
    qInfo("BASELINE mapsnapshot_setup_ms = %lld", setupMs);
    qInfo("BASELINE mapsnapshot_prepare_ms = %lld", prepareMs);
    qInfo("BASELINE mapsnapshot_save_worker_ms = %lld", workerMs);
    qInfo("BASELINE mapsnapshot_ui_gap_ms = %lld", uiGapMs);
    qInfo("BASELINE mapsnapshot_bytes = %lld", bytes);
    qInfo("BASELINE mapsnapshot_reopened_layers = %d", reopened);
    qInfo("BASELINE mapsnapshot_second_prepare_ms = %lld", secondPrepare);
    qInfo("BASELINE mapsnapshot_second_worker_ms = %lld", secondWorker);
    qInfo("BASELINE mapsnapshot_other_horizon_features = 32");
    qInfo("BASELINE mapsnapshot_other_horizon_worker_ms = %lld", otherWorker);
    qInfo("BASELINE mapsnapshot_publish_prepare_ms = %lld", publishPrepare);
    qInfo("BASELINE mapsnapshot_publish_worker_ms = %lld", publishWorker);
    qInfo("BASELINE mapsnapshot_publish_ui_gap_ms = %lld", publishGap);
    qInfo("BASELINE mapsnapshot_cancel_ms = %lld", cancelMs);
    qInfo("BASELINE mapsnapshot_cancel_added_version = %d", afterCancel > beforeCancel ? 1 : 0);
    qInfo("BASELINE mapsnapshot_vmhwm_kb = %lld", peakKb());
    QVERIFY(tasks.shutdown());
  }
};
int main(int argc, char **argv) {
  if (qgetenv("QT_QPA_PLATFORM").isEmpty()) qputenv("QT_QPA_PLATFORM", "offscreen");
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true);
  app.initQgis();
  TestMapSnapshotScale test;
  const auto result = QTest::qExec(&test, argc, argv);
  QgsApplication::exitQgis();
  return result;
}
#include "tst_mapsnapshot_scale.moc"
