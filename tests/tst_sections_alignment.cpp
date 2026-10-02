#include "domain/seismic/sectiongeometry.h"
#include "domain/seismic/sectionwellprojector.h"
#include "domain/seismic/timedepthmodel.h"
#include "linkage/seismicmaplink.h"
#include "qgis/mappingartifactwriter.h"
#include "ui/seismicsection/sectionsetupdialog.h"
#include "ui/seismicsection/seismicsectiondockwidget.h"
#include "workflow/sectionworkbench.h"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QListWidget>
#include <QPushButton>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <cmath>
#include <cstring>
#include <qgsapplication.h>
#include <qgsexpression.h>
#include <qgsexpressioncontext.h>
#include <qgsmapcanvas.h>
#include <qgsvectordataprovider.h>
#include <qgsvectorlayer.h>
#include <qgsvectorlayerlabeling.h>

class TestSectionsAlignment : public QObject {
  Q_OBJECT
private slots:
  void strictTimeDepth() {
    // BIZ-12: Defensive check ordering for empty model and non-finite inputs
    seismic::TimeDepthModel emptyModel;
    QVERIFY(std::isnan(emptyModel.DepthToTwtMs(std::numeric_limits<double>::quiet_NaN())));
    QVERIFY(std::isnan(emptyModel.DepthToTwtMs(std::numeric_limits<double>::infinity())));
    QVERIFY(std::isnan(emptyModel.DepthToTwtMs(-std::numeric_limits<double>::infinity())));
    QVERIFY(std::isnan(emptyModel.TwtMsToDepth(std::numeric_limits<double>::quiet_NaN())));
    QVERIFY(std::isnan(emptyModel.TwtMsToDepth(std::numeric_limits<double>::infinity())));
    QVERIFY(std::isnan(emptyModel.TwtMsToDepth(-std::numeric_limits<double>::infinity())));
    emptyModel.setVelocity(2000.0);
    QCOMPARE(emptyModel.DepthToTwtMs(100.0), 100.0);
    QCOMPARE(emptyModel.TwtMsToDepth(100.0), 100.0);

    seismic::TimeDepthModel model;
    QVERIFY(model.setCheckshots({{100, 80}, {200, 130}, {500, 250}}));
    QCOMPARE(model.DepthToTwtMs(100.0), 80.0);   // exact front
    QCOMPARE(model.DepthToTwtMs(500.0), 250.0);  // exact back
    QCOMPARE(model.TwtMsToDepth(80.0), 100.0);   // exact front
    QCOMPARE(model.TwtMsToDepth(250.0), 500.0);  // exact back
    QCOMPARE(model.DepthToTwtMs(350), 190.0);
    QCOMPARE(model.TwtMsToDepth(190), 350.0);
    QVERIFY(std::isnan(model.DepthToTwtMs(501)));
    QVERIFY(std::isnan(model.DepthToTwtMs(99.9)));
    QVERIFY(std::isnan(model.TwtMsToDepth(79)));
    QVERIFY(std::isnan(model.TwtMsToDepth(250.1)));
    QVERIFY(std::isnan(model.DepthToTwtMs(std::numeric_limits<double>::quiet_NaN())));
    QVERIFY(std::isnan(model.DepthToTwtMs(std::numeric_limits<double>::infinity())));
    QVERIFY(std::isnan(model.TwtMsToDepth(std::numeric_limits<double>::quiet_NaN())));
    QVERIFY(std::isnan(model.TwtMsToDepth(std::numeric_limits<double>::infinity())));
    QVERIFY(!model.setCheckshots({{100, 100}, {200, 90}}));
    QVERIFY(!model.setCheckshots({{100, 100}, {100, 150}}));
    QVERIFY(!model.setCheckshots({{100, 100}}));
    QCOMPARE(model.DepthToTwtMs(350), 190.0); // bad edit is atomic
  }
  void bentAnisotropicGeometry() {
    auto g = seismic::SectionGeometry::fromColumns(
        {{0, 0}, {10, 0}, {10, 10}}, {{0, 0}, {100, 0}, {100, 300}},
        {0, 5, 10, 15, 20});
    QCOMPARE(g.distancesM, (std::vector<float>{0, 50, 100, 250, 400}));
    QCOMPARE(g.coordinates[3].x, 100.0);
    QCOMPARE(g.coordinates[3].y, 150.0);
    seismic::SectionWellInfo w;
    w.surfaceX = 110;
    w.surfaceY = 150;
    w.calibrated = true;
    w.tops.push_back({"H", 500, 450, 900});
    auto wells = seismic::SectionWellProjector::ProjectWells(
        {{0, 0}, {100, 0}, {100, 300}}, {}, {0, 50, 100, 250, 400}, {w}, 20);
    QCOMPARE(wells.front().tracePosition, 3.0);
    QCOMPARE(wells.front().offsetDistanceM, -10.0);
    QCOMPARE(wells.front().tops.front().twtMs,
             900.0); // keep per-well calibration
  }
  void independentCalibrationAndVersions() {
    QTemporaryDir dir;
    DataCatalog catalog;
    QVERIFY(catalog.open(dir.path()));
    for (int i = 1; i <= 2; ++i) {
      CatalogEntity e;
      e.id = QString("well-%1").arg(i);
      e.name = QString("A%1").arg(i);
      e.entityType = "well";
      e.hasSurface = true;
      e.surfaceX = i * 100;
      e.surfaceY = 0;
      e.coordinateStatus = "untransformed";
      e.td = 350;
      QVERIFY(catalog.addEntity(e));
      CatalogAsset a;
      a.id = QString("td-%1").arg(i);
      a.type = "time_depth";
      a.displayName = a.id;
      QVERIFY(catalog.addAsset(a));
      const auto path = dir.filePath(a.id + ".dat");
      QFile file(path);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("# TIME TVDSS TVD MD\n100 0 100 150\n200 0 200 350\n");
      file.close();
      CatalogVersion v;
      v.id = "v-" + a.id;
      v.assetId = a.id;
      v.managed = false;
      v.path = path;
      v.stage = "RAW";
      QVERIFY(catalog.addVersion(v));
      EntityAssetLink l;
      l.entityId = e.id;
      l.entityType = "well";
      l.role = "time_depth";
      l.assetId = a.id;
      QVERIFY(catalog.addLink(l));
    }
    SectionWorkbench work(&catalog);
    QString error;
    auto rows = work.wells();
    QCOMPARE(rows.size(), 2);
    QVERIFY(rows[0].toMap().value("status").toString().contains("TVD / MD"));
    auto data = work.sectionWells();
    QCOMPARE(data[0].bottomTwtMs, 200.0);
    QVERIFY(work.setCalibration("well-1", false, 2500, 25, &error));
    data = work.sectionWells();
    QCOMPARE(data[0].bottomTwtMs, 225.0);
    QCOMPARE(data[1].bottomTwtMs, 200.0);
    auto route = work.wellRoute({"well-2", "well-1"}, &error);
    QCOMPARE(route[0].x, 200.0);
    QCOMPARE(route[1].x, 100.0);
    QVERIFY(work.wellRoute({"well-1", "well-1"}, &error).empty());
    QVERIFY(work.save("A2-A1", route, "/survey.sgy", "H1", &error));
    const auto id = work.savedSections().front().toMap().value("id").toString();
    const auto version = catalog.versionById(id);
    QVERIFY(version.parentVersionIds.contains("v-td-1"));
    QVERIFY(
        QFile::exists(DataCatalog::resolvedVersionPath(dir.path(), version)));
    SectionWorkbench reopened(&catalog);
    QVERIFY(!reopened.restore(id, &error).isEmpty());
    QCOMPARE(reopened.sectionWells()[0].bottomTwtMs, 225.0);
    QVERIFY(reopened.setCalibration("well-1", true, 2000, -10, &error));
    QCOMPARE(reopened.sectionWells()[0].bottomTwtMs, 340.0);
    QVERIFY(reopened.save("A2-A1", route, "/survey.sgy", "H1", &error));
    const auto next = catalog.versionById(
        reopened.savedSections().front().toMap().value("id").toString());
    QCOMPARE(next.versionNumber, 2);
    QVERIFY(next.parentVersionIds.contains(id));
  }
  void engineExtractionLatestWinsAndMapClick() {
    QTemporaryDir dir;
    const auto path = dir.filePath("section.sgy");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArray(3200, ' '));
    QByteArray binary(400, 0);
    auto put16 = [](QByteArray &data, int offset, qint16 value) {
      qToBigEndian(value, reinterpret_cast<uchar *>(data.data()) + offset);
    };
    auto put32 = [](QByteArray &data, int offset, qint32 value) {
      qToBigEndian(value, reinterpret_cast<uchar *>(data.data()) + offset);
    };
    put16(binary, 12, 4);
    put16(binary, 16, 2000);
    put16(binary, 20, 64);
    put16(binary, 24, 5);
    file.write(binary);
    for (int il = 0; il < 4; ++il)
      for (int xl = 0; xl < 4; ++xl) {
        QByteArray header(240, 0);
        put32(header, 0, il * 4 + xl + 1);
        put32(header, 188, 1000 + il);
        put32(header, 192, 2000 + xl);
        put16(header, 114, 64);
        file.write(header);
        QByteArray samples(64 * 4, 0);
        for (int k = 0; k < 64; ++k) {
          const float v = std::sin(k * .4f + il + xl);
          quint32 bits;
          std::memcpy(&bits, &v, 4);
          qToBigEndian(bits, reinterpret_cast<uchar *>(samples.data()) + 4 * k);
        }
        file.write(samples);
      }
    file.close();
    auto volume = std::make_shared<seismic::SgyVolume>();
    std::string error;
    QVERIFY2(volume->Load(path.toStdString(), error), error.c_str());
    seismic::SeismicSectionDockWidget dock;
    dock.resize(1100, 500);
    dock.show();
    dock.setTimeOriginMs(50);
    QSignalSpy done(
        &dock, &seismic::SeismicSectionDockWidget::sectionExtractionFinished);
    dock.extractSectionFromVolumeAsync(volume, {{1000, 2000}, {1003, 2000}},
                                       "old", {{0, 0}, {300, 0}});
    seismic::SectionWellInfo w;
    w.wellName = "A1";
    w.surfaceX = 300;
    w.surfaceY = 100;
    w.calibrated = true;
    w.bottomTwtMs = 170;
    w.alignmentStatus = "时深表";
    w.tops.push_back({"H1", 120, 100, 100});
    dock.extractSectionFromVolumeAsync(
        volume, {{1000, 2000}, {1003, 2000}, {1003, 2003}}, "current",
        {{0, 0}, {300, 0}, {300, 600}}, {w});
    QTRY_VERIFY_WITH_TIMEOUT(done.count() == 1, 10000);
    QVERIFY(done[0][0].toBool());
    QVERIFY(dock.hasRoute());
    QCOMPARE(dock.canvas()->startSampleMs(), 50.0);
    QCOMPARE(dock.canvas()->totalDistanceM(), 900.0);
    QSignalSpy clicked(dock.canvas(),
                       &seismic::SeismicSectionCanvas::traceClicked);
    const int col = dock.canvas()->traceCount() - 2;
    QPoint at(qRound(dock.canvas()->traceToPixelX(col)),
              qRound(dock.canvas()->timeToPixelY(100)));
    QTest::mouseClick(dock.canvas(), Qt::LeftButton, Qt::NoModifier, at);
    QCOMPARE(clicked.count(), 1);
    QCOMPARE(clicked[0][4].toDouble(), 300.0);
    QVERIFY(clicked[0][5].toDouble() > 0);
    dock.grab().save("/tmp/paleo-linked-section.png");
    QgsMapCanvas map;
    map.setDestinationCrs(QgsCoordinateReferenceSystem("EPSG:3857"));
    SeismicMapLink link(&map, nullptr);
    link.setActiveVolume(volume);
    SurveyGridGeometry grid;
    grid.valid = true;
    grid.a = 100;
    grid.d = 100;
    grid.p1Inline = 1000;
    grid.p1Xline = 2000;
    grid.inlineMin = 1000;
    grid.inlineMax = 1003;
    grid.xlineMin = 2000;
    grid.xlineMax = 2003;
    link.setGridGeometry(grid);
    int requests = 0;
    connect(&link, &SeismicMapLink::sectionExtractRequested, this,
            [&] { ++requests; });
    link.triggerSectionFromMapPolyline({{0, 0}, {10000, 10000}, {200, 200}});
    QCOMPARE(requests, 0);
    link.triggerSectionFromMapPolyline({{0, 0}, {100, 200}, {200, 200}});
    QCOMPARE(requests, 1);
    // Destruction before extraction completion must not deliver into a deleted
    // widget.
    auto *closing = new seismic::SeismicSectionDockWidget;
    closing->extractSectionFromVolumeAsync(volume, {{1000, 2000}, {1003, 2003}},
                                           "closing", {{0, 0}, {300, 600}});
    delete closing;
  }
  void labelExpressionsFollowAttributes() {
    for (const auto &geometry : {"Point", "Polygon"}) {
      QgsVectorLayer layer(QString(geometry) +
                               "?field=name:string&field=facies_code:integer&"
                               "field=facies_label:string",
                           "facies", "memory");
      QVERIFY(layer.isValid());
      MappingArtifactWriter::applyFaciesLabels(&layer, 3);
      QVERIFY(layer.labelsEnabled());
      QgsExpression expr(layer.labeling()->settings().fieldName);
      QVERIFY(!expr.hasParserError());
      QgsFeature f(layer.fields());
      f.setAttribute("name", "A1");
      f.setAttribute("facies_label", "河道");
      QgsExpressionContext ctx;
      ctx.setFields(layer.fields());
      ctx.setFeature(f);
      QCOMPARE(expr.evaluate(&ctx).toString(), QString("A1\n河道"));
      f.setAttribute("facies_label", "湖泊");
      ctx.setFeature(f);
      QCOMPARE(expr.evaluate(&ctx).toString(), QString("A1\n湖泊"));
      f.setAttribute("facies_label", QVariant());
      ctx.setFeature(f);
      QVERIFY(expr.evaluate(&ctx).toString().contains("其他 / 未分类"));
      MappingArtifactWriter::applyFaciesLabels(&layer, 0);
      QVERIFY(!layer.labelsEnabled());
    }
  }
  void setupIntentsAndSelectionSurviveRefresh() {
    SectionSetupDialog dialog;
    QVariantList rows{QVariantMap{{"id", "w1"},
                                  {"name", "A1"},
                                  {"coordinates", true},
                                  {"velocity", 2500}},
                      QVariantMap{{"id", "w2"},
                                  {"name", "A2"},
                                  {"coordinates", true},
                                  {"velocity", 2500}}};
    dialog.setWells(rows);
    auto *list = dialog.findChild<QListWidget *>("sectionWells");
    QVERIFY(list);
    list->item(0)->setCheckState(Qt::Checked);
    list->item(1)->setCheckState(Qt::Checked);
    auto *first = list->takeItem(0);
    list->addItem(first);
    list->setCurrentRow(1);
    dialog.setWells(rows);
    QSignalSpy build(&dialog, &SectionSetupDialog::buildRequested);
    dialog.findChild<QPushButton *>("buildWellSection")->click();
    QCOMPARE(build.count(), 1);
    QCOMPARE(build[0][0].toStringList(), QStringList({"w2", "w1"}));
    QSignalSpy alignment(&dialog, &SectionSetupDialog::calibrationRequested);
    dialog.findChild<QDoubleSpinBox *>("alignmentShift")->setValue(32);
    dialog.findChild<QPushButton *>("applyAlignment")->click();
    QCOMPARE(alignment[0][0].toString(), QString("w1"));
    QCOMPARE(alignment[0][3].toDouble(), 32.0);
    dialog.show();
    QTest::qWait(50);
    dialog.grab().save("/tmp/paleo-section-setup.png");
  }
};
int main(int argc, char **argv) {
  QgsApplication app(argc, argv, true);
  app.setPrefixPath("/usr", true);
  app.initQgis();
  TestSectionsAlignment test;
  const int result = QTest::qExec(&test, argc, argv);
  QgsApplication::exitQgis();
  return result;
}
#include "tst_sections_alignment.moc"
