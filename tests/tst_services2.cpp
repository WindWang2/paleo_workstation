#include <QtTest>
#include <QTemporaryDir>
#include <QSignalSpy>

#include <qgsapplication.h>
#include <qgsgeometry.h>
#include <qgslayout.h>
#include <qgslayoutitemlabel.h>
#include <qgslayoutmanager.h>
#include <qgslayoutpagecollection.h>
#include <qgsnativealgorithms.h>
#include <qgsprocessingregistry.h>
#include <qgsproject.h>
#include <qgsrenderer.h>
#include <qgsvectorlayer.h>

#include "../src/algorithms/paleoalgorithms.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgiseditingservice.h"
#include "../src/qgis/qgislayoutservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisstyleservice.h"

// Wave-2 services acceptance:
//  - editing sessions mark the layer busy in PaleoProjectStore ("edit" task,
//    "editing in progress" reason) and free it on commit AND on rollback/failure;
//  - processing runs a registry algorithm synchronously and remaps unspecified
//    destination params into a QTemporaryDir (§41.2 temp-then-merge contract);
//  - style service applies stylesRoot/<ref>.qml via loadNamedStyle;
//  - layout service drives QgsLayoutManager CRUD + PDF export.
class TestServices2 : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase()
  {
    QVERIFY(QgsApplication::instance() != nullptr);
    // initQgis() does not auto-register providers in this build — the native
    // provider is added here so "native:buffer" exists regardless of which
    // other test binaries ran first (each test binary is its own process).
    QgsProcessingRegistry *reg = QgsApplication::processingRegistry();
    if (!reg->algorithmById(QStringLiteral("native:buffer")))
      QVERIFY(reg->addProvider(new QgsNativeAlgorithms(reg)));
    QVERIFY(reg->algorithmById(QStringLiteral("native:buffer")) != nullptr);
  }

  // (a) begin -> busy("edit","editing in progress"); commit via store queue ->
  // feature persisted, layer freed, signals fired
  void editingCommitMarksAndFrees()
  {
    PaleoProjectStore store;
    QgisEditingService svc(&store);
    QSignalSpy startedSpy(&svc, &QgisEditingService::editStarted);
    QSignalSpy committedSpy(&svc, &QgisEditingService::editCommitted);

    QgsVectorLayer layer(QStringLiteral("Point?crs=EPSG:4326"), QStringLiteral("pts"), QStringLiteral("memory"));
    QVERIFY(layer.isValid());
    QVERIFY(!store.layerBusy(layer.id()));

    QString err;
    QVERIFY2(svc.beginEdit(&layer, &err), qPrintable(err));
    QVERIFY(svc.isEditing(&layer));

    QString reason;
    QVERIFY(store.layerBusy(layer.id(), &reason));
    QCOMPARE(reason, QStringLiteral("edit — editing in progress")); // "taskId — reason" contract
    QCOMPARE(startedSpy.count(), 1);
    QCOMPARE(startedSpy.at(0).at(0).toString(), layer.id());

    // edits happen against the layer's edit buffer
    QgsFeature f(layer.fields());
    f.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(1.0, 2.0)));
    QVERIFY(layer.addFeature(f));

    QVERIFY2(svc.commitEdit(&layer, &err), qPrintable(err));
    QVERIFY(!svc.isEditing(&layer));      // commitChanges(stopEditing=true)
    QVERIFY(!store.layerBusy(layer.id())); // freed on success
    QCOMPARE(committedSpy.count(), 1);
    QCOMPARE(committedSpy.at(0).at(0).toString(), layer.id());
    QCOMPARE(layer.featureCount(), 1);    // edit buffer flushed to the provider
  }

  // (b) rollback discards the buffer and frees the layer
  void editingRollbackFrees()
  {
    PaleoProjectStore store;
    QgisEditingService svc(&store);
    QSignalSpy rolledBackSpy(&svc, &QgisEditingService::editRolledBack);

    QgsVectorLayer layer(QStringLiteral("Point?crs=EPSG:4326"), QStringLiteral("pts"), QStringLiteral("memory"));
    QVERIFY(layer.isValid());

    QVERIFY(svc.beginEdit(&layer));
    QVERIFY(store.layerBusy(layer.id()));

    QgsFeature f(layer.fields());
    f.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(3.0, 4.0)));
    QVERIFY(layer.addFeature(f));

    QVERIFY(svc.rollbackEdit(&layer));
    QVERIFY(!svc.isEditing(&layer));
    QVERIFY(!store.layerBusy(layer.id()));
    QCOMPARE(rolledBackSpy.count(), 1);
    QCOMPARE(layer.featureCount(), 0); // buffer discarded
  }

  // (c) commit failure still frees the busy mark (no stuck "editing in progress")
  void editingCommitFailureStillFrees()
  {
    PaleoProjectStore store;
    QgisEditingService svc(&store);

    QgsVectorLayer layer(QStringLiteral("Point?crs=EPSG:4326"), QStringLiteral("pts"), QStringLiteral("memory"));
    QVERIFY(layer.isValid());
    // simulate a session whose busy mark outlived the edit state
    store.markLayerBusy(layer.id(), QStringLiteral("edit"), QStringLiteral("editing in progress"));

    QString err;
    QVERIFY(!svc.commitEdit(&layer, &err)); // commitChanges on non-editable layer -> false
    QVERIFY(!err.isEmpty());
    QVERIFY(!store.layerBusy(layer.id()));  // freed on failure too

    // null layer is a clean error, not a crash
    err.clear();
    QVERIFY(!svc.beginEdit(nullptr, &err));
    QVERIFY(!err.isEmpty());
  }

  // (d) run() resolves a registry algorithm, remaps the missing OUTPUT into the
  // service temp dir, and the result map points at a real, loadable layer file
  void processingRunNativeAlgLandsTempOutput()
  {
    PaleoProjectStore store;
    QgisProcessingService svc(&store);

    QgsVectorLayer layer(QStringLiteral("Point?crs=EPSG:4326&field=id:integer"), QStringLiteral("pts"), QStringLiteral("memory"));
    QVERIFY(layer.isValid());
    QgsFeature f(layer.fields());
    f.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(10.0, 20.0)));
    QVERIFY(layer.dataProvider()->addFeature(f));
    layer.updateExtents();
    QCOMPARE(layer.featureCount(), 1);

    QVariantMap params;
    params.insert(QStringLiteral("INPUT"), QVariant::fromValue(&layer));
    params.insert(QStringLiteral("DISTANCE"), 10.0);
    // no OUTPUT on purpose: the service must remap destination params itself

    QString err;
    const QVariantMap out = svc.run(QStringLiteral("native:buffer"), params, &err);
    QVERIFY2(!out.isEmpty(), qPrintable(err));

    const QString outPath = out.value(QStringLiteral("OUTPUT")).toString();
    QVERIFY2(!outPath.isEmpty(), "expected OUTPUT key in algorithm results");
    QVERIFY2(QFile::exists(outPath), qPrintable(outPath)); // landed on disk

    QgsVectorLayer buffered(outPath, QStringLiteral("buffered"), QStringLiteral("ogr"));
    QVERIFY2(buffered.isValid(), qPrintable(buffered.error().message()));
    QVERIFY(buffered.featureCount() >= 1);
  }

  // (e) unknown algorithm id -> empty map + error, no crash
  void processingRunUnknownAlgFailsCleanly()
  {
    PaleoProjectStore store;
    QgisProcessingService svc(&store);
    QString err;
    const QVariantMap out = svc.run(QStringLiteral("nope:does_not_exist"), QVariantMap(), &err);
    QVERIFY(out.isEmpty());
    QVERIFY(!err.isEmpty());
  }

  // (f) paleoAlgorithmIds returns only paleo namespace ids from the registry.
  // Tolerant both ways: 0 entries while paleoalgorithms.cpp is unlinked, >0
  // after integration — but never a non-paleo id.
  void paleoAlgorithmIdsFiltered()
  {
    PaleoProjectStore store;
    QgisProcessingService svc(&store);
    const QStringList ids = svc.paleoAlgorithmIds();
    QVERIFY(!ids.contains(QStringLiteral("native:buffer")));
    for (const QString &id : ids)
      QVERIFY2(id.startsWith(QStringLiteral("paleo")), qPrintable(id));
  }

  // (g) applyStyle loads stylesRoot/<ref>.qml; availableStyles lists basenames
  void styleApplyFromStylesRoot()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    // hand-written minimal QGIS 4.x .qml: single red marker symbol
    const QString qml = QStringLiteral(
      "<!DOCTYPE qgis PUBLIC 'http://mrcc.com/qgis.dtd' 'SYSTEM'>\n"
      "<qgis version=\"4.2.2\" styleCategories=\"Symbology\">\n"
      "  <renderer-v2 type=\"singleSymbol\" enableorderby=\"0\" forceraster=\"0\" referencescale=\"-1\" symbollevels=\"0\">\n"
      "    <symbols>\n"
      "      <symbol alpha=\"1\" clip_to_extent=\"1\" force_rhr=\"0\" frame_rate=\"10\" is_animated=\"0\" name=\"0\" type=\"marker\">\n"
      "        <layer class=\"SimpleMarker\" enabled=\"1\" id=\"{00000000-0000-0000-0000-000000000001}\" locked=\"0\" pass=\"0\">\n"
      "          <Option type=\"Map\">\n"
      "            <Option name=\"name\" type=\"QString\" value=\"circle\"/>\n"
      "            <Option name=\"color\" type=\"QString\" value=\"200,30,30,255,rgb:0.7843137,0.1176471,0.1176471,1\"/>\n"
      "            <Option name=\"size\" type=\"QString\" value=\"4\"/>\n"
      "            <Option name=\"size_unit\" type=\"QString\" value=\"MM\"/>\n"
      "          </Option>\n"
      "        </layer>\n"
      "      </symbol>\n"
      "    </symbols>\n"
      "    <rotation/>\n"
      "    <sizescale/>\n"
      "  </renderer-v2>\n"
      "</qgis>\n");
    QFile qmlFile(tmp.filePath(QStringLiteral("redpoint.qml")));
    QVERIFY(qmlFile.open(QIODevice::WriteOnly));
    QCOMPARE(qmlFile.write(qml.toUtf8()), qint64(qml.toUtf8().size()));
    qmlFile.close();
    // a non-style file must not appear in availableStyles
    QFile other(tmp.filePath(QStringLiteral("notes.txt")));
    QVERIFY(other.open(QIODevice::WriteOnly));
    other.write("not a style");
    other.close();

    QgisStyleService svc;
    svc.setStylesRoot(tmp.path());
    QCOMPARE(svc.availableStyles(), QStringList({QStringLiteral("redpoint")}));

    QgsVectorLayer layer(QStringLiteral("Point?crs=EPSG:4326"), QStringLiteral("pts"), QStringLiteral("memory"));
    QVERIFY(layer.isValid());
    QVERIFY(layer.renderer() != nullptr);

    QString err;
    QVERIFY2(svc.applyStyle(&layer, QStringLiteral("redpoint"), &err), qPrintable(err));
    QCOMPARE(layer.renderer()->type(), QStringLiteral("singleSymbol"));

    // missing ref -> false + error
    QVERIFY(!svc.applyStyle(&layer, QStringLiteral("ghost"), &err));
    QVERIFY(!err.isEmpty());
    // null layer -> false + error
    QVERIFY(!svc.applyStyle(nullptr, QStringLiteral("redpoint"), &err));
    QVERIFY(!err.isEmpty());
  }

  // (h) layout lifecycle: create (duplicate rejected) -> pdf export -> remove
  void layoutLifecycleAndPdfExport()
  {
    QgsProject project;
    QgisLayoutService svc(&project);
    QSignalSpy addedSpy(&svc, &QgisLayoutService::layoutAdded);
    QSignalSpy removedSpy(&svc, &QgisLayoutService::layoutRemoved);

    QVERIFY(svc.layoutNames().isEmpty());

    QString err;
    QgsLayout *layout = svc.createLayout(QStringLiteral("Map 1"), &err);
    QVERIFY2(layout != nullptr, qPrintable(err));
    QCOMPARE(svc.layoutNames(), QStringList({QStringLiteral("Map 1")}));
    QCOMPARE(svc.layout(QStringLiteral("Map 1")), layout);
    QCOMPARE(addedSpy.count(), 1);
    QCOMPARE(addedSpy.at(0).at(0).toString(), QStringLiteral("Map 1"));

    // duplicate name rejected with error
    QVERIFY(svc.createLayout(QStringLiteral("Map 1"), &err) == nullptr);
    QVERIFY(!err.isEmpty());

    // give the page real content so the exporter has something to render
    if (layout->pageCollection()->pageCount() == 0)
      layout->pageCollection()->extendByNewPage();
    auto *label = new QgsLayoutItemLabel(layout);
    label->setText(QStringLiteral("paleo layout test"));
    label->attemptSetSceneRect(QRectF(10, 10, 80, 20));
    layout->addLayoutItem(label);

    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString pdfPath = tmp.filePath(QStringLiteral("map1.pdf"));
    err.clear();
    QVERIFY2(svc.exportPdf(QStringLiteral("Map 1"), pdfPath, &err), qPrintable(err));
    QVERIFY(QFile::exists(pdfPath));
    QVERIFY(QFileInfo(pdfPath).size() > 0); // non-empty PDF

    // export of unknown layout -> false + error
    QVERIFY(!svc.exportPdf(QStringLiteral("ghost"), pdfPath, &err));
    QVERIFY(!err.isEmpty());

    QVERIFY(svc.removeLayout(QStringLiteral("Map 1")));
    QCOMPARE(removedSpy.count(), 1);
    QCOMPARE(removedSpy.at(0).at(0).toString(), QStringLiteral("Map 1"));
    QVERIFY(svc.layoutNames().isEmpty());
    QVERIFY(svc.layout(QStringLiteral("Map 1")) == nullptr);
    QVERIFY(!svc.removeLayout(QStringLiteral("Map 1"))); // second remove fails
  }
};

int main(int argc, char *argv[])
{
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(QStringLiteral("/usr"), true); // distro install
  app.initQgis();
  TestServices2 tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_services2.moc"
