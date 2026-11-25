#include <QtTest>
#include <QUndoStack>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimer>
#include <qgsapplication.h>
#include <qgspolygon.h>
#include "../src/qgis/faciespartitioneditor.h"
#include "../src/qgis/faciesmapboundary.h"
#include "../src/qgis/faciespartitiontool.h"
#include "../src/qgis/mappingfeatureidentity.h"
#include "../src/qgis/qgisruntime.h"
#include <qgsvectorlayer.h>
#include <qgsvectordataprovider.h>
#include <qgsadvanceddigitizingdockwidget.h>
#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>
namespace {
QVariantList classification() {
  return {QVariantMap{{"code", 1}, {"name", "河道"}, {"facies", "河流"}, {"subfacies", "曲流河"}, {"microfacies", "河道"}},
          QVariantMap{{"code", 2}, {"name", "边滩"}, {"facies", "河流"}, {"subfacies", "曲流河"}, {"microfacies", "边滩"}},
          QVariantMap{{"code", 3}, {"name", "河口坝"}, {"facies", "三角洲"}, {"subfacies", "前缘"}, {"microfacies", "河口坝"}}};
}
struct Fixture {
  QgsVectorLayer layer{"MultiPolygon?crs=EPSG:3857&field=facies_code:integer&field=facies_name:string&field=subfacies:string&field=microfacies:string&field=facies_label:string&field=facies_evidence:string&field=paleo_interpretation:string&field=value_min:double&field=value_max:double", "partition", "memory"};
  QgsFeatureIds ids;
  bool init(QString *error, bool sameClass = false, bool hole = false, bool gap = false, bool cap = false) {
    QgsFeatureList features;
    for (int i = 0; i < 2; ++i) {
      QgsFeature f(layer.fields());
      const int start = gap && i == 1 ? 200 : i * 100;
      auto geometry = QgsGeometry::fromRect(QgsRectangle(start, 0, start + 100, 100));
      if (hole && i == 0) geometry = geometry.difference(QgsGeometry::fromRect(QgsRectangle(30, 30, 40, 40)));
      geometry.convertToMultiType(); f.setGeometry(geometry);
      f.setAttribute("facies_code", sameClass ? 1 : i + 1);
      f.setAttribute("facies_evidence", "[]"); f.setAttribute("paleo_interpretation", "{}");
      f.setAttribute("value_min", .2); f.setAttribute("value_max", .4); features << f;
    }
    if (cap) {
      QgsFeature f(layer.fields()); f.setAttribute("facies_code", 1);
      auto geometry = QgsGeometry::fromWkt("POLYGON((0 100,100 100,200 100,200 200,0 200,0 100))");
      geometry.convertToMultiType(); f.setGeometry(geometry); features << f;
    }
    if (!layer.dataProvider()->addFeatures(features)) return false;
    layer.updateExtents();
    for (const auto &f : features) ids.insert(f.id());
    return FaciesPartitionEditor::install(&layer, classification(), {}, error) &&
        MappingFeatureIdentity::Observer::attach(&layer, error) && layer.startEditing();
  }
  QgsFeature first() { auto it = layer.getFeatures(); QgsFeature f; it.nextFeature(f); return f; }
  double area() { double sum = 0; auto it = layer.getFeatures(); QgsFeature f;
    while (it.nextFeature(f)) sum += f.geometry().area(); return sum; }
};
QStringList parents(const QgsFeature &f) {
  QStringList result;
  for (const auto &id : QJsonDocument::fromJson(f.attribute(MappingFeatureIdentity::ParentsField).toString().toUtf8()).toVariant().toList()) result << id.toString();
  return result;
}
struct Capture {
  QgsMapCanvas canvas;
  QgsAdvancedDigitizingDockWidget cad{&canvas};
  FaciesPartitionTool tool;
  Capture(QgsVectorLayer &layer, FaciesPartitionTool::Operation operation)
      : tool(&canvas, &cad, &layer, operation, [] { return 3; }) {
    canvas.resize(600, 400);
    canvas.setDestinationCrs(layer.crs());
    canvas.setExtent(QgsRectangle(-20, -20, 260, 130));
    canvas.setLayers({&layer});
    canvas.setCurrentLayer(&layer);
    canvas.setMapTool(&tool);
  }
  void click(double x, double y, Qt::MouseButton button = Qt::LeftButton,
             Qt::KeyboardModifiers modifiers = {}) {
    const auto p = canvas.getCoordinateTransform()->transform(QgsPointXY(x, y));
    const QPoint pixel(qRound(p.x()), qRound(p.y()));
    QgsMapMouseEvent press(&canvas, QEvent::MouseButtonPress, pixel, button, button, modifiers);
    QgsMapMouseEvent release(&canvas, QEvent::MouseButtonRelease, pixel, button, Qt::NoButton, modifiers);
    tool.canvasPressEvent(&press);
    tool.canvasReleaseEvent(&release);
  }
};
}
#include "faciespartitionbench.h"
class TestFaciesPartition : public QObject {
  Q_OBJECT
private slots:
  void capturePerformance() {
    if (!qEnvironmentVariableIsSet("PALEO_FACIES_BENCH_JSON"))
      QSKIP("Set PALEO_FACIES_BENCH_JSON to record the optional large-grid benchmark.");
    FaciesPartitionBench::run();
  }
  void correctLastVertexOfLongSplit() {
    Fixture f; QString error; QVERIFY2(f.init(&error), qPrintable(error));
    f.layer.selectByIds({f.first().id()});
    Capture capture(f.layer, FaciesPartitionTool::Split);
    QSignalSpy edited(&capture.tool, &FaciesPartitionTool::featureEdited);
    for (int i = 0; i < 20; ++i) capture.click(-10 + i * 5, 45 + i % 2);
    QCOMPARE(capture.tool.captureCurve()->numPoints(), 20);
    capture.click(85, 46, Qt::RightButton);
    QCOMPARE(capture.tool.captureCurve()->numPoints(), 20);
    QCOMPARE(f.layer.undoStack()->count(), 0);
    QCOMPARE(f.layer.selectedFeatureCount(), 1);
    capture.click(110, 45, Qt::LeftButton, Qt::ShiftModifier);
    QCOMPARE(capture.tool.captureCurve()->numPoints(), 20);
    capture.click(110, 45, Qt::RightButton);
    QCOMPARE(edited.count(), 1);
    QCOMPARE(f.layer.featureCount(), 3);
    QCOMPARE(f.layer.undoStack()->count(), 1);
    f.layer.undoStack()->undo();
    QCOMPARE(f.layer.featureCount(), 2);
    QCOMPARE(f.area(), 20000.);
  }
  void deletingSplitTargetOrEndingSessionClearsCapture() {
    Fixture f; QString error; QVERIFY2(f.init(&error), qPrintable(error));
    const auto face = f.first().id();
    f.layer.selectByIds({face});
    Capture capture(f.layer, FaciesPartitionTool::Split);
    capture.click(-10, 45); capture.click(30, 45);
    QCOMPARE(capture.tool.captureCurve()->numPoints(), 2);
    QVERIFY(f.layer.deleteFeature(face));
    QCOMPARE(capture.tool.captureCurve()->numPoints(), 0);
    auto *layer = &f.layer;
    layer->selectByIds({f.first().id()});
    capture.click(120, 20);
    QCOMPARE(capture.tool.captureCurve()->numPoints(), 1);
    QVERIFY(layer->rollBack());
    QCOMPARE(capture.tool.captureCurve()->numPoints(), 0);
  }
  void correctSelfIntersectionAndConfirmClippedStroke() {
    Fixture f; QString error; QVERIFY2(f.init(&error), qPrintable(error));
    Capture capture(f.layer, FaciesPartitionTool::Paint);
    capture.click(20, 20); capture.click(80, 20); capture.click(40, 80); capture.click(20, 0);
    capture.click(20, 0, Qt::RightButton);
    QCOMPARE(capture.tool.captureCurve()->numPoints(), 4);
    QCOMPARE(f.layer.undoStack()->count(), 0);
    QKeyEvent back(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
    static_cast<QgsMapTool &>(capture.tool).keyPressEvent(&back);
    QCOMPARE(capture.tool.captureCurve()->numPoints(), 3);
    capture.click(20, 65); capture.click(20, 65, Qt::RightButton);
    QCOMPARE(f.layer.undoStack()->count(), 1);
    capture.click(220, 10); capture.click(240, 50); capture.click(220, 80);
    capture.click(220, 80, Qt::RightButton);
    QCOMPARE(capture.tool.captureCurve()->numPoints(), 3);
    QCOMPARE(f.layer.undoStack()->count(), 1);
    capture.click(180, 70, Qt::LeftButton, Qt::ShiftModifier);
    capture.click(180, 70, Qt::RightButton);
    QCOMPARE(capture.tool.captureCurve()->numPoints(), 3);
    QCOMPARE(f.layer.undoStack()->count(), 1);
    capture.click(180, 70, Qt::RightButton);
    QCOMPARE(f.layer.undoStack()->count(), 2);
    QCOMPARE(f.area(), 20000.);
    capture.click(20, 20);
    capture.canvas.setCurrentLayer(nullptr);
    QCOMPARE(capture.tool.captureCurve()->numPoints(), 0);
    QVERIFY2(FaciesPartitionEditor::validate(&f.layer, &error), qPrintable(error));
  }
  void initialBoundaryPreservesHolesPartsAndUndo() {
    Fixture draft;
    QgsVectorLayer boundary("MultiPolygon?crs=EPSG:3857", "survey", "memory");
    const auto outline = QgsGeometry::fromWkt(
        "MULTIPOLYGON(((0 0,50 0,50 50,0 50,0 0),(10 10,10 20,20 20,20 10,10 10)),"
        "((70 0,80 0,80 10,70 10,70 0)))");
    QgsFeature area;
    area.setGeometry(outline);
    QgsFeatureList features{area};
    QVERIFY(boundary.dataProvider()->addFeatures(features));
    QString error;
    auto snapshot = FaciesMapBoundary::inspect(&boundary, &error);
    QVERIFY2(!snapshot.isEmpty(), qPrintable(error));
    QVERIFY(!FaciesMapBoundary::thumbnail(snapshot.value("wkb").toByteArray(), QSize(160, 80), Qt::black).isNull());
    QVERIFY(FaciesPartitionEditor::install(&draft.layer, classification(), {}, &error));
    QVERIFY(MappingFeatureIdentity::Observer::attach(&draft.layer, &error));
    QVERIFY(draft.layer.startEditing());
    QVERIFY(!FaciesMapBoundary::seed(&draft.layer, snapshot, 0, &error));
    QCOMPARE(draft.layer.undoStack()->count(), 0);
    QVERIFY2(FaciesMapBoundary::seed(&draft.layer, snapshot, 2, &error), qPrintable(error));
    QCOMPARE(draft.layer.featureCount(), 2);
    QgsGeometry seeded;
    seeded.fromWkb(QByteArray::fromBase64(FaciesPartitionEditor::footprint(&draft.layer).toLatin1()));
    QVERIFY(seeded.isGeosEqual(outline));
    QVERIFY(FaciesPartitionEditor::clippedPaint(&draft.layer, QgsGeometry::fromRect(QgsRectangle(12,12,18,18))).isEmpty());
    const auto overlap = FaciesPartitionEditor::clippedPaint(&draft.layer, QgsGeometry::fromRect(QgsRectangle(-5,0,5,10)));
    QCOMPARE(overlap.area(), 50.);
    draft.layer.undoStack()->undo();
    QCOMPARE(draft.layer.featureCount(), 0);
    QVERIFY(!FaciesPartitionState::hasFrozenFootprint(&draft.layer));
    draft.layer.undoStack()->redo();
    QCOMPARE(draft.layer.featureCount(), 2);
    draft.layer.undoStack()->undo();
    boundary.setCrs(QgsCoordinateReferenceSystem("EPSG:4326"));
    QVERIFY(FaciesMapBoundary::inspect(&boundary, &error).isEmpty());
    snapshot.insert("crs", boundary.crs().toWkt());
    QVERIFY(!FaciesMapBoundary::seed(&draft.layer, snapshot, 2, &error));
  }

  void splitUpdatesNeighborAndUndoesEverything() {
    Fixture f; QString error; QVERIFY2(f.init(&error), qPrintable(error));
    auto original = f.first(); const auto uid = MappingFeatureIdentity::uid(original);
    f.layer.selectByIds({original.id()});
    QVERIFY2(FaciesPartitionEditor::split(&f.layer, QgsGeometry::fromWkt("LINESTRING(-10 50,110 50)"), &error), qPrintable(error));
    QCOMPARE(f.layer.featureCount(), 3); QCOMPARE(f.area(), 20000.);
    QVERIFY2(FaciesPartitionEditor::validate(&f.layer, &error), qPrintable(error));
    QSet<QString> identities;
    auto it = f.layer.getFeatures(); QgsFeature feature;
    while (it.nextFeature(feature)) {
      identities.insert(MappingFeatureIdentity::uid(feature));
      if (feature.attribute("facies_code").toInt() == 1) {
        QCOMPARE(parents(feature), QStringList{uid});
        QCOMPARE(feature.attribute("microfacies").toString(), QString("河道"));
      } else QVERIFY(feature.geometry().constGet()->nCoordinates() > 5);
    }
    QCOMPARE(identities.size(), 3); QCOMPARE(f.layer.undoStack()->count(), 1);
    f.layer.undoStack()->undo(); QCOMPARE(f.layer.featureCount(), 2);
    QCOMPARE(MappingFeatureIdentity::uid(f.layer.getFeature(original.id())), uid);
    f.layer.undoStack()->redo(); QCOMPARE(f.layer.featureCount(), 3);
    QSet<QString> replay; it = f.layer.getFeatures(); while (it.nextFeature(feature)) replay.insert(MappingFeatureIdentity::uid(feature));
    QCOMPARE(replay, identities);
  }
  void paintInsideAndAcrossFacesPreservesCoverageAndClassification() {
    Fixture f; QString error; QVERIFY2(f.init(&error), qPrintable(error));
    const auto polygon = QgsGeometry::fromRect(QgsRectangle(50, 10, 150, 90));
    QVERIFY2(FaciesPartitionEditor::paint(&f.layer, polygon, 3, &error), qPrintable(error));
    QCOMPARE(f.area(), 20000.); QCOMPARE(f.layer.featureCount(), 3);
    const auto painted = f.layer.selectedFeatures().first();
    QCOMPARE(painted.geometry().area(), 8000.); QCOMPARE(parents(painted).size(), 2);
    QCOMPARE(painted.attribute("facies_code").toInt(), 3);
    QCOMPARE(painted.attribute("facies_name").toString(), QString("三角洲"));
    QCOMPARE(painted.attribute("subfacies").toString(), QString("前缘"));
    QCOMPARE(painted.attribute("microfacies").toString(), QString("河口坝"));
    QVERIFY(painted.attribute("value_min").isNull()); QVERIFY(painted.attribute("value_max").isNull());
    QCOMPARE(f.layer.undoStack()->count(), 1); f.layer.undoStack()->undo();
    QVERIFY2(FaciesPartitionEditor::paint(&f.layer, QgsGeometry::fromRect(QgsRectangle(20, 20, 80, 80)), 3, &error), qPrintable(error));
    QCOMPARE(f.area(), 20000.); QCOMPARE(f.layer.featureCount(), 3);
    QCOMPARE(parents(f.layer.selectedFeatures().first()).size(), 1);
  }
  void paintSplitsRemainderAndClipsToFootprint() {
    Fixture f; QString error; QVERIFY2(f.init(&error), qPrintable(error));
    QVERIFY2(FaciesPartitionEditor::paint(&f.layer, QgsGeometry::fromRect(QgsRectangle(40, -10, 60, 110)), 3, &error), qPrintable(error));
    QCOMPARE(f.area(), 20000.); QCOMPARE(f.layer.featureCount(), 4);
    QVERIFY2(FaciesPartitionEditor::validate(&f.layer, &error), qPrintable(error));
    f.layer.undoStack()->undo(); QCOMPARE(f.layer.featureCount(), 2);
    f.layer.undoStack()->redo(); QCOMPARE(f.layer.featureCount(), 4);
  }
  // #541 子项 1：覆盖绘制裁切原面走 apply() 的 changed 路径。该路径曾漏掉
  // convertToMultiType()（add 路径的 child() 有），GEOS difference 的单部件
  // 结果是 Polygon——落库后 GeoPackage 的 MULTIPOLYGON 列会混进 POLYGON 行。
  void changedGeometryStaysMultiTypeForMultiColumns() {
    Fixture f; QString error; QVERIFY2(f.init(&error), qPrintable(error));
    QCOMPARE(f.layer.wkbType(), Qgis::WkbType::MultiPolygon);
    // 50..150 × 10..90 同时切过两个原面，每个面的 remainder 都是单部件 → changed 路径。
    QVERIFY2(FaciesPartitionEditor::paint(&f.layer, QgsGeometry::fromRect(QgsRectangle(50, 10, 150, 90)), 3, &error), qPrintable(error));
    QCOMPARE(f.layer.featureCount(), 3);
    auto it = f.layer.getFeatures(); QgsFeature feature;
    while (it.nextFeature(feature))
      QCOMPARE(feature.geometry().wkbType(), Qgis::WkbType::MultiPolygon);
    QVERIFY2(FaciesPartitionEditor::validate(&f.layer, &error), qPrintable(error));
    // 撤销/重做后仍不得退化成单部件。
    f.layer.undoStack()->undo(); QCOMPARE(f.layer.featureCount(), 2);
    f.layer.undoStack()->redo(); QCOMPARE(f.layer.featureCount(), 3);
    it = f.layer.getFeatures();
    while (it.nextFeature(feature))
      QCOMPARE(feature.geometry().wkbType(), Qgis::WkbType::MultiPolygon);
  }
  void mergeRequiresExplicitLeafAcrossClassesAndPreservesParents() {
    Fixture f; QString error; QVERIFY2(f.init(&error), qPrintable(error));
    f.layer.selectByIds(f.ids);
    QVERIFY(!FaciesPartitionEditor::merge(&f.layer, 0, &error));
    QCOMPARE(f.layer.featureCount(), 2); QCOMPARE(f.layer.undoStack()->count(), 0);
    QVERIFY2(FaciesPartitionEditor::merge(&f.layer, 3, &error), qPrintable(error));
    QCOMPARE(f.layer.featureCount(), 1); QCOMPARE(f.area(), 20000.);
    QCOMPARE(parents(f.first()).size(), 2); QCOMPARE(f.first().attribute("facies_code").toInt(), 3);
    f.layer.undoStack()->undo(); QCOMPARE(f.layer.featureCount(), 2);
    f.layer.undoStack()->redo(); QCOMPARE(parents(f.first()).size(), 2);
    Fixture same; QVERIFY2(same.init(&error, true), qPrintable(error)); same.layer.selectByIds(same.ids);
    QVERIFY2(FaciesPartitionEditor::merge(&same.layer, 0, &error), qPrintable(error));
    QCOMPARE(same.first().attribute("facies_code").toInt(), 1);
  }
  void invalidRequestsAreAtomicAndFootprintBlocksAccidentalHoles() {
    Fixture f; QString error; QVERIFY2(f.init(&error), qPrintable(error));
    const auto invalid = QgsGeometry::fromWkt("POLYGON((0 0,50 50,50 0,0 50,0 0))");
    QVERIFY(!FaciesPartitionEditor::paint(&f.layer, invalid, 3, &error));
    QVERIFY(!FaciesPartitionEditor::paint(&f.layer, QgsGeometry::fromRect(QgsRectangle(20,20,80,80)), 999, &error));
    QCOMPARE(f.layer.undoStack()->count(), 0); QCOMPARE(f.area(), 20000.);
    f.layer.beginEditCommand("accidental delete"); QVERIFY(f.layer.deleteFeature(f.first().id())); f.layer.endEditCommand();
    QVERIFY(!FaciesPartitionEditor::validate(&f.layer, &error)); QVERIFY(error.contains("覆盖范围"));
    f.layer.undoStack()->undo(); QVERIFY(FaciesPartitionEditor::validate(&f.layer, &error));
  }
  void disconnectedFacesCannotBeMergedAcrossNoData() {
    Fixture f; QString error; QVERIFY2(f.init(&error, false, false, true), qPrintable(error));
    f.layer.selectByIds(f.ids);
    QVERIFY(!FaciesPartitionEditor::merge(&f.layer, 3, &error));
    QVERIFY(error.contains("连通")); QCOMPARE(f.layer.featureCount(), 2);
    QCOMPARE(f.layer.undoStack()->count(), 0); QCOMPARE(f.area(), 20000.);
  }
  void mergeKeepsUntouchedNeighborsJunctions() {
    Fixture f; QString error; QVERIFY2(f.init(&error, false, false, false, true), qPrintable(error));
    QgsFeatureIds selected; auto it = f.layer.getFeatures(); QgsFeature feature;
    while (it.nextFeature(feature)) if (feature.geometry().boundingBox().yMaximum() <= 100) selected.insert(feature.id());
    QCOMPARE(selected.size(), 2); f.layer.selectByIds(selected);
    QVERIFY2(FaciesPartitionEditor::merge(&f.layer, 3, &error), qPrintable(error));
    QCOMPARE(f.layer.featureCount(), 2); QCOMPARE(f.area(), 40000.);
    QVERIFY2(FaciesPartitionEditor::validate(&f.layer, &error), qPrintable(error));
    it = f.layer.getFeatures();
    while (it.nextFeature(feature)) {
      bool junction = false; auto vertices = feature.geometry().vertices();
      while (vertices.hasNext()) { const auto point = vertices.next();
        junction |= qgsDoubleNear(point.x(), 100) && qgsDoubleNear(point.y(), 100); }
      QVERIFY(junction);
    }
  }
  void missingClassificationCannotBypassPartitionValidation() {
    Fixture f; QString error; QVERIFY2(f.init(&error), qPrintable(error));
    f.layer.beginEditCommand("clear class");
    QVERIFY(f.layer.changeAttributeValue(f.first().id(), f.layer.fields().indexOf("facies_code"), QVariant()));
    f.layer.endEditCommand();
    QVERIFY(!FaciesPartitionEditor::validate(&f.layer, &error)); QVERIFY(error.contains("微相编码"));
    f.layer.undoStack()->undo(); QVERIFY(FaciesPartitionEditor::validate(&f.layer, &error));
    f.layer.beginEditCommand("drop class field"); QVERIFY(f.layer.deleteAttribute(f.layer.fields().indexOf("facies_code"))); f.layer.endEditCommand();
    QVERIFY(!FaciesPartitionEditor::validate(&f.layer, &error)); QVERIFY(error.contains("缺少微相编码字段"));
    f.layer.undoStack()->undo(); QVERIFY(FaciesPartitionEditor::validate(&f.layer, &error));
  }
  void intentionalNoDataHoleSurvivesOverlayAndFrozenFootprintRoundTrip() {
    Fixture f; QString error; QVERIFY2(f.init(&error, false, true), qPrintable(error));
    const auto frozen = FaciesPartitionEditor::footprint(&f.layer, &error);
    QVERIFY(!frozen.isEmpty());
    QVERIFY2(FaciesPartitionEditor::paint(&f.layer, QgsGeometry::fromRect(QgsRectangle(-10,-10,210,110)), 3, &error), qPrintable(error));
    QCOMPARE(f.area(), 19900.); QVERIFY(FaciesPartitionEditor::validate(&f.layer, &error));
    QgsGeometry original, current;
    original.fromWkb(QByteArray::fromBase64(frozen.toLatin1()));
    current.fromWkb(QByteArray::fromBase64(FaciesPartitionEditor::footprint(&f.layer, &error).toLatin1()));
    QVERIFY(original.isGeosEqual(current));
  }
  void auditFailureRollsBackTheEntireOperationAndCannotRedo() {
    Fixture f; QString error; QVERIFY2(f.init(&error), qPrintable(error));
    const auto original = f.first();
    auto *observer = MappingFeatureIdentity::Observer::of(&f.layer);
    const auto failure = connect(observer, &MappingFeatureIdentity::Observer::businessEdit, &f.layer,
        [&f](qint64, const QString &) { f.layer.setProperty("paleo.evidenceEditFailure", "injected audit failure"); f.layer.setAllowCommit(false); });
    QVERIFY(!FaciesPartitionEditor::paint(&f.layer, QgsGeometry::fromRect(QgsRectangle(50,10,150,90)), 3, &error));
    QCOMPARE(f.layer.featureCount(), 2); QCOMPARE(f.area(), 20000.);
    QCOMPARE(MappingFeatureIdentity::uid(f.layer.getFeature(original.id())), MappingFeatureIdentity::uid(original));
    QVERIFY(!f.layer.undoStack()->canRedo()); QVERIFY(f.layer.allowCommit());
    QVERIFY(f.layer.property("paleo.evidenceEditFailure").toString().isEmpty());
    disconnect(failure);
    QVERIFY2(FaciesPartitionEditor::paint(&f.layer, QgsGeometry::fromRect(QgsRectangle(50,10,150,90)), 3, &error), qPrintable(error));
  }
};
int main(int argc, char **argv) {
  if (!QgisRuntime::initialize("/usr")) return 1;
  TestFaciesPartition test; const int result = QTest::qExec(&test, argc, argv);
  QgisRuntime::shutdown(); return result;
}
#include "tst_faciespartition.moc"
