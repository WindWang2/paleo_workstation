#include <QtTest>
#include <QTemporaryDir>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>
#include <QUndoStack>
#include <QAction>
#include <QSpinBox>
#include <QKeyEvent>
#include <QMessageBox>
#include <QAbstractButton>
#include <QTimer>
#include <qgssnappingutils.h>
#include <qgsproject.h>
#include <qgsvectordataprovider.h>
#include "../src/qgis/qgiscanvascontroller.h"
#include "../src/ui/edittools/editingtoolbar.h"
#include "../src/ui/maptools/paleomaptools.h"
#include "../src/ui/maptools/paleoshapetools.h"
#include <qgsapplication.h>
#include <qgsvectorlayer.h>
#include <qgsfeature.h>
#include <qgsgeometry.h>
#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>
#include <qgsprocessingcontext.h>
#include <qgsprocessingutils.h>
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/metadata/layermanifest.h"
#include "../src/workflow/workflows.h"
#include "../src/ui/edittools/vertexeditortools.h"
#include "../src/algorithms/singlefactor/constraintparse.h"

class EditingVertexTool : public PaleoVertexTool
{
public:
  using PaleoVertexTool::PaleoVertexTool;
  using PaleoVertexTool::canvasPressEvent;
  using PaleoVertexTool::canvasReleaseEvent;
  using PaleoVertexTool::canvasDoubleClickEvent;
};
#include "../src/io/constraintstore.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgiseditingservice.h"
#include "../src/qgis/qgisruntime.h"

class EditingLineCapture : public PaleoDrawConstraintTool
{
public:
  using PaleoDrawConstraintTool::PaleoDrawConstraintTool;
  using PaleoDrawConstraintTool::keyPressEvent;
  using PaleoDrawConstraintTool::canvasDoubleClickEvent;
};
class EditingPolygonCapture : public PaleoDrawPolygonTool
{
public:
  using PaleoDrawPolygonTool::PaleoDrawPolygonTool;
  using PaleoDrawPolygonTool::keyPressEvent;
};
class EditingCircleCapture : public PaleoDrawCircleTool
{
public:
  using PaleoDrawCircleTool::PaleoDrawCircleTool;
  using PaleoDrawCircleTool::keyPressEvent;
};

class TestConstraintEditing : public QObject
{
  Q_OBJECT
  struct Fixture
  {
    QTemporaryDir dir;
    PaleoProjectStore project;
    ConstraintStore store{dir.filePath("project.gpkg"), &project};
    QgisEditingService edits{&project};
    std::unique_ptr<QgsVectorLayer> layer;
    QString error;
    bool open()
    {
      project.setProjectPaths(dir.filePath("p.qgz"), store.gpkgPath(), dir.filePath("meta.sqlite"));
      if (!store.appendExtended("T1", "c-1", "LINESTRING(0 0, 10 0, 20 0)",
                                "direction_line", 7, "{\"semantic\":\"direction_guide\",\"ratio\":8,\"opaque\":42}", 1, &error, 0.7123456789012345) ||
          !store.append("T2", "c-2", "POINT(30 30)", "point", 9, &error))
        return false;
      layer = std::make_unique<QgsVectorLayer>(store.gpkgPath()+"|layername=constraints", "constraints", "ogr");
      layer->setSubsetString("horizon='T1'");
      layer->setCustomProperty("paleoLayerId", "constraints.T1");
      return layer->isValid() && edits.beginEdit(layer.get(), &error);
    }
    QgsFeature feature() { return layer->getFeature(store.load("T1").first().value("fid").toLongLong()); }
    bool move(double y)
    {
      auto f = feature();
      auto geometry = f.geometry();
      if (!geometry.moveVertex(10, y, 1))
        return false;
      layer->beginEditCommand("move");
      const bool ok = layer->changeGeometry(f.id(), geometry);
      layer->endEditCommand();
      return ok;
    }
    bool consistent()
    {
      const auto rows = store.load("T1");
      if (rows.size() != layer->featureCount())
        return false;
      for (const auto &row : rows)
      {
        const auto f = layer->getFeature(row.value("fid").toLongLong());
        if (!f.isValid() || !f.geometry().equals(QgsGeometry::fromWkt(row.value("wkt").toString())))
          return false;
        for (const QString &key : {QString("id"), QString("horizon"), QString("type"), QString("facies_code"),
                                   QString("weight"), QString("params_json"), QString("schema_version")})
          if (row.contains(key) ? f.attribute(key) != row.value(key) : !f.attribute(key).isNull())
            return false;
      }
      return true;
    }
  };
private slots:
  void vertexRoundTripUndoRedoSave()
  {
    Fixture f;
    QVERIFY2(f.open(), qPrintable(f.error));
    const auto original = f.store.load("T1");
    const auto other = f.store.load("T2");
    QVERIFY(f.move(5));
    QVERIFY(f.consistent());
    const auto edited = f.store.load("T1");
    QVERIFY(edited != original);
    f.layer->undoStack()->undo();
    QCOMPARE(f.store.load("T1"), original);
    QVERIFY(f.consistent());
    f.layer->undoStack()->redo();
    QCOMPARE(f.store.load("T1"), edited);
    QVERIFY(f.consistent());
    QVERIFY2(f.edits.commitEdit(f.layer.get(), &f.error), qPrintable(f.error));
    QVERIFY(!f.layer->isEditable());
    QCOMPARE(f.layer->undoStack()->count(), 0);
    QCOMPARE(f.store.load("T1"), edited);
    QCOMPARE(f.store.load("T2"), other);
    QVERIFY(f.consistent());
  }
  void vertexGesturesPersistInsertDeleteDrag()
  {
    Fixture f;
    QVERIFY2(f.open(), qPrintable(f.error));
    QgsMapCanvas canvas;
    canvas.resize(500, 300);
    canvas.setDestinationCrs(f.layer->crs());
    canvas.setLayers({f.layer.get()});
    canvas.setCurrentLayer(f.layer.get());
    canvas.setExtent(QgsRectangle(-5, -10, 25, 10));
    canvas.refresh();
    f.layer->selectAll();
    EditingVertexTool tool(&canvas, f.layer.get());
    canvas.setMapTool(&tool);
    const auto px = [&](double x, double y) { return canvas.getCoordinateTransform()->transform(x,y).toQPointF().toPoint(); };
    QgsMapMouseEvent press(&canvas, QEvent::MouseButtonPress, px(10,0), Qt::LeftButton, Qt::LeftButton);
    tool.canvasPressEvent(&press);
    QVERIFY(tool.isDragging());
    QgsMapMouseEvent release(&canvas, QEvent::MouseButtonRelease, px(10,5), Qt::LeftButton);
    tool.canvasReleaseEvent(&release);
    QVERIFY(f.consistent());
    QVERIFY(f.feature().geometry().vertexAt(1).y() > 4.5);
    f.layer->undoStack()->undo();
    QVERIFY(f.consistent());
    QgsMapMouseEvent insert(&canvas, QEvent::MouseButtonDblClick, px(5,0), Qt::LeftButton);
    tool.canvasDoubleClickEvent(&insert);
    QCOMPARE(f.feature().geometry().constGet()->nCoordinates(), 4);
    QVERIFY(f.consistent());
    QgsMapMouseEvent remove(&canvas, QEvent::MouseButtonRelease, px(5,0), Qt::RightButton);
    tool.canvasReleaseEvent(&remove);
    QCOMPARE(f.feature().geometry().constGet()->nCoordinates(), 3);
    QVERIFY(f.consistent());
    canvas.unsetMapTool(&tool);
    QVERIFY(f.edits.rollbackEdit(f.layer.get()));
  }
  void batchSemanticUndoAndNextParser()
  {
    QTemporaryDir dir;
    PaleoProjectStore store;
    const QString gpkg = dir.filePath("project.gpkg");
    store.setProjectPaths(dir.filePath("p.qgz"), gpkg, dir.filePath("m.sqlite"));
    ConstraintStore constraints(gpkg, &store);
    QString error;
    QVERIFY(constraints.appendExtended("T1", "a", "LINESTRING(0 0,10 0)", "direction_line", 1,
                                       "{\"semantic\":\"direction_guide\",\"ratio\":11,\"opaque\":42}", 1, &error));
    QVERIFY(constraints.appendExtended("T1", "b", "LINESTRING(0 5,10 5)", "direction_line", 2,
                                       "{\"semantic\":\"direction_guide\",\"ratio\":7}", 1, &error));
    QgisProjectService project;
    QVERIFY(project.createProject(dir.filePath("p.qgz")));
    LayerManifest manifest(dir.filePath("m.sqlite"));
    QVERIFY(manifest.open());
    QgisLayerService layers(&project, &manifest);
    ConstraintWorkflow workflow(nullptr, &layers);
    workflow.setConstraintStore(&constraints);
    workflow.loadConstraints("T1");
    auto *layer = qobject_cast<QgsVectorLayer *>(layers.instantiate("constraints.T1", &error));
    QVERIFY2(layer, qPrintable(error));
    QgisEditingService edits(&store);
    QVERIFY2(edits.beginEdit(layer, &error), qPrintable(error));
    connect(layer->undoStack(), &QUndoStack::indexChanged, &workflow, [&workflow] {
      workflow.loadConstraints("T1"); // page refresh must not replace a live provider
    });
    const auto original = constraints.load("T1");
    QVERIFY2(workflow.updateConstraintLines({"a", "b"}, {{"semantic", "hard_barrier"}, {"blockMode", "full_block"}}, &error), qPrintable(error));
    const auto edited = constraints.load("T1");
    QCOMPARE(layer->undoStack()->count(), 1);
    for (const auto &row : edited)
      QCOMPARE(row.value("type").toString(), QString("break_line"));
    QgsProcessingContext context;
    QgsProcessingFeatureSource source(layer, context, false);
    auto parsed = paleo::singlefactor::readConstraintLines(&source, layer->crs(), context);
    QCOMPARE(parsed.lines.size(), size_t(2));
    for (const auto &line : parsed.lines)
      QCOMPARE(line.semantic, paleo::singlefactor::Semantic::HardBarrier);
    layer->undoStack()->undo();
    QCOMPARE(constraints.load("T1"), original);
    layer->undoStack()->redo();
    QCOMPARE(constraints.load("T1"), edited);
    QVERIFY(project.writeProject()); // active-session saves use the durable source
    QgsProject saved;
    QVERIFY(saved.read(dir.filePath("p.qgz")));
    auto *savedLayer = qobject_cast<QgsVectorLayer *>(saved.mapLayer(layer->id()));
    QVERIFY(savedLayer && savedLayer->isValid());
    QCOMPARE(QFileInfo(savedLayer->source().section('|',0,0)).canonicalFilePath(), QFileInfo(gpkg).canonicalFilePath());
    QCOMPARE(savedLayer->featureCount(), 2);
    QVERIFY(QDir().mkpath(dir.filePath("moved")));
    QVERIFY(QFile::copy(dir.filePath("p.qgz"), dir.filePath("moved/p.qgz")));
    QVERIFY(QFile::copy(gpkg, dir.filePath("moved/project.gpkg")));
    QgsProject moved;
    QVERIFY(moved.read(dir.filePath("moved/p.qgz")));
    auto *movedLayer = qobject_cast<QgsVectorLayer *>(moved.mapLayer(layer->id()));
    QVERIFY(movedLayer && movedLayer->isValid());
    QCOMPARE(QFileInfo(movedLayer->source().section('|',0,0)).canonicalFilePath(), QFileInfo(dir.filePath("moved/project.gpkg")).canonicalFilePath());
    QVERIFY2(workflow.updateConstraintLines({"a"}, {{"semantic", "direction_guide"}, {"geometryAzimuth", 0.0}}, &error), qPrintable(error));
    const auto line = layer->getFeature(original.first().value("fid").toLongLong()).geometry().asPolyline();
    QVERIFY(std::abs(line.first().x() - line.last().x()) < 1e-8);
    QVERIFY(line.last().y() > line.first().y());
    const auto json = QJsonDocument::fromJson(constraints.load("T1").first().value("params_json").toString().toUtf8()).object();
    QCOMPARE(json.value("opaque").toInt(), 42);
    QVERIFY2(edits.commitEdit(layer, &error), qPrintable(error));
    QCOMPARE(layers.layer("constraints.T1"), layer);
  }
  void boundaryPolygonRoundTrip()
  {
    Fixture f;
    QVERIFY2(f.open(), qPrintable(f.error));
    QVERIFY(f.edits.rollbackEdit(f.layer.get()));
    QVERIFY(f.store.append("T1", "boundary", "POLYGON((0 0,20 0,20 20,0 20,0 0))", "polygon", 3, &f.error));
    f.layer->reload();
    QVERIFY2(f.edits.beginEdit(f.layer.get(), &f.error), qPrintable(f.error));
    const auto original = f.store.load("T1");
    const auto row = original.last();
    const auto fid = row.value("fid").toLongLong();
    auto geometry = f.layer->getFeature(fid).geometry();
    QVERIFY(geometry.moveVertex(24,20,2));
    f.layer->beginEditCommand("polygon vertex");
    QVERIFY(f.layer->changeGeometry(fid, geometry));
    f.layer->endEditCommand();
    QVERIFY(f.consistent());
    f.layer->undoStack()->undo();
    QCOMPARE(f.store.load("T1"), original);
    QVERIFY(f.consistent());
    f.layer->undoStack()->redo();
    QVERIFY(f.consistent());
    QVERIFY2(f.edits.commitEdit(f.layer.get(), &f.error), qPrintable(f.error));
    QVERIFY(f.consistent());
  }
  void batchRejectsNonLineWithoutPartialWrite()
  {
    QTemporaryDir dir;
    ConstraintStore store(dir.filePath("constraints.gpkg"));
    QString error;
    QVERIFY(store.appendExtended("T1", "a", "LINESTRING(0 0,10 0)", "direction_line", 1, "{\"semantic\":\"direction_guide\"}", 1, &error));
    QVERIFY(store.append("T1", "b", "POLYGON((0 0,10 0,10 10,0 0))", "polygon", 1, &error));
    ConstraintWorkflow workflow(nullptr, nullptr);
    workflow.setConstraintStore(&store);
    const auto original = store.load();
    QVERIFY(!workflow.updateConstraintLines({"a", "b"}, {{"semantic", "hard_barrier"}}, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(store.load(), original);
  }
  void deleteUndoAndCancelRestoresSnapshot()
  {
    Fixture f;
    QVERIFY2(f.open(), qPrintable(f.error));
    const auto original = f.store.load("T1");
    const auto fid = f.feature().id();
    f.layer->beginEditCommand("delete");
    QVERIFY(f.layer->deleteFeature(fid));
    f.layer->endEditCommand();
    QVERIFY(f.store.load("T1").isEmpty());
    QVERIFY(f.consistent());
    f.layer->undoStack()->undo();
    QCOMPARE(f.store.load("T1"), original);
    QVERIFY(f.consistent());
    QVERIFY(f.move(8));
    QVERIFY(f.edits.rollbackEdit(f.layer.get()));
    QCOMPARE(f.store.load("T1"), original);
    QVERIFY(f.consistent());
  }
  void failedWriteRevertsBuffer()
  {
    Fixture f;
    QVERIFY2(f.open(), qPrintable(f.error));
    const auto original = f.store.load("T1");
    QSignalSpy failed(&f.edits, &QgisEditingService::editFailed);
    f.project.setReadOnly(true);
    QVERIFY(f.move(5));
    QCOMPARE(failed.count(), 1);
    QCOMPARE(f.store.load("T1"), original);
    QVERIFY(f.consistent());
    f.project.setReadOnly(false);
    QVERIFY(f.edits.rollbackEdit(f.layer.get()));
  }
  void snapshotTransactionRejectsPartialAndCrossHorizon()
  {
    Fixture f;
    QVERIFY2(f.open(), qPrintable(f.error));
    const auto all = f.store.load();
    auto rows = f.store.load("T1");
    rows[0]["type"] = "break_line";
    rows.append(rows.first());
    QVERIFY(!f.store.replaceHorizon("T1", rows, &f.error));
    QCOMPARE(f.store.load(), all);
    rows = f.store.load("T2");
    QVERIFY(!f.store.replaceHorizon("T1", rows, &f.error));
    QCOMPARE(f.store.load(), all);
    QVERIFY(f.edits.rollbackEdit(f.layer.get()));
  }
  void captureEnterDoubleClickAndShapePreview()
  {
    QgsMapCanvas canvas;
    canvas.setDestinationCrs(QgsCoordinateReferenceSystem("EPSG:3857"));
    canvas.resize(400, 400);
    canvas.setExtent(QgsRectangle(0,0,100,100));
    canvas.refresh();
    const auto click = [&](QgsMapTool &tool, QPoint point) {
      QgsMapMouseEvent press(&canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
      QgsMapMouseEvent release(&canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton);
      tool.canvasPressEvent(&press); tool.canvasReleaseEvent(&release);
    };
    EditingLineCapture line(&canvas);
    canvas.setMapTool(&line);
    QSignalSpy lines(&line, &PaleoDrawConstraintTool::constraintDrawn);
    click(line, {50,200}); click(line, {200,100});
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    line.keyPressEvent(&enter);
    QCOMPARE(lines.count(), 1);
    canvas.unsetMapTool(&line);
    canvas.setMapTool(&line);
    click(line, {50,200}); click(line, {200,100});
    QgsMapMouseEvent doubleClick(&canvas, QEvent::MouseButtonDblClick, {200,100}, Qt::LeftButton);
    line.canvasDoubleClickEvent(&doubleClick);
    QCOMPARE(lines.count(), 2);
    canvas.unsetMapTool(&line);
    EditingPolygonCapture polygon(&canvas);
    canvas.setMapTool(&polygon);
    QSignalSpy polygons(&polygon, &PaleoDrawPolygonTool::constraintDrawn);
    click(polygon, {50,300}); click(polygon, {200,100}); click(polygon, {300,300});
    polygon.keyPressEvent(&enter);
    QCOMPARE(polygons.count(), 1);
    QVERIFY(QgsGeometry::fromWkt(polygons.first().first().toString()).isGeosValid());
    canvas.unsetMapTool(&polygon);
    EditingCircleCapture circle(&canvas);
    canvas.setMapTool(&circle);
    QSignalSpy circles(&circle, &PaleoDrawCircleTool::constraintDrawn);
    click(circle, {200,200});
    QgsMapMouseEvent move(&canvas, QEvent::MouseMove, {300,200});
    circle.canvasMoveEvent(&move);
    circle.keyPressEvent(&enter);
    QCOMPARE(circles.count(), 1);
    const auto circleGeometry = QgsGeometry::fromWkt(circles.first().first().toString());
    QCOMPARE(circleGeometry.constGet()->nCoordinates(), 25);
    QVERIFY(circleGeometry.isGeosValid());
    canvas.unsetMapTool(&circle);
  }
  void snapToleranceAndEditingExitDialog()
  {
    Fixture f;
    QVERIFY2(f.open(), qPrintable(f.error));
    QVERIFY(f.edits.rollbackEdit(f.layer.get()));
    const auto original = f.store.load("T1");
    QgsMapCanvas canvas;
    canvas.setLayers({f.layer.get()});
    canvas.setCurrentLayer(f.layer.get());
    canvas.setDestinationCrs(f.layer->crs());
    canvas.resize(400,300);
    canvas.setExtent(QgsRectangle(-5,-10,25,10));
    canvas.snappingUtils()->setConfig(QgisCanvasController::nativeSnappingConfig());
    canvas.refresh();
    PaleoEditingToolbar toolbar(&canvas);
    toolbar.setEditingService(&f.edits);
    toolbar.setLayers({f.layer.get()});
    toolbar.setCurrentLayer(f.layer.get());
    toolbar.snapToleranceSpin()->setValue(17);
    QCOMPARE(canvas.snappingUtils()->config().tolerance(), 17.0);
    QCOMPARE(canvas.snappingUtils()->config().units(), Qgis::MapToolUnit::Pixels);
    auto match = canvas.snappingUtils()->snapToMap(QgsPointXY(10,0));
    QVERIFY(match.isValid());
    QVERIFY(match.hasVertex());
    toolbar.actionEditMode()->trigger();
    QVERIFY(toolbar.isEditing());
    QVERIFY(f.move(5));
    QTimer::singleShot(0, [] {
      auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
      QVERIFY(dialog);
      for (auto *button : dialog->buttons())
        if (button->text() == QStringLiteral("放弃"))
        {
          button->click();
          return;
        }
      QFAIL("Discard button is missing");
    });
    toolbar.actionEditMode()->trigger();
    QVERIFY(!toolbar.isEditing());
    QCOMPARE(f.store.load("T1"), original);
    f.project.setReadOnly(true);
    QVERIFY(!toolbar.actionVertexEdit()->isEnabled());
    QVERIFY(!toolbar.startEditing());
    f.project.setReadOnly(false);
    QVERIFY(toolbar.actionVertexEdit()->isEnabled());
    f.layer->setReadOnly(true);
    QVERIFY(!toolbar.actionVertexEdit()->isEnabled());
  }
  void fullStackWriteFailureAndBranching()
  {
    Fixture f;
    f.edits.setUndoDepth(2);
    QVERIFY2(f.open(), qPrintable(f.error));
    QVERIFY(f.move(1)); QVERIFY(f.move(2));
    const auto previous = f.store.load("T1");
    f.project.setReadOnly(true);
    QVERIFY(f.move(3));
    QCOMPARE(f.store.load("T1"), previous);
    QVERIFY(f.consistent());
    f.project.setReadOnly(false);
    f.layer->undoStack()->redo();
    QVERIFY(f.consistent());
    f.layer->undoStack()->undo();
    QVERIFY(f.move(9));
    QVERIFY(!f.layer->undoStack()->canRedo());
    QVERIFY(f.consistent());
    QVERIFY(f.edits.rollbackEdit(f.layer.get()));
  }
  void nativeSnappingIncludesWellsBoundariesAndConstraints()
  {
    QgsVectorLayer wells("Point?crs=EPSG:3857", "wells", "memory");
    QgsVectorLayer boundaries("Polygon?crs=EPSG:3857", "boundaries", "memory");
    QgsVectorLayer constraints("LineString?crs=EPSG:3857", "constraints", "memory");
    const auto add = [](QgsVectorLayer &layer, const QString &wkt) {
      QgsFeature feature;
      feature.setGeometry(QgsGeometry::fromWkt(wkt));
      QgsFeatureList features{feature};
      return layer.dataProvider()->addFeatures(features);
    };
    QVERIFY(add(wells, "POINT(0 0)"));
    QVERIFY(add(boundaries, "POLYGON((30 0,40 0,40 10,30 10,30 0))"));
    QVERIFY(add(constraints, "LINESTRING(60 0,70 10)"));
    QgsMapCanvas canvas;
    canvas.resize(800,400);
    canvas.setDestinationCrs(wells.crs());
    canvas.setLayers({&wells, &boundaries, &constraints});
    canvas.setExtent(QgsRectangle(-10,-10,80,30));
    canvas.snappingUtils()->setConfig(QgisCanvasController::nativeSnappingConfig());
    canvas.refresh();
    for (const auto &target : {qMakePair(&wells, QgsPointXY(0,0)), qMakePair(&boundaries, QgsPointXY(30,0)), qMakePair(&constraints, QgsPointXY(60,0))})
    {
      const auto match = canvas.snappingUtils()->snapToMap(target.second);
      QVERIFY(match.isValid() && match.hasVertex());
      QCOMPARE(match.layer(), target.first);
    }
  }
  void highlightDoesNotOutliveCanvasScene()
  {
    Fixture f;
    QVERIFY2(f.open(), qPrintable(f.error));
    QVERIFY(f.edits.rollbackEdit(f.layer.get()));
    auto canvas = std::make_unique<QgsMapCanvas>();
    auto toolbar = std::make_unique<PaleoEditingToolbar>(canvas.get());
    toolbar->setEditingService(&f.edits);
    toolbar->setLayers({f.layer.get()});
    toolbar->setCurrentLayer(f.layer.get());
    f.layer->selectAll();
    QVERIFY(toolbar->startEditing());
    canvas.reset();
    toolbar.reset();
    QVERIFY(f.edits.rollbackEdit(f.layer.get()));
  }
  void unboundConstraintToolbarRefusesBufferOnlyEdit()
  {
    Fixture f;
    QVERIFY2(f.open(), qPrintable(f.error));
    QVERIFY(f.edits.rollbackEdit(f.layer.get()));
    QgsMapCanvas canvas;
    PaleoEditingToolbar toolbar(&canvas);
    toolbar.setLayers({f.layer.get()});
    toolbar.setCurrentLayer(f.layer.get());
    QVERIFY(!toolbar.actionVertexEdit()->isEnabled());
    QVERIFY(!toolbar.startEditing());
    QVERIFY(!f.layer->isEditable());
  }
  void removedLayerRestoresStoreAndMissingAssetRefuses()
  {
    Fixture f;
    QVERIFY2(f.open(), qPrintable(f.error));
    QVERIFY(f.edits.rollbackEdit(f.layer.get()));
    QVERIFY2(f.edits.beginEdit(f.layer.get(), &f.error), qPrintable(f.error));
    const auto original = f.store.load("T1");
    QVERIFY(f.move(5));
    f.layer.reset();
    QVERIFY(!f.project.layerBusy("constraints.T1"));
    QCOMPARE(f.store.load("T1"), original);
    // 「缺资产拒绝」：指向不存在路径的约束图层直接构造（无效图层），
    // beginEdit 必须拒绝。不用 rename 挪走真实 gpkg——Windows 上 GDAL/OGR
    // 连接池在图层析构后仍持有句柄，rename 被文件锁挡住（POSIX 只是碰巧允许）。
    f.layer = std::make_unique<QgsVectorLayer>(f.store.gpkgPath()+".missing|layername=constraints", "constraints", "ogr");
    f.layer->setSubsetString("horizon='T1'");
    f.layer->setCustomProperty("paleoLayerId", "constraints.T1");
    QVERIFY(!f.edits.beginEdit(f.layer.get(), &f.error));
    QVERIFY(!f.layer->isEditable());
  }
  void rejectedBranchCanRedoTheCorrectCommand()
  {
    Fixture f;
    QVERIFY2(f.open(), qPrintable(f.error));
    QVERIFY(f.move(1)); QVERIFY(f.move(2));
    f.layer->undoStack()->undo();
    const auto previous = f.store.load("T1");
    f.project.setReadOnly(true);
    QVERIFY(f.move(9));
    QCOMPARE(f.store.load("T1"), previous);
    QVERIFY(f.consistent());
    f.project.setReadOnly(false);
    f.layer->undoStack()->redo();
    QCOMPARE(f.feature().geometry().vertexAt(1).y(), 9.0);
    QVERIFY(f.consistent());
    QVERIFY(f.edits.rollbackEdit(f.layer.get()));
  }
  void boundedUndoCancelStillRestoresStart()
  {
    Fixture f;
    f.edits.setUndoDepth(2);
    QVERIFY2(f.open(), qPrintable(f.error));
    const auto original = f.store.load("T1");
    QVERIFY(f.move(1)); QVERIFY(f.move(2)); QVERIFY(f.move(3));
    QCOMPARE(f.layer->undoStack()->count(), 2);
    f.layer->undoStack()->undo();
    QVERIFY(f.consistent());
    f.layer->undoStack()->redo();
    QVERIFY(f.consistent());
    QVERIFY(f.edits.rollbackEdit(f.layer.get()));
    QCOMPARE(f.store.load("T1"), original);
  }
};
int main(int argc, char **argv)
{
  QTemporaryDir settings;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QgisRuntime::defaultPrefixPath()), true);
  app.initQgis();
  TestConstraintEditing test;
  const int result = QTest::qExec(&test, argc, argv);
  app.exitQgis();
  return result;
}
#include "tst_constraintediting.moc"
