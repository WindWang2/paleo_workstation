#include <QtTest>
#include <gdal.h>
#include <ogr_api.h>
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
#include <qgsmaprenderersequentialjob.h>
#include <qgsmapmouseevent.h>
#include <qgsprocessingcontext.h>
#include <qgsprocessingutils.h>
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/layervocabulary.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisstyleservice.h"
#include <qgsrulebasedrenderer.h>
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
#include "../src/qgis/qgisconstrainteditsession.h"

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
  void constraintAuditTracksNativeCompensationAndIgnoresSelection() {
    Fixture f; QVERIFY2(f.open(), qPrintable(f.error)); QVERIFY(f.edits.rollbackEdit(f.layer.get()));
    const auto original = f.store.load("T1"); const int initialEvents = f.store.history("c-1").size();
    QStringList failures;
    QgisConstraintEditSession session(f.layer.get(), &f.project,
        [&](const QString &reason) { failures << reason; },
        {"local:sedimentologist", "检查并纠正约束方向", "native-session", {}});
    QVERIFY2(session.initialize(&f.error), qPrintable(f.error));
    f.layer->selectAll(); f.layer->removeSelection();
    QCOMPARE(f.store.history("c-1").size(), initialEvents);
    QVERIFY(f.move(5)); QCOMPARE(f.store.history("c-1").size(), initialEvents + 1);
    auto event = f.store.history("c-1").first();
    QCOMPARE(event.value("actor_id").toString(), QString("local:sedimentologist"));
    QCOMPARE(event.value("action").toString(), QString("edit"));
    QCOMPARE(event.value("operation_id").toString(), QString("native-session"));
    f.layer->undoStack()->undo(); QCOMPARE(f.store.load("T1"), original);
    QCOMPARE(f.store.history("c-1").first().value("action").toString(), QString("undo"));
    f.layer->undoStack()->redo(); QVERIFY(f.consistent());
    QCOMPARE(f.store.history("c-1").first().value("action").toString(), QString("redo"));
    const auto count = f.store.history("c-1").size();
    auto geometry = f.feature().geometry(); QVERIFY(geometry.moveVertex(10, 9, 1));
    f.layer->beginEditCommand("cancelled gesture"); QVERIFY(f.layer->changeGeometry(f.feature().id(), geometry));
    f.layer->destroyEditCommand(); QVERIFY(f.consistent());
    QCOMPARE(f.store.history("c-1").size(), count);
    QVERIFY2(session.finish(false, &f.error), qPrintable(f.error));
    QCOMPARE(f.store.load("T1"), original);
    event = f.store.history("c-1").first(); QCOMPARE(event.value("action").toString(), QString("cancel"));
    QVERIFY(event.value("before").toMap().value("geometry_fingerprint") != event.value("after").toMap().value("geometry_fingerprint"));
    QVERIFY(failures.isEmpty());
    f.layer->reload(); QVERIFY(f.consistent());
  }
  void constraintAuditRejectionRestoresNativeStateWithoutEvents() {
    Fixture f; QVERIFY2(f.open(), qPrintable(f.error));
    const auto original = f.store.load("T1"); const auto events = f.store.history("c-1");
    f.project.setReadOnly(true); QVERIFY(f.move(8));
    QCOMPARE(f.store.load("T1"), original); QCOMPARE(f.store.history("c-1"), events); QVERIFY(f.consistent());
    f.project.setReadOnly(false); QVERIFY(f.edits.rollbackEdit(f.layer.get()));
    QCOMPARE(f.store.history("c-1"), events);
  }
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
  // #742：几何类型缓存按层位 + store 修订号失效——重复同步不再全量重解析
  // WKT；绘制/删除（含临时 ConstraintStore 实例写入）后必须重新扫描。
  void geometryTypeCacheInvalidatesOnDrawAndDelete()
  {
    QTemporaryDir dir;
    PaleoProjectStore store;
    const QString gpkg = dir.filePath("project.gpkg");
    store.setProjectPaths(dir.filePath("p.qgz"), gpkg, dir.filePath("m.sqlite"));
    ConstraintStore constraints(gpkg, &store);
    QString error;
    QVERIFY(constraints.appendExtended("T1", "a", "LINESTRING(0 0,10 0)", "direction_line", 1,
                                       "{\"semantic\":\"direction_guide\",\"ratio\":4}", 1, &error));
    QgisProjectService project;
    QVERIFY(project.createProject(dir.filePath("p.qgz")));
    LayerManifest manifest(dir.filePath("m.sqlite"));
    QVERIFY(manifest.open());
    QgisLayerService layers(&project, &manifest);
    ConstraintWorkflow workflow(nullptr, &layers);
    workflow.setConstraintStore(&constraints);
    workflow.loadConstraints("T1");
    const int afterFirst = workflow.constraintGeometryParseCount();
    QVERIFY(afterFirst >= 1);
    for (int i = 0; i < 5; ++i) {
      workflow.syncAllConstraintGeometryViews();
      workflow.loadConstraints("T1");
    }
    QCOMPARE(workflow.constraintGeometryParseCount(), afterFirst); // 未变更 → 零重解析

    QString drawnId;
    QVERIFY2(workflow.addConstraint("T1", "LINESTRING(0 5,10 5)", "break_line", 0, &error, &drawnId,
                                    {{"semantic", "hard_barrier"}, {"blockMode", "full_block"}}), qPrintable(error));
    workflow.syncAllConstraintGeometryViews();
    const int afterDraw = workflow.constraintGeometryParseCount();
    QVERIFY(afterDraw > afterFirst); // 绘制使缓存失效
    workflow.syncAllConstraintGeometryViews();
    QCOMPARE(workflow.constraintGeometryParseCount(), afterDraw);

    { // 另一个 store 实例写同一 gpkg 也必须失效（修订号按路径共享）
      ConstraintStore other(gpkg, &store);
      QVERIFY(other.remove(drawnId, &error));
    }
    workflow.syncAllConstraintGeometryViews();
    const int afterDelete = workflow.constraintGeometryParseCount();
    QVERIFY(afterDelete > afterDraw);
    QVERIFY2(workflow.removeConstraint("a", &error), qPrintable(error));
    workflow.syncAllConstraintGeometryViews();
    QVERIFY(workflow.constraintGeometryParseCount() > afterDelete);
    QVERIFY(workflow.loadConstraints("T1").isEmpty());
  }
  // P2：会话内绘制的新约束进入原生撤销栈；store fid 不撞其他层位；
  // undo/redo/取消/保存都与 store 一致；无语义裸要素仍被拒收并撤回。
  // PR457-closure F4：undo 释放预分配 fid 且被其他写入方占用后，redo 不再
  // 永久撞身份守卫——同身份让位新编号重放，两层位数据都完好。
  void redoSurvivesFidReuseByOtherWriter()
  {
    QTemporaryDir dir;
    PaleoProjectStore store;
    const QString gpkg = dir.filePath("project.gpkg");
    store.setProjectPaths(dir.filePath("p.qgz"), gpkg, dir.filePath("m.sqlite"));
    ConstraintStore constraints(gpkg, &store);
    QString error;
    QVERIFY(constraints.appendExtended("T1", "a", "LINESTRING(0 0,10 0)", "direction_line", 1,
                                       "{\"semantic\":\"direction_guide\",\"ratio\":4}", 1, &error));
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
    const auto original = constraints.load("T1");

    QString drawnId;
    QVERIFY2(workflow.addConstraint("T1", "LINESTRING(0 5,10 5)", "break_line", 0, &error, &drawnId,
                                    {{"semantic", "hard_barrier"}, {"blockMode", "full_block"}}),
             qPrintable(error));
    qlonglong drawnFid = 0;
    for (const auto &r : constraints.load("T1"))
      if (r.value("id").toString() == drawnId)
        drawnFid = r.value("fid").toLongLong();
    QVERIFY(drawnFid > 0);

    // undo 删除新绘行（释放预分配 fid）。GDAL 的自增序列不复用已删编号，
    // F4 的冲突场景是外部编辑/手写行——用显式 FID 插入占掉该编号。
    layer->undoStack()->undo();
    QCOMPARE(constraints.load("T1"), original);
    {
      GDALDatasetH ds = GDALOpenEx(gpkg.toUtf8().constData(), GDAL_OF_UPDATE | GDAL_OF_VECTOR,
                                   nullptr, nullptr, nullptr);
      QVERIFY(ds);
      OGRLayerH thiefLayer = GDALDatasetGetLayerByName(ds, "constraints");
      QVERIFY(thiefLayer);
      OGRFeatureH feature = OGR_F_Create(OGR_L_GetLayerDefn(thiefLayer));
      OGR_F_SetFID(feature, drawnFid); // 外部编辑者占走 undo 释放的编号
      OGR_F_SetFieldString(feature, OGR_F_GetFieldIndex(feature, "id"), "thief-external");
      OGR_F_SetFieldString(feature, OGR_F_GetFieldIndex(feature, "horizon"), "T2");
      OGR_F_SetFieldString(feature, OGR_F_GetFieldIndex(feature, "type"), "break_line");
      const char *wkt = "LINESTRING(40 40,50 40)";
      char *wktPtr = const_cast<char *>(wkt);
      OGRGeometryH geom = nullptr;
      QCOMPARE(OGR_G_CreateFromWkt(&wktPtr, nullptr, &geom), OGRERR_NONE);
      OGR_F_SetGeometry(feature, geom);
      OGR_G_DestroyGeometry(geom);
      QCOMPARE(OGR_L_CreateFeature(thiefLayer, feature), OGRERR_NONE);
      OGR_F_Destroy(feature);
      GDALClose(ds);
    }
    QVERIFY(!constraints.load("T2").isEmpty()); // 占用者在库

    // redo：同身份、新编号重放——不再拒绝，两层位数据都完好。
    layer->undoStack()->redo();
    const auto after = constraints.load("T1");
    QCOMPARE(after.size(), original.size() + 1);
    bool replayed = false;
    for (const auto &r : after)
      replayed |= r.value("id").toString() == drawnId &&
                  QgsGeometry::fromWkt(r.value("wkt").toString()).equals(
                      QgsGeometry::fromWkt(QStringLiteral("LINESTRING(0 5, 10 5)")));
    QVERIFY2(replayed, "redrawn constraint must be back with the same identity");
    QCOMPARE(constraints.load("T2").size(), 1); // 占用者不受影响
    QVERIFY(edits.rollbackEdit(layer));
    QCOMPARE(constraints.load("T1"), original);
  }

  void drawnConstraintJoinsUndoStack()
  {
    QTemporaryDir dir;
    PaleoProjectStore store;
    const QString gpkg = dir.filePath("project.gpkg");
    store.setProjectPaths(dir.filePath("p.qgz"), gpkg, dir.filePath("m.sqlite"));
    ConstraintStore constraints(gpkg, &store);
    QString error;
    QVERIFY(constraints.appendExtended("T1", "a", "LINESTRING(0 0,10 0)", "direction_line", 1,
                                       "{\"semantic\":\"direction_guide\",\"ratio\":4}", 1, &error));
    QVERIFY(constraints.append("T2", "z", "POINT(30 30)", "point", 9, &error));
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
    const auto original = constraints.load("T1");
    const auto otherHorizon = constraints.load("T2");

    // 会话期间往未编辑的层位直写一条（占掉当前最大 fid 之后的编号）。
    QString otherId;
    QVERIFY2(workflow.addConstraint("T2", "LINESTRING(40 40,50 40)", "break_line", 0, &error, &otherId,
                                    {{"semantic", "hard_barrier"}}),
             qPrintable(error));
    const auto otherHorizon2 = constraints.load("T2");
    QCOMPARE(otherHorizon2.size(), otherHorizon.size() + 1);
    QString drawnId;
    QVERIFY2(workflow.addConstraint("T1", "LINESTRING(0 5,10 5)", "break_line", 0, &error, &drawnId,
                                    {{"semantic", "hard_barrier"}, {"blockMode", "full_block"}}),
             qPrintable(error));
    QCOMPARE(layer->undoStack()->count(), 1);
    const auto drawn = constraints.load("T1");
    QCOMPARE(drawn.size(), original.size() + 1);
    QVariantMap row;
    for (const auto &r : drawn)
      if (r.value("id").toString() == drawnId)
        row = r;
    QVERIFY(!row.isEmpty());
    QCOMPARE(row.value("type").toString(), QString("break_line"));
    QVERIFY(row.value("params_json").toString().contains("full_block"));
    // 与 store append 口径一致：权重显式落 1.0（不是 NULL→读回默认值）。
    // 缓冲区里的新要素仍是负 fid，按身份找。
    QgsFeature buffered;
    for (auto it = layer->getFeatures(); it.nextFeature(buffered);)
      if (buffered.attribute("id").toString() == drawnId)
        break;
    QCOMPARE(buffered.attribute("id").toString(), drawnId);
    QVERIFY(!buffered.attribute("weight").isNull());
    QCOMPARE(buffered.attribute("weight").toDouble(), 1.0);
    for (const auto &other : otherHorizon2)
      QVERIFY(row.value("fid").toLongLong() != other.value("fid").toLongLong());
    QCOMPARE(constraints.load("T2"), otherHorizon2);

    layer->undoStack()->undo();
    QCOMPARE(constraints.load("T1"), original);
    layer->undoStack()->redo();
    QCOMPARE(constraints.load("T1"), drawn); // 同一 fid / 身份重放
    layer->undoStack()->undo();
    QCOMPARE(constraints.load("T1"), original);
    layer->undoStack()->redo();
    QCOMPARE(constraints.load("T1"), drawn);

    // 非线约束（旧 polygon 类型）在线类型图层的会话里同样进撤销栈并落 store。
    QString polygonId;
    QVERIFY2(workflow.addConstraint("T1", "POLYGON((20 20,30 20,30 30,20 20))", "polygon", 2, &error, &polygonId),
             qPrintable(error));
    bool polygonStored = false;
    for (const auto &r : constraints.load("T1"))
      polygonStored |= r.value("id").toString() == polygonId &&
                       QgsGeometry::fromWkt(r.value("wkt").toString()).type() == Qgis::GeometryType::Polygon;
    QVERIFY(polygonStored);
    layer->undoStack()->undo();
    QCOMPARE(constraints.load("T1"), drawn);

    // 工具栏裸画（无身份/语义）仍被拒收：store 不变，缓冲区撤回。
    QStringList failures;
    QgsFeature bare(layer->fields());
    bare.setAttribute(layer->fields().indexFromName("horizon"), "T1");
    bare.setGeometry(QgsGeometry::fromWkt("LINESTRING(0 9,10 9)"));
    const int before = layer->undoStack()->count();
    layer->beginEditCommand("bare");
    QVERIFY(layer->addFeature(bare));
    layer->endEditCommand();
    QCOMPARE(constraints.load("T1"), drawn);
    QVERIFY(layer->undoStack()->index() <= before);
    QCOMPARE(layer->featureCount(), drawn.size());

    // 取消会话 → 回到会话起点（含删除新绘约束）。
    QVERIFY(edits.rollbackEdit(layer));
    QCOMPARE(constraints.load("T1"), original);
    QCOMPARE(constraints.load("T2"), otherHorizon2);

    // 再开会话绘制并保存：落盘且重载后可见。
    QVERIFY2(edits.beginEdit(layer, &error), qPrintable(error));
    QVERIFY2(workflow.addConstraint("T1", "LINESTRING(0 7,10 7)", "break_line", 0, &error, &drawnId,
                                    {{"semantic", "hard_barrier"}, {"blockMode", "display_only"}}),
             qPrintable(error));
    QVERIFY2(edits.commitEdit(layer, &error), qPrintable(error));
    QCOMPARE(constraints.load("T1").size(), original.size() + 1);
    QCOMPARE(layer->featureCount(), original.size() + 1);
    QCOMPARE(constraints.load("T2"), otherHorizon2);
    Q_UNUSED(failures);
  }

  // P2 调查：同一层位混存点/线/面（旧 append 的 point/polygon 与打断线）。
  // 观测：OGR 按首个要素给出图层几何类型，但不丢要素——图层要素数 == store 行数；
  // 线解析器逐条把非线要素记为 not_line，不静默吞掉。
  void mixedPointLineHorizonProbe_data()
  {
    QTest::addColumn<bool>("pointFirst");
    QTest::newRow("line_first") << false;
    QTest::newRow("point_first") << true;
  }
  void mixedPointLineHorizonProbe()
  {
    QFETCH(bool, pointFirst);
    QTemporaryDir dir;
    ConstraintStore store(dir.filePath("c.gpkg"));
    QString error;
    if (pointFirst)
      QVERIFY(store.append("T1", "pt", "POINT(5 5)", "point", 3, &error));
    QVERIFY(store.appendExtended("T1", "line", "LINESTRING(0 0,10 0)", "break_line", 0,
                                 "{\"semantic\":\"hard_barrier\"}", 1, &error));
    if (!pointFirst)
      QVERIFY(store.append("T1", "pt", "POINT(5 5)", "point", 3, &error));
    QVERIFY(store.append("T1", "poly", "POLYGON((0 0,4 0,4 4,0 0))", "polygon", 3, &error));
    QgsVectorLayer layer(store.gpkgPath() + "|layername=constraints", "c", "ogr");
    layer.setSubsetString("horizon='T1'");
    QVERIFY(layer.isValid());
    qWarning() << "PROBE" << (pointFirst ? "point_first" : "line_first") << "wkb"
               << QgsWkbTypes::displayString(layer.wkbType());
    QCOMPARE(layer.featureCount(), static_cast<long long>(store.load("T1").size()));
    QgsProcessingContext context;
    QgsProcessingFeatureSource source(&layer, context, false);
    const auto parsed = paleo::singlefactor::readConstraintLines(&source, layer.crs(), context);
    QCOMPARE(parsed.lines.size(), size_t(1));
    QStringList ignored;
    for (const auto &item : parsed.ignored)
      ignored << QString::fromStdString(item);
    ignored.sort();
    QCOMPARE(ignored, QStringList({"poly not_line", "pt not_line"}));
  }

  // P4：相界编辑准备会 releaseHorizon 整组释放（对编辑中图层显式回滚，决定 b
  // 不改）。约束层有未保存的显式编辑时必须拒绝，且不留任何副作用。
  void composePrepareRefusesWhileConstraintSessionEditing()
  {
    QTemporaryDir dir;
    PaleoProjectStore store;
    const QString gpkg = dir.filePath("project.gpkg");
    store.setProjectPaths(dir.filePath("p.qgz"), gpkg, dir.filePath("m.sqlite"));
    ConstraintStore constraints(gpkg, &store);
    QgisProjectService project;
    QVERIFY(project.createProject(dir.filePath("p.qgz")));
    LayerManifest manifest(dir.filePath("m.sqlite"));
    QVERIFY(manifest.open());
    QgisLayerService layers(&project, &manifest);
    QgisEditingService edits(&store);
    layers.setEditingService(&edits);
    ConstraintWorkflow workflow(nullptr, &layers);
    workflow.setConstraintStore(&constraints);
    workflow.setEditingService(&edits);
    QString error;
    QVERIFY2(workflow.drawConstraint("T1", "LINESTRING(0 0,10 0)", "break_line", 0, &error, nullptr,
                                     {{"semantic", "hard_barrier"}}),
             qPrintable(error));
    auto *constraintLayer = qobject_cast<QgsVectorLayer *>(layers.layer("constraints.T1"));
    QVERIFY(constraintLayer);
    QVERIFY2(edits.beginEdit(constraintLayer, &error), qPrintable(error));
    QVERIFY2(workflow.drawConstraint("T1", "LINESTRING(0 1,10 1)", "break_line", 0, &error), qPrintable(error));
    QCOMPARE(constraintLayer->undoStack()->count(), 1);

    // T26 只读相界派生件（同层位）。
    const QString facies = dir.filePath("facies_ro.gpkg");
    QVERIFY(QFile::copy(gpkg, facies));
    QVERIFY(QFile::setPermissions(facies, QFileDevice::ReadOwner | QFileDevice::ReadUser));
    LayerDeclaration decl;
    decl.layerId = "facies.T1";
    decl.horizon = "T1";
    decl.type = "vector";
    decl.source = facies + "|layername=constraints";
    QVERIFY2(layers.declare(decl, &error), qPrintable(error));
    QVERIFY(layers.instantiate("facies.T1", &error));

    CompositionWorkflow compose(nullptr, &layers);
    compose.setCatalog(nullptr, dir.path());
    error.clear();
    QVERIFY(compose.prepareFaciesForEditing("facies.T1", &error).isEmpty());
    QVERIFY2(error.contains("正在编辑"), qPrintable(error));
    // 无副作用：会话、撤销栈、store、声明、工作副本均未动。
    QCOMPARE(layers.layer("constraints.T1"), static_cast<QgsMapLayer *>(constraintLayer));
    QVERIFY(constraintLayer->isEditable());
    QCOMPARE(constraintLayer->undoStack()->count(), 1);
    QCOMPARE(constraints.load("T1").size(), 2);
    QCOMPARE(layers.declaration("facies.T1").source, decl.source);
    QVERIFY(!QFileInfo::exists(dir.filePath("artifacts/layers/facies/facies.T1.gpkg")));

    // 保存后放行：绘制保留，工作副本重指。
    QVERIFY2(edits.commitEdit(constraintLayer, &error), qPrintable(error));
    error.clear();
    QCOMPARE(compose.prepareFaciesForEditing("facies.T1", &error), QString("facies.T1"));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(constraints.load("T1").size(), 2);
    QVERIFY(layers.declaration("facies.T1").source.startsWith(dir.filePath("artifacts/layers/facies/facies.T1.gpkg")));
    QFile::setPermissions(facies, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  }

  // P4：删除/语义改型可开会话时同走会话（可撤销）；会话保存/取消/删除后伴随视图重同步。
  void sessionRoutesRemoveUpdateAndResyncsViews()
  {
    QTemporaryDir dir;
    PaleoProjectStore store;
    const QString gpkg = dir.filePath("project.gpkg");
    store.setProjectPaths(dir.filePath("p.qgz"), gpkg, dir.filePath("m.sqlite"));
    ConstraintStore constraints(gpkg, &store);
    QgisProjectService project;
    QVERIFY(project.createProject(dir.filePath("p.qgz")));
    LayerManifest manifest(dir.filePath("m.sqlite"));
    QVERIFY(manifest.open());
    QgisLayerService layers(&project, &manifest);
    QgisEditingService edits(&store);
    layers.setEditingService(&edits);
    ConstraintWorkflow workflow(nullptr, &layers);
    workflow.setConstraintStore(&constraints);
    workflow.setEditingService(&edits);
    QString error;
    QVERIFY2(constraints.append("T1", "c-l", "LINESTRING(0 0,10 0)", "break_line", 0, &error), qPrintable(error));
    workflow.loadConstraints("T1");
    const QString polygonView = "constraintgeom.T1.polygon";
    QVERIFY(layers.declaration(polygonView).layerId.isEmpty());

    // 取消：会话内画面 → 视图声明；回滚后面消失 → 视图撤销。
    auto *layer = qobject_cast<QgsVectorLayer *>(layers.instantiate("constraints.T1", &error));
    QVERIFY(layer); QVERIFY2(edits.beginEdit(layer, &error), qPrintable(error));
    QVERIFY2(workflow.drawConstraint("T1", "POLYGON((20 20,30 20,30 30,20 20))", "polygon", 2, &error), qPrintable(error));
    QVERIFY(layer && layer->isEditable());
    QVERIFY(!layers.declaration(polygonView).layerId.isEmpty());
    QVERIFY(edits.rollbackEdit(layer));
    QCOMPARE(constraints.load("T1").size(), 1);
    QVERIFY(layers.declaration(polygonView).layerId.isEmpty());

    // 删除：无会话时自动开会话、进撤销栈；删后视图撤销，撤销后保存 → 视图回来。
    QVERIFY2(constraints.append("T1", "c-p", "POLYGON((20 20,30 20,30 30,20 20))", "polygon", 2, &error), qPrintable(error));
    workflow.loadConstraints("T1");
    QVERIFY(!layers.declaration(polygonView).layerId.isEmpty());
    QVERIFY(!layer->isEditable());
    QVERIFY2(workflow.removeConstraint("c-p", &error), qPrintable(error));
    QVERIFY(layer->isEditable());
    QCOMPARE(layer->undoStack()->count(), 1);
    QCOMPARE(constraints.load("T1").size(), 1);
    QVERIFY(layers.declaration(polygonView).layerId.isEmpty());
    layer->undoStack()->undo();
    QCOMPARE(constraints.load("T1").size(), 2);
    QVERIFY2(edits.commitEdit(layer, &error), qPrintable(error));
    QVERIFY(!layers.declaration(polygonView).layerId.isEmpty());

    // 语义改型：同样自动开会话、可撤销。
    const QString before = constraints.load("T1").first().value("params_json").toString();
    QVERIFY(!layer->isEditable());
    QVERIFY2(workflow.updateConstraintLines({"c-l"}, {{"semantic", "direction_guide"}}, &error), qPrintable(error));
    QVERIFY(layer->isEditable());
    QCOMPARE(layer->undoStack()->count(), 1);
    QString changed;
    for (const auto &row : constraints.load("T1"))
      if (row.value("id") == "c-l")
        changed = row.value("params_json").toString();
    QVERIFY2(changed.contains("direction_guide"), qPrintable(changed));
    layer->undoStack()->undo();
    for (const auto &row : constraints.load("T1"))
      if (row.value("id") == "c-l")
        QCOMPARE(row.value("params_json").toString(), before);
    QVERIFY(edits.rollbackEdit(layer));
    QVERIFY(!layer->isEditable());
  }

  // ---- PR457-closure 六：层位名的过滤转义与层位隔离 ----

  void hostileHorizonNamesRoundTrip()
  {
    // 单引号 / 中文 / 空格 / 反斜杠的层位名：subset 构造必须安全，
    // 图层有效且行数与 store 直读一致（转义不改变语义）。
    const QStringList horizons = { QStringLiteral("O'Connell-1"),
                                   QStringLiteral("S1 主层位"),
                                   QStringLiteral("D 61' 下段"),
                                   QStringLiteral("T\\1") };
    for ( const QString &h : horizons )
    {
      QTemporaryDir dir;
      PaleoProjectStore store;
      const QString gpkg = dir.filePath( "project.gpkg" );
      store.setProjectPaths( dir.filePath( "p.qgz" ), gpkg, dir.filePath( "m.sqlite" ) );
      ConstraintStore constraints( gpkg, &store );
      QgisProjectService project;
      QVERIFY( project.createProject( dir.filePath( "p.qgz" ) ) );
      LayerManifest manifest( dir.filePath( "m.sqlite" ) );
      QVERIFY( manifest.open() );
      QgisLayerService layers( &project, &manifest );
      QgisEditingService edits( &store );
      ConstraintWorkflow workflow( nullptr, &layers );
      workflow.setConstraintStore( &constraints );
      workflow.setEditingService( &edits );
      QString error;
      QString id;
      QVERIFY2( workflow.drawConstraint( h, "LINESTRING(0 0,10 0)", "break_line", 0, &error, &id,
                                        { { "semantic", "hard_barrier" } } ),
                qPrintable( h + ": " + error ) );
      QCOMPARE( constraints.load( h ).size(), 1 );
      auto *layer = qobject_cast<QgsVectorLayer *>( layers.layer( "constraints." + h ) );
      QVERIFY2( layer, qPrintable( h ) );
      QVERIFY2( layer->isValid(), qPrintable( h + " layer invalid" ) );
      QCOMPARE( layer->featureCount(), 1 ); // 只看到本层位那一行
      QCOMPARE( layer->subsetString(), QStringLiteral( "horizon='%1'" ).arg( QString( h ).replace( "'", "''" ) ) );
    }
  }

  void hostileHorizonCannotChangeFilterSemantics()
  {
    // 注入型层位名不得看到其它层位的行。
    const QString hostile = QStringLiteral( "T1' OR '1'='1" );
    QTemporaryDir dir;
    PaleoProjectStore store;
    const QString gpkg = dir.filePath( "project.gpkg" );
    store.setProjectPaths( dir.filePath( "p.qgz" ), gpkg, dir.filePath( "m.sqlite" ) );
    ConstraintStore constraints( gpkg, &store );
    QgisProjectService project;
    QVERIFY( project.createProject( dir.filePath( "p.qgz" ) ) );
    LayerManifest manifest( dir.filePath( "m.sqlite" ) );
    QVERIFY( manifest.open() );
    QgisLayerService layers( &project, &manifest );
    QgisEditingService edits( &store );
    ConstraintWorkflow workflow( nullptr, &layers );
    workflow.setConstraintStore( &constraints );
    workflow.setEditingService( &edits );
    QString error, id;
    QVERIFY2( workflow.drawConstraint( hostile, "LINESTRING(0 0,10 0)", "break_line", 0, &error, &id,
                                      { { "semantic", "hard_barrier" } } ),
              qPrintable( error ) );
    QVERIFY2( workflow.drawConstraint( QStringLiteral( "OTHER" ), "LINESTRING(0 5,10 5)", "break_line", 0, &error, &id,
                                      { { "semantic", "hard_barrier" } } ),
              qPrintable( error ) );
    auto *layer = qobject_cast<QgsVectorLayer *>( layers.layer( "constraints." + hostile ) );
    QVERIFY( layer );
    QVERIFY( layer->isValid() );
    // 关键断言：注入层位只看到自己的 1 行，不是全部 2 行。
    QCOMPARE( layer->featureCount(), 1 );
    QCOMPARE( static_cast<int>( constraints.load( hostile ).size() ), 1 );
  }

  void pipeHorizonIsRefusedNotTruncated()
  {
    // '|' 会截断 OGR URI：构造点必须拒绝而不是静默给出错误过滤的图层。
    const QString hostile = QStringLiteral( "A|B" );
    QTemporaryDir dir;
    PaleoProjectStore store;
    const QString gpkg = dir.filePath( "project.gpkg" );
    store.setProjectPaths( dir.filePath( "p.qgz" ), gpkg, dir.filePath( "m.sqlite" ) );
    ConstraintStore constraints( gpkg, &store );
    QgisProjectService project;
    QVERIFY( project.createProject( dir.filePath( "p.qgz" ) ) );
    LayerManifest manifest( dir.filePath( "m.sqlite" ) );
    QVERIFY( manifest.open() );
    QgisLayerService layers( &project, &manifest );
    ConstraintWorkflow workflow( nullptr, &layers );
    workflow.setConstraintStore( &constraints );
    QString error, id;
    QVERIFY( !workflow.drawConstraint( hostile, "LINESTRING(0 0,10 0)", "break_line", 0, &error, &id,
                                       { { "semantic", "hard_barrier" } } ) );
    QVERIFY2( error.contains( QLatin1Char( '|' ) ), qPrintable( error ) );
    // 拒绝不留半截：库里没有这行。
    QVERIFY( constraints.load( hostile ).isEmpty() );
    QVERIFY( constraints.load().isEmpty() );
    QVERIFY( !layers.layer( "constraints." + hostile ) );
  }

  void completedDrawingIsPersistentVector_data()
  {
    QTest::addColumn<int>("mode");
    QTest::newRow("empty-store") << 0;
    QTest::newRow("legacy-empty-table") << 1;
    QTest::newRow("another-horizon-point") << 2;
  }
  void completedDrawingIsPersistentVector()
  {
    QFETCH(int, mode);
    QTemporaryDir dir;
    PaleoProjectStore store;
    const QString gpkg = dir.filePath("project.gpkg");
    store.setProjectPaths(dir.filePath("p.qgz"), gpkg, dir.filePath("m.sqlite"));
    ConstraintStore constraints(gpkg, &store);
    QgisProjectService project;
    QVERIFY(project.createProject(dir.filePath("p.qgz")));
    LayerManifest manifest(dir.filePath("m.sqlite")); QVERIFY(manifest.open());
    QgisLayerService layers(&project, &manifest);
    QgisEditingService edits(&store);
    ConstraintWorkflow workflow(nullptr, &layers);
    workflow.setConstraintStore(&constraints); workflow.setEditingService(&edits);
    QString error;
    QgsVectorLayer *existing = nullptr;
    if (mode == 1) {
      QVERIFY(constraints.ensureSchema(&error));
      LayerDeclaration d; d.layerId = "constraints.D61"; d.horizon = "D61";
      d.type = "vector"; d.source = gpkg + "|layername=constraints|subset=horizon='D61'";
      QVERIFY(layers.declare(d, &error));
      existing = qobject_cast<QgsVectorLayer *>(layers.instantiate(d.layerId, &error));
      QVERIFY(existing);
    }
    if (mode == 2)
      QVERIFY(constraints.append("OTHER", "c-1", "POINT(50 50)", "point", 0, &error));
    QString id;
    QVERIFY2(workflow.drawConstraint("D61", "LINESTRING(0 0,10 5)", "break_line", 0, &error, &id,
                                     {{"semantic", "hard_barrier"}}), qPrintable(error));
    auto *layer = qobject_cast<QgsVectorLayer *>(layers.layer("constraints.D61"));
    QVERIFY(layer); QVERIFY(!layer->isEditable());
    if (existing) QCOMPARE(layer, existing); // 图层树和可见性保留在同一对象上。
    QCOMPARE(layer->geometryType(), Qgis::GeometryType::Line);
    QCOMPARE(layer->featureCount(), 1);
    QCOMPARE(constraints.load("D61").size(), 1);
    const auto assertRendered = [&](QgsVectorLayer *vector) {
      QgsMapSettings settings; settings.setLayers({vector});
      settings.setDestinationCrs(vector->crs()); settings.setExtent(QgsRectangle(-2, -2, 12, 7));
      settings.setOutputSize(QSize(400, 300)); settings.setBackgroundColor(Qt::white);
      QgsMapRendererSequentialJob render(settings); render.start(); render.waitForFinished();
      const auto image = render.renderedImage();
      int ink = 0;
      for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
          ink += image.pixelColor(x, y) != QColor(Qt::white);
      return ink > 50;
    };
    QVERIFY(assertRendered(layer)); // 捕获橡皮筋已不存在，仍由矢量图层渲染。
    QVERIFY2(workflow.drawConstraint("D61", "LINESTRING(0 1,10 6)", "direction_line", 0, &error), qPrintable(error));
    QCOMPARE(layers.layer("constraints.D61"), static_cast<QgsMapLayer *>(layer));
    QCOMPARE(layer->featureCount(), 2); QVERIFY(!layer->isEditable());

    // 用户主动编辑仍有正常的撤销/重做和放弃；成图前可直接保存。
    QVERIFY2(edits.beginEdit(layer, &error), qPrintable(error));
    QCOMPARE(layer->geometryType(), Qgis::GeometryType::Line);
    QVERIFY2(workflow.drawConstraint("D61", "LINESTRING(0 2,10 7)", "break_line", 0, &error), qPrintable(error));
    QCOMPARE(layer->undoStack()->count(), 1);
    QCOMPARE(constraints.load("D61").size(), 3);
    layer->undoStack()->undo(); QCOMPARE(constraints.load("D61").size(), 2);
    layer->undoStack()->redo(); QCOMPARE(constraints.load("D61").size(), 3);
    QVERIFY2(workflow.saveConstraintEdits("D61", &error), qPrintable(error));
    QVERIFY(!layer->isEditable()); QCOMPARE(layer->geometryType(), Qgis::GeometryType::Line);
    QCOMPARE(layer->featureCount(), 3); QVERIFY(assertRendered(layer));
    QVERIFY2(edits.beginEdit(layer, &error), qPrintable(error));
    QVERIFY2(workflow.drawConstraint("D61", "LINESTRING(0 3,10 8)", "break_line", 0, &error), qPrintable(error));
    QVERIFY(edits.rollbackEdit(layer)); QCOMPARE(constraints.load("D61").size(), 3);
    QVERIFY(project.writeProject());
    QgsProject reopened; QVERIFY(reopened.read(dir.filePath("p.qgz")));
    auto *saved = qobject_cast<QgsVectorLayer *>(reopened.mapLayer(layer->id()));
    QVERIFY(saved); QCOMPARE(saved->geometryType(), Qgis::GeometryType::Line);
    QCOMPARE(saved->featureCount(), 3); QVERIFY(assertRendered(saved));
    QCOMPARE(constraints.load("OTHER").size(), mode == 2 ? 1 : 0);
  }

  // 另一层位已有线时，在空层位上画间隔线并保存。保存前是线，保存后仍是线，
  // 不能退回无几何的属性表。
  void saveKeepsIntervalLineGeometry()
  {
    QTemporaryDir dir;
    PaleoProjectStore store;
    const QString gpkg = dir.filePath("project.gpkg");
    store.setProjectPaths(dir.filePath("p.qgz"), gpkg, dir.filePath("m.sqlite"));
    ConstraintStore constraints(gpkg, &store);
    QString error;
    QVERIFY2(constraints.appendExtended("D53", "c-1", "LINESTRING(0 0,10 0)", "break_line", 0,
                                        "{\"semantic\":\"hard_barrier\"}", 1, &error),
             qPrintable(error));
    QgisProjectService project;
    QVERIFY(project.createProject(dir.filePath("p.qgz")));
    LayerManifest manifest(dir.filePath("m.sqlite"));
    QVERIFY(manifest.open());
    QgisLayerService layers(&project, &manifest);
    QgisEditingService edits(&store);
    ConstraintWorkflow workflow(nullptr, &layers);
    workflow.setConstraintStore(&constraints);
    workflow.setEditingService(&edits);
    workflow.loadConstraints("D62");
    QVERIFY2(workflow.drawConstraint("D62", "LINESTRING(1 1,8 4)", "break_line", 0, &error, nullptr,
                                     {{"semantic", "hard_barrier"}}),
             qPrintable(error));
    auto *layer = qobject_cast<QgsVectorLayer *>(layers.layer("constraints.D62"));
    QVERIFY(layer);
    QCOMPARE(layer->geometryType(), Qgis::GeometryType::Line);
    QgisStyleService::applyConstraintLayerStyle(layer);
    auto *drawnStyle = dynamic_cast<QgsRuleBasedRenderer *>(layer->renderer());
    QVERIFY(drawnStyle);
    QCOMPARE(drawnStyle->rootRule()->children().size(), 2);
    QCOMPARE(drawnStyle->rootRule()->children().at(0)->label(), QStringLiteral("方向线"));
    QCOMPARE(drawnStyle->rootRule()->children().at(1)->label(), QStringLiteral("打断线"));
    QVERIFY2(edits.beginEdit(layer, &error), qPrintable(error));
    QVERIFY2(edits.commitEdit(layer, &error), qPrintable(error));
    QVERIFY2(layer->geometryType() == Qgis::GeometryType::Line,
             qPrintable(QStringLiteral("after save wkb=%1 type=%2 features=%3 source=%4 subset=%5")
                            .arg(QgsWkbTypes::displayString(layer->wkbType()))
                            .arg(int(layer->geometryType()))
                            .arg(layer->featureCount())
                            .arg(layer->source(), layer->subsetString())));
    QCOMPARE(layer->featureCount(), 1);
    auto *savedStyle = dynamic_cast<QgsRuleBasedRenderer *>(layer->renderer());
    QVERIFY(savedStyle);
    QCOMPARE(savedStyle->rootRule()->children().size(), 2);
    const auto saved = constraints.load("D62");
    QCOMPARE(saved.size(), 1);
    QCOMPARE(saved.first().value(QStringLiteral("id")).toString(), QStringLiteral("c-2"));
    QCOMPARE(constraints.load("D53").size(), 1);
  }

  // P3：混合几何层位按几何类型声明只读伴随视图（不迁移 GPKG、不改主图层）。
  // 主图层类型随首要素；伴随视图补齐主图层不覆盖的类型，多部件同类计入，且只读。
  void mixedGeometryViewsPerType_data() { mixedPointLineHorizonProbe_data(); }
  void mixedGeometryViewsPerType()
  {
    QFETCH(bool, pointFirst);
    QTemporaryDir dir;
    PaleoProjectStore store;
    const QString gpkg = dir.filePath("project.gpkg");
    store.setProjectPaths(dir.filePath("p.qgz"), gpkg, dir.filePath("m.sqlite"));
    ConstraintStore constraints(gpkg, &store);
    QString error;
    if (pointFirst)
      QVERIFY(constraints.append("T1", "pt", "POINT(5 5)", "point", 3, &error));
    QVERIFY(constraints.appendExtended("T1", "line", "LINESTRING(0 0,10 0)", "break_line", 0,
                                       "{\"semantic\":\"hard_barrier\"}", 1, &error));
    QVERIFY(constraints.appendExtended("T1", "mline", "MULTILINESTRING((0 2,10 2),(0 3,10 3))", "break_line", 0,
                                       "{\"semantic\":\"hard_barrier\"}", 1, &error));
    if (!pointFirst)
      QVERIFY(constraints.append("T1", "pt", "POINT(5 5)", "point", 3, &error));
    QVERIFY(constraints.append("T2", "other", "POINT(9 9)", "point", 3, &error)); // 其他层位不计入
    QgisProjectService project;
    QVERIFY(project.createProject(dir.filePath("p.qgz")));
    LayerManifest manifest(dir.filePath("m.sqlite"));
    QVERIFY(manifest.open());
    QgisLayerService layers(&project, &manifest);
    ConstraintWorkflow workflow(nullptr, &layers);
    workflow.setConstraintStore(&constraints);
    workflow.loadConstraints("T1");
    auto *main = qobject_cast<QgsVectorLayer *>(layers.instantiate("constraints.T1", &error));
    QVERIFY2(main, qPrintable(error));
    QCOMPARE(main->featureCount(), 3LL); // 主图层不过滤：覆盖层位全部行（编辑会话前提）
    const QString missing = pointFirst ? "constraintgeom.T1.line" : "constraintgeom.T1.point";
    const QString covered = pointFirst ? "constraintgeom.T1.point" : "constraintgeom.T1.line";
    QCOMPARE(main->geometryType(), pointFirst ? Qgis::GeometryType::Point : Qgis::GeometryType::Line);
    QVERIFY(layers.declaration(covered).layerId.isEmpty());
    QVERIFY(layers.declaration("constraintgeom.T1.polygon").layerId.isEmpty());
    auto *view = qobject_cast<QgsVectorLayer *>(layers.instantiate(missing, &error));
    QVERIFY2(view, qPrintable(error));
    QVERIFY(view->readOnly());
    QCOMPARE(view->geometryType(), pointFirst ? Qgis::GeometryType::Line : Qgis::GeometryType::Point);
    QCOMPARE(view->featureCount(), pointFirst ? 2LL : 1LL);
    QCOMPARE(layers.declaration(missing).group, PaleoLayerVocabulary::kConstraintsGroup);
    QgisEditingService edits(&store);
    QVERIFY(!edits.beginEdit(view, &error)); // 伴随视图不可编辑：编辑走主图层会话
    // 新绘面约束 → 声明面视图。
    QVERIFY2(workflow.addConstraint("T1", "POLYGON((20 20,30 20,30 30,20 20))", "polygon", 2, &error), qPrintable(error));
    QVERIFY(!layers.declaration("constraintgeom.T1.polygon").layerId.isEmpty());
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
  // P1（pr457-closure）：FID 冲突安全失败——快照事务绝不静默覆盖其他要素。
  // 三种撞法都必须整批拒绝且原行原样：fid 已被同层位其他身份占用、fid 跨层位
  // 占用（外部写者）、稳定身份已被其他 fid 使用。
  void snapshotRefusesFidAndIdentityCollisions()
  {
    Fixture f;
    QVERIFY2(f.open(), qPrintable(f.error));
    const auto all = f.store.load();
    const qlonglong t1Fid = f.store.load("T1").first().value("fid").toLongLong();
    const qlonglong t2Fid = f.store.load("T2").first().value("fid").toLongLong();

    // (a) 同层位：fid 已存在但身份不同（覆盖冒充）。
    {
      auto rows = f.store.load("T1");
      QVariantMap intruder = rows.first();
      intruder.insert("id", "c-imposter");
      intruder.insert("fid", t1Fid);
      intruder.insert("wkt", "LINESTRING(99 99, 100 100)");
      rows.append(intruder);
      QVERIFY(!f.store.replaceHorizon("T1", rows, &f.error));
      QVERIFY(!f.error.isEmpty());
      QCOMPARE(f.store.load(), all);
    }
    // (b) 跨层位：fid 属于 T2 的行（外部/其他会话写者占号）。
    {
      auto rows = f.store.load("T1");
      QVariantMap foreign = rows.first();
      foreign.insert("id", "c-foreign");
      foreign.insert("fid", t2Fid);
      foreign.insert("wkt", "LINESTRING(98 98, 99 99)");
      rows.append(foreign);
      QVERIFY(!f.store.replaceHorizon("T1", rows, &f.error));
      QCOMPARE(f.store.load(), all);
    }
    // (c) 新 fid 但稳定身份已被别的行使用。
    {
      auto rows = f.store.load("T1");
      QVariantMap stolen = rows.first();
      stolen.insert("id", rows.first().value("id")); // 同一 id，不同 fid
      stolen.insert("fid", t2Fid + 1000);
      stolen.insert("wkt", "LINESTRING(97 97, 98 98)");
      rows.append(stolen);
      QVERIFY(!f.store.replaceHorizon("T1", rows, &f.error));
      QCOMPARE(f.store.load(), all);
    }
    // 会话缓冲区与 store 仍一致（没有任何半提交痕迹）。
    QVERIFY(f.consistent());
    QVERIFY(f.edits.rollbackEdit(f.layer.get()));
    QCOMPARE(f.store.load(), all);
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
  void removedLayerPreservesEditsAndMissingAssetRefuses()
  {
    Fixture f;
    QVERIFY2(f.open(), qPrintable(f.error));
    QVERIFY(f.edits.rollbackEdit(f.layer.get()));
    QVERIFY2(f.edits.beginEdit(f.layer.get(), &f.error), qPrintable(f.error));
    const auto original = f.store.load("T1");
    QVERIFY(f.move(5));
    const auto edited = f.store.load("T1");
    QVERIFY(edited != original);
    QSignalSpy committed(&f.edits, &QgisEditingService::editCommitted);
    QSignalSpy rolledBack(&f.edits, &QgisEditingService::editRolledBack);
    f.layer.reset();
    QVERIFY(!f.project.layerBusy("constraints.T1"));
    // B7（用户决定）：移除图层保留未保存的编辑，不回到会话起点。
    QCOMPARE(f.store.load("T1"), edited);
    QCOMPARE(committed.count(), 1);
    QCOMPARE(rolledBack.count(), 0);
    // 「缺资产拒绝」：指向不存在路径的约束图层直接构造（无效图层），
    // beginEdit 必须拒绝。不用 rename 挪走真实 gpkg——Windows 上 GDAL/OGR
    // 连接池在图层析构后仍持有句柄，rename 被文件锁挡住（POSIX 只是碰巧允许）。
    f.layer = std::make_unique<QgsVectorLayer>(f.store.gpkgPath()+".missing|layername=constraints", "constraints", "ogr");
    f.layer->setSubsetString("horizon='T1'");
    f.layer->setCustomProperty("paleoLayerId", "constraints.T1");
    QVERIFY(!f.edits.beginEdit(f.layer.get(), &f.error));
    QVERIFY(!f.layer->isEditable());
  }
  // B7：撤销后再移除，保留的是撤销后的状态（store 跟随原生撤销栈的当前位置）；
  // 新绘约束同样保留；重开图层看到同一份数据，且能重新进入编辑。
  void removedLayerPreservesUndoPositionAndDrawnConstraint()
  {
    Fixture f;
    QVERIFY2(f.open(), qPrintable(f.error));
    const auto original = f.store.load("T1");
    const auto other = f.store.load("T2");
    QVERIFY(f.move(5));
    const auto first = f.store.load("T1");
    QVERIFY(f.move(9));
    f.layer->undoStack()->undo();
    QCOMPARE(f.store.load("T1"), first);
    QgsFeature drawn(f.layer->fields());
    drawn.setAttribute(f.layer->fields().indexFromName("id"), "drawn-1");
    drawn.setAttribute(f.layer->fields().indexFromName("horizon"), "T1");
    drawn.setAttribute(f.layer->fields().indexFromName("type"), "break_line");
    drawn.setAttribute(f.layer->fields().indexFromName("params_json"), "{\"semantic\":\"hard_barrier\"}");
    drawn.setAttribute(f.layer->fields().indexFromName("schema_version"), 1);
    drawn.setAttribute(f.layer->fields().indexFromName("weight"), 1.0);
    drawn.setGeometry(QgsGeometry::fromWkt("LINESTRING(0 3,10 3)"));
    f.layer->beginEditCommand("draw");
    QVERIFY(f.layer->addFeature(drawn));
    f.layer->endEditCommand();
    const auto beforeRemoval = f.store.load("T1");
    QCOMPARE(beforeRemoval.size(), original.size() + 1);
    f.layer.reset();
    QCOMPARE(f.store.load("T1"), beforeRemoval);
    QCOMPARE(f.store.load("T2"), other);
    QVERIFY(!f.project.layerBusy("constraints.T1"));
    f.layer = std::make_unique<QgsVectorLayer>(f.store.gpkgPath()+"|layername=constraints", "constraints", "ogr");
    f.layer->setSubsetString("horizon='T1'");
    f.layer->setCustomProperty("paleoLayerId", "constraints.T1");
    QVERIFY(f.layer->isValid());
    QVERIFY(f.consistent());
    QVERIFY2(f.edits.beginEdit(f.layer.get(), &f.error), qPrintable(f.error));
    QVERIFY(f.edits.rollbackEdit(f.layer.get()));
    QCOMPARE(f.store.load("T1"), beforeRemoval);
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
