#include "../src/ai/remotepredictconfig.h"
#include "../src/app/aiwiring.h"
#include "../src/domain/faciescatalog.h"
#include "../src/domain/facieshierarchy.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/facieshierarchyrenderer.h"
#include "../src/qgis/mapcanvaslink.h"
#include "../src/qgis/mappingartifactwriter.h"
#include "../src/qgis/mappingfeatureidentity.h"
#include "../src/qgis/qgiscanvascontroller.h"
#include "../src/linkage/selectioncontext.h"
#include "../src/domain/interpretationevidence.h"
#include "../src/workflow/evidenceinteraction.h"
#include "../src/services/datagraph.h"
#include "../src/qgis/nativeattributetablehost.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/wellattributestore.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/services/paleotaskservice.h"
#include "../src/ui/edittools/editingtoolbar.h"
#include "../src/ui/edittools/vertexeditortools.h"
#include "../src/ui/pages/mappingworkbenchpage.h"
#include "../src/ui/pages/wellpredictionpanel.h"
#include "../src/ui/paleotheme.h"
#include "../src/ui/wellcomposite/wellcompositecanvas.h"
#include "../src/io/constraintstore.h"
#include "../src/workflow/derivedassets.h"
#include "../src/workflow/mappingworkbench.h"
#include "../src/workflow/wellpredictionservice.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <cmath>
#include <qgsrasterdataprovider.h>
#include <qgseditorwidgetsetup.h>
#include "../src/workflow/workflows.h"
#include "mockremotepredictionservice.h"
#include <QAction>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QHash>
#include <QFile>
#include <QGraphicsScene>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QThread>
#include <QUndoStack>
#include <QtTest>
#include <atomic>
#include <gdal.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>
#include <qgsapplication.h>
#include <qgscategorizedsymbolrenderer.h>
#include <qgsexpressioncontextutils.h>
#include <qgsfillsymbollayer.h>
#include <qgsmarkersymbol.h>
#include <qgsmarkersymbollayer.h>
#include <QSvgRenderer>
#include <QPainter>
#include <qgsgeometry.h>
#include <qgsmapcanvas.h>
#include <qgsrendercontext.h>
#include <qgsrenderer.h>
#include <qgssymbol.h>
#include <qgsmaprenderersequentialjob.h>
#include <qgsmapsettings.h>
#include <qgsmergedfeaturerenderer.h>
#include <qgslabelingresults.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsrasterrenderer.h>
#include <qgsrubberband.h>
#include <qgsvectordataprovider.h>
#include <qgsvectorlayer.h>
#include <qgsvectorlayerlabeling.h>

class DelayedPredictionService : public RemotePredictionService {
public:
  RemotePredictionRequest request;
  void start(const RemotePredictionRequest &r) override { request = r; }
  void cancel() override {}
  void deliver() {
    RemotePredictionResult result;
    result.request = request;
    result.method = "test";
    result.points = {QVariantMap{
        {"id", "wrong"}, {"x", 100}, {"y", 100}, {"facies_code", 1}}};
    emit completed(result);
  }
};

// 测井预测的定制应答：整井优势相与 D61 地层内优势相刻意不同——
// [top, top+10) 相 1、[top+10, top+60) 相 2、[top+60, bottom) 相 1。
// 整井：相 1 厚 70 > 相 2 厚 50；D61（1000–1060）：相 2 厚 50 > 相 1 厚 10。
class CraftedWellService : public RemotePredictionService {
public:
  RemotePredictionRequest request;
  void start(const RemotePredictionRequest &r) override { request = r; }
  void cancel() override {}
  void deliver() {
    RemotePredictionResult result;
    result.request = request;
    result.method = "crafted";
    for (const auto &value : request.wells) {
      auto point = value.toMap();
      const double top = point.value("depth_top").toDouble();
      const double bottom = point.value("depth_bottom").toDouble();
      point.insert("facies_code", 1);
      point.insert("facies_intervals",
                   QString::fromUtf8(QJsonDocument::fromVariant(QVariantList{
                       QVariantMap{{"top", top}, {"bottom", top + 10}, {"code", 1}},
                       QVariantMap{{"top", top + 10}, {"bottom", top + 60}, {"code", 2}},
                       QVariantMap{{"top", top + 60}, {"bottom", bottom}, {"code", 1}}})
                                         .toJson(QJsonDocument::Compact)));
      result.points << point;
    }
    emit completed(result);
  }
};

// opt-04：真实 RemotePredictionRouter 的 cancel() 会同步 emit failed(id,"已取消")。
// 该替身镜像这一行为，用来钉住「用户主动取消不得被翻译成 errorOccurred」。
class AbortingOnCancelService : public RemotePredictionService {
public:
  RemotePredictionRequest request;
  void start(const RemotePredictionRequest &r) override { request = r; }
  void cancel() override { emit failed(request.id, QStringLiteral("已取消")); }
  // 反向守卫用：模拟真实远端失败（非取消），必须照常上报。
  void failNow(const QString &reason) { emit failed(request.id, reason); }
};
// opt-04：远端返回 200 但网格全 null（→ 全 -9999）的替身。
class AllNodataSeismicService : public RemotePredictionService {
public:
  RemotePredictionRequest request;
  void start(const RemotePredictionRequest &r) override {
    request = r;
    RemotePredictionResult result;
    result.request = request;
    result.mock = false;
    result.method = QStringLiteral("fake-remote-all-nodata");
    result.cells = QVector<int>(request.rows * request.columns, -9999);
    QTimer::singleShot(0, this, [this, result]() { emit completed(result); });
  }
  void cancel() override {}
};

// opt-04：远端 200 + completed，但返回的是空网格（格数与请求不符）。
// 这既不是「全无数据」也不是成功——原因必须报到「与请求不一致」上，
// 不能被更具体的「全部为无数据」判据抢先给出误导性说明。
class EmptyGridSeismicService : public RemotePredictionService {
public:
  RemotePredictionRequest request;
  void start(const RemotePredictionRequest &r) override {
    request = r;
    RemotePredictionResult result;
    result.request = request;
    result.mock = false;
    result.method = QStringLiteral("fake-remote-empty-grid");
    result.cells = {}; // 0 格，而请求是 rows*columns
    QTimer::singleShot(0, this, [this, result]() { emit completed(result); });
  }
  void cancel() override {}
};

// Local API 2.0 contract server: exercises production transport, real curve
// preparation and work area persistence without uploading project data.
class WellApiServer {
public:
  QTcpServer server;
  QList<QJsonObject> bodies;
  int modelRequests = 0;
  bool hold = false, failPredict = false, queued = false, holdPoll = false;
  // opt-04 E2E 缺口：远端 200 但无可用模型 / 无输出（predictions 为空）。
  bool emptyModels = false, emptyPredictions = false;
  QJsonObject pending;
  int polls = 0;
  WellApiServer() {
    server.listen(QHostAddress::LocalHost);
    QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
      while (server.hasPendingConnections()) {
        auto *socket = server.nextPendingConnection();
        auto buffer = std::make_shared<QByteArray>();
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
          *buffer += socket->readAll();
          const auto end = buffer->indexOf("\r\n\r\n");
          if (end < 0) return;
          int length = 0;
          for (const auto &line : buffer->left(end).split('\n'))
            if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
          if (buffer->size() < end + 4 + length) return;
          const auto path = buffer->split(' ').value(1);
          QJsonObject response;
          int status = 200;
          if (path.endsWith("/models")) {
            ++modelRequests;
            if (emptyModels)
              response = {{"models", QJsonArray{}}};
            else
              response = {{"models", QJsonArray{QJsonObject{
                {"id", "m1"}, {"name", "test-model"}, {"version", "v2"},
                {"inputSchema", QJsonObject{{"formationGroup", ""}, {"curves", QJsonArray{"GR"}},
                  {"categoricalFields", QJsonArray{QStringLiteral("段"), QStringLiteral("岩性")}}, {"window", 1}}}}}}};
          } else if (path.endsWith("/predict")) {
            const auto body = QJsonDocument::fromJson(buffer->mid(end + 4, length)).object();
            bodies << body;
            if (hold) { buffer->clear(); return; }
            const auto well = body.value("wells").toArray().first().toObject();
            QJsonArray predictions;
            for (const auto &value : well.value("rows").toArray()) {
              const auto row = value.toObject();
              const auto depth = row.value(QStringLiteral("深度"));
              predictions << QJsonObject{{"wellName", well.value("wellName")}, {"depth", depth},
                {"label", depth.toDouble() < 1060 ? QStringLiteral("河口坝") : QStringLiteral("分流河道")},
                {"confidence", .91}};
            }
            if (emptyPredictions)
              predictions = QJsonArray{}; // 200 + completed，但一个预测点都没有
            response = {{"jobId", "job-" + well.value("wellName").toString()}, {"status", "completed"},
              {"model", QJsonObject{{"name", "test-model"}, {"version", "v2"}}}, {"predictions", predictions}};
            if (failPredict) { status = 500; response = {{"error", "remote failure"}}; }
            else if (queued) {
              pending = response;
              status = 202;
              response = {{"jobId", pending.value("jobId")}, {"status", "predicting"}, {"pollAfterMs", 100}};
            }
          } else if (path.contains("/predictions/")) {
            ++polls;
            if (holdPoll) { buffer->clear(); return; }
            response = pending;
          } else { status = 404; }
          buffer->clear();
          const auto bytes = QJsonDocument(response).toJson(QJsonDocument::Compact);
          socket->write("HTTP/1.1 " + QByteArray::number(status) + " OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(bytes.size()) + "\r\n\r\n" + bytes);
          socket->disconnectFromHost();
        });
      }
    });
  }
};
class WellApiEnvironment {
  QHash<QByteArray, QByteArray> saved;
  QSet<QByteArray> present;
public:
  explicit WellApiEnvironment(const WellApiServer &server) {
    const QHash<QByteArray, QByteArray> values{
      {"PALEO_WELL_FACIES_URL", "http://127.0.0.1:" + QByteArray::number(server.server.serverPort()) + "/api/v1"},
      {"PALEO_WELL_FACIES_API_KEY", "test-key"}, {"PALEO_WELL_FACIES_NO_KEYCHAIN", "1"}};
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
      if (qEnvironmentVariableIsSet(it.key())) present.insert(it.key());
      saved.insert(it.key(), qgetenv(it.key()));
      qputenv(it.key(), it.value());
    }
  }
  ~WellApiEnvironment() {
    for (auto it = saved.cbegin(); it != saved.cend(); ++it)
      if (present.contains(it.key())) qputenv(it.key(), it.value()); else qunsetenv(it.key());
  }
};

class TestMappingWorkbench : public QObject {
  Q_OBJECT
  struct Fixture {
    QTemporaryDir dir;
    DataCatalog catalog;
    QgisProjectService project;
    PaleoProjectStore store;
    LayerManifest manifest{dir.filePath("project.sqlite")};
    QgisLayerService layers{&project, &manifest};
    QgisProcessingService processing{&store};
    ConstraintWorkflow constraints{&processing, &layers};
    MappingWorkbench work{&layers, &processing, &project, &constraints};
    // 方向51：替身 Mock 不再由 workbench 自装（产品装配走 app/aiwiring 的
    // router）——需要旧有替身行为的用例在这里显式注入。
    MockRemotePredictionService prediction;
    bool init() {
      work.setPredictionService(&prediction);
      if (!catalog.open(dir.path()) ||
          !project.createProject(dir.filePath("project.qgz")) ||
          !manifest.open())
        return false;
      store.setProjectPaths(project.projectPath(), dir.filePath("project.gpkg"),
                            dir.filePath("project.sqlite"));
      constraints.setStore(&store);
      constraints.setCatalog(&catalog, dir.path());
      project.setDeclarationProvider(
          [this](QVector<LayerDeclaration> *out, QString *error) {
            return layers.tryDeclared(out, error);
          });
      processing.setProject(project.project());
      work.bindCatalog(&catalog, dir.path());
      return true;
    }
    QString input(const QString &name, const QString &type) {
      DerivedAssetRegistrar r(&catalog, dir.path());
      auto st = r.stage(type, name, name + ".dat");
      QFile f(st.absolutePath);
      if (!f.open(QIODevice::WriteOnly))
        return {};
      if (type == "well_log")
        f.write("~V\nVERS. 2.0 : version\nWRAP. NO : wrap\n~W\nNULL. -999.25 : "
                "null\n~C\nDEPT.M : depth\nGR.API : gamma\n~A\n1000 30\n1030 "
                "50\n1060 90\n1090 45\n1120 20\n");
      else
        f.write("mock input bytes");
      f.close();
      if (!r.commit(st, {}, "fixture", {}))
        return {};
      return st.assetId;
    }
    QString well(const QString &name, double x, double y) {
      auto asset = input(name, "well_log");
      CatalogEntity e;
      e.id = name;
      e.name = name;
      e.entityType = "well";
      e.hasSurface = true;
      e.surfaceX = x;
      e.surfaceY = y;
      e.coordinateStatus = "untransformed";
      if (!catalog.addEntity(e))
        return {};
      EntityAssetLink l;
      l.entityType = "well";
      l.entityId = e.id;
      l.assetId = asset;
      l.role = "well_log";
      if (!catalog.addLink(l))
        return {};
      return e.id;
    }
    bool tops(const QString &wellId, const QString &text) {
      const QString id = "tops-" + wellId;
      const QString path = dir.filePath(id + ".dat");
      QFile file(path);
      if (!file.open(QIODevice::WriteOnly))
        return false;
      file.write(text.toUtf8());
      file.close();
      CatalogAsset a;
      a.id = id;
      a.type = "well_tops";
      a.displayName = id;
      CatalogVersion v;
      v.id = "v-" + id;
      v.assetId = id;
      v.path = path;
      v.managed = false;
      v.stage = "RAW";
      v.versionNumber = 1;
      EntityAssetLink l;
      l.entityId = wellId;
      l.entityType = "well";
      l.assetId = id;
      l.role = "tops";
      l.isPrimary = true;
      QString error;
      return catalog.addAsset(a, &error) && catalog.addVersion(v, &error) &&
             catalog.addLink(l, &error);
    }
    QString seismic() {
      auto asset = input("seismic", "seismic");
      CatalogEntity e;
      e.id = "survey";
      e.name = "survey";
      e.entityType = "seismic_survey";
      e.corners = {{0, 0}, {640, 0}, {640, 640}, {0, 640}};
      if (!catalog.addEntity(e))
        return {};
      EntityAssetLink l;
      l.entityId = e.id;
      l.entityType = e.entityType;
      l.assetId = asset;
      l.role = "seismic_volume";
      if (!catalog.addLink(l))
        return {};
      return asset;
    }
    QString latest(const QString &kind) {
      QString found;
      int ver = 0;
      for (const auto &row : work.products("D61")) {
        auto r = row.toMap();
        if (r.value("kind") == kind && r.value("version").toInt() > ver) {
          found = r.value("id").toString();
          ver = r.value("version").toInt();
        }
      }
      return found;
    }
  };
  static QByteArray digest(const QString &path) {
    QFile f(path.section('|', 0, 0));
    if (!f.open(QIODevice::ReadOnly))
      return {};
    return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256);
  }
  static QVariantList hierarchySchema() {
    QVariantList schema;
    const QStringList parents{"河流", "河流", "三角洲", "三角洲", "湖泊"};
    const QStringList children{"河道", "河道", "河道", "河道", ""};
    const QStringList leaves{"主槽", "边滩", "主槽", "边滩", ""};
    for (int i = 0; i < 5; ++i)
      schema << QVariantMap{{"code", i + 1},
                            {"name", parents[i] + leaves[i]},
                            {"facies", parents[i]},
                            {"subfacies", children[i]},
                            {"microfacies", leaves[i]},
                            {"color", i < 2 ? "#97B4CE" : "#F2D28B"}};
    return schema;
  }
  static QString hierarchyDraft(Fixture &f, QString *error) {
    if (!f.work.saveFacies("D61", hierarchySchema(), error))
      return {};
    QgsVectorLayer grid("Polygon?field=facies_code:integer", "grid", "memory");
    grid.setCrs(
        QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt()));
    QgsFeatureList features;
    for (int i = 0; i < 5; ++i) {
      QgsFeature feature(grid.fields());
      feature.setAttribute("facies_code", i + 1);
      feature.setGeometry(
          QgsGeometry::fromRect(QgsRectangle(i * 100, 0, (i + 1) * 100, 100)));
      features << feature;
    }
    if (!grid.dataProvider()->addFeatures(features))
      return {};
    const auto path = f.dir.filePath("input-grid.gpkg");
    if (!MappingArtifactWriter::vectorSnapshot(&grid, path, error))
      return {};
    LayerDeclaration d;
    d.layerId = "input.grid";
    d.horizon = "D61";
    d.type = "vector";
    d.group = "05_Composition";
    d.title = "hierarchical grid";
    d.source = path + "|layername=features";
    f.layers.declare(d);
    return f.work.copyForEditing(d.layerId, {}, error);
  }
private slots:
  void advancedSingleFactorsReachMappingWithoutReopeningProject() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto source = f.dir.filePath("advanced-samples.geojson");
    QVERIFY(MappingArtifactWriter::points(
        source,
        {QVariantMap{{"x", 0}, {"y", 0}, {"z", 10}},
         QVariantMap{{"x", 100}, {"y", 0}, {"z", 20}},
         QVariantMap{{"x", 0}, {"y", 100}, {"z", 40}},
         QVariantMap{{"x", 100}, {"y", 100}, {"z", 60}}},
        &error));
    LayerDeclaration samples;
    samples.layerId = "wells.D61";
    samples.horizon = "D61";
    samples.type = "vector";
    samples.source = source;
    samples.title = "高级工具样点";
    QVERIFY(f.layers.declare(samples, &error));
    MappingWorkbenchPage page("compose", &f.work);
    page.setHorizon("D61");
    auto *inputs = page.findChild<QListWidget *>("workbenchInputs");
    const QVariantMap options{
        {"pointsLayerId", samples.layerId}, {"field", "z"}, {"cellSize", 10}};
    // Use exactly the advanced-tools entry, bypassing MappingWorkbench.
    QVERIFY2(f.constraints.generateFactor("D61", "sandthick", options, &error),
             qPrintable(error));
    const auto first = f.work.versionForLayer("factor.D61.sandthick");
    QVERIFY(!first.id.isEmpty());
    const auto firstLayer = "product." + first.id;
    QTRY_VERIFY(!f.work.declaration(firstLayer).layerId.isEmpty());
    // #346：changed→scheduleRefresh，不可见页不刷；测试页未 show，显式 refresh。
    page.refresh();
    int checked = -1;
    for (int i = 0; i < inputs->count(); ++i)
      if (inputs->item(i)->data(Qt::UserRole).toString() == firstLayer)
        checked = i;
    QVERIFY(checked >= 0);
    inputs->item(checked)->setCheckState(Qt::Checked);
    const auto originalPath = f.work.declaration(firstLayer).source;
    const auto hash = digest(originalPath);
    QVERIFY2(f.constraints.generateFactor("D61", "sandthick", options, &error),
             qPrintable(error));
    const auto second = f.work.versionForLayer("factor.D61.sandthick");
    QVERIFY(second.id != first.id);
    QTRY_VERIFY(!f.work.declaration("product." + second.id).layerId.isEmpty());
    page.refresh();
    QCOMPARE(f.work.versionForLayer(firstLayer).id, first.id);
    QCOMPARE(f.work.declaration(firstLayer).source, originalPath);
    QCOMPARE(digest(originalPath), hash);
    int factors = 0;
    bool stillChecked = false;
    for (const auto &entry : f.work.products("D61")) {
      const auto row = entry.toMap();
      if (row.value("kind") != "single_factor_raster")
        continue;
      ++factors;
      QCOMPARE(row.value("asset_id").toString(), first.assetId);
      QVERIFY(row.value("version_id") == first.id ||
              row.value("version_id") == second.id);
    }
    QCOMPARE(factors, 2);
    for (int i = 0; i < inputs->count(); ++i)
      if (inputs->item(i)->data(Qt::UserRole).toString() == firstLayer)
        stillChecked = inputs->item(i)->checkState() == Qt::Checked;
    QVERIFY(stillChecked);
    QCOMPARE(f.work.layerForVersion(first.id, &error), firstLayer);
    auto *rendered = f.layers.instantiate(firstLayer, &error);
    QVERIFY(rendered);
    QCOMPARE(rendered->customProperty("paleoAssetId").toString(),
             first.assetId);
    QCOMPARE(rendered->customProperty("paleoVersionId").toString(), first.id);
    page.selectLayer(firstLayer);
    QVERIFY(page.commandButton("catalog")->isEnabled());
    QSignalSpy intent(&page, &MappingWorkbenchPage::commandRequested);
    page.commandButton("catalog")->click();
    QCOMPARE(intent.last()[0].toString(), QString("catalog"));
    // Old factor versions remain usable as frozen evidence sources.
    const auto draft = hierarchyDraft(f, &error);
    QVERIFY(!draft.isEmpty());
    auto *v = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft));
    auto features = v->getFeatures();
    QgsFeature feature;
    QVERIFY(features.nextFeature(feature));
    QVERIFY(f.work.addEvidence(draft, {feature.id()},
                               {{"text", "依据重算前的砂厚图"},
                                {"level", "facies"},
                                {"source_layer", firstLayer}},
                               &error));
    QCOMPARE(f.work.evidence(draft, {feature.id()})[0]
                 .toMap()
                 .value("source_version")
                 .toString(),
             first.id);
    QVERIFY(v->rollBack());
  }
  void movingCatalogAliasesDoNotReplaceHistoricalReferences() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    DerivedAssetRegistrar registrar(&f.catalog, f.dir.path());
    const QVariantMap extra{{"mapping_product", true},
                            {"layer_id", "cartographic.D61.sandthick"},
                            {"manifest_layer_id", "cartographic.D61.sandthick"},
                            {"horizon", "D61"},
                            {"layer_type", "raster"},
                            {"group", "04_SingleFactor/Cartographic"},
                            {"title", "砂厚制图工作场"},
                            {"kind", "single_factor_cartographic_work"},
                            {"value_source", "cartographic_work"}};
    QString first, second;
    for (int i = 0; i < 2; ++i) {
      const auto staged = registrar.stage("single_factor_cartographic_work",
                                          "砂厚制图工作场", "work.tif", &error);
      QVERIFY(staged.isValid());
      QVERIFY(MappingArtifactWriter::raster(staged.absolutePath, {1, 1, 2, 2},
                                            2, 2, QRectF(0, 0, 100, 100),
                                            &error));
      QVERIFY(registrar.commit(staged, {}, "fixture", extra, &error));
      (i == 0 ? first : second) = staged.versionId;
    }
    QTRY_VERIFY(!f.work.declaration("product." + second).layerId.isEmpty());
    QCOMPARE(f.work.versionForLayer("product." + first).id, first);
    QCOMPARE(f.work.versionForLayer("product." + second).id, second);
    QCOMPARE(f.work.layerForVersion(first, &error), "product." + first);
    QVERIFY(f.work.declaration("product." + first).source !=
            f.work.declaration("product." + second).source);
    QVERIFY(f.work.products("D62").isEmpty());
    QCOMPARE(f.work.products("D61").size(), 2);
    QVERIFY(f.work.compose("D61", {"product." + first}, {}, &error).isEmpty());
    QVERIFY(
        !error
             .isEmpty()); // display-only work cannot enter quantitative fusion.
    f.work.bindCatalog(&f.catalog, f.dir.path());
    QCOMPARE(f.work.versionForLayer("product." + first).id, first);
    QCOMPARE(f.work.products("D61").size(), 2);
    QVERIFY(f.work.layerForVersion("missing", &error).isEmpty());
  }
  void hierarchyScaleFallbackAndScopedNames() {
    QCOMPARE(FaciesHierarchy::resolveLevel("auto", 8000001), QString("facies"));
    QCOMPARE(FaciesHierarchy::resolveLevel("auto", 8000000), QString("sub_facies"));
    QCOMPARE(FaciesHierarchy::resolveLevel("auto", 7999999),
             QString("sub_facies"));
    QCOMPARE(FaciesHierarchy::resolveLevel("auto", 4000001),
             QString("sub_facies"));
    QCOMPARE(FaciesHierarchy::resolveLevel("auto", 4000000),
             QString("micro_facies"));
    QCOMPARE(FaciesHierarchy::resolveLevel("auto", 3999999),
             QString("micro_facies"));
    QCOMPARE(FaciesHierarchy::resolveLevel("facies", 1), QString("facies"));
    QCOMPARE(FaciesHierarchy::path({{"microfacies", "仅微相"}}),
             QStringList({"仅微相", "仅微相", "仅微相"}));
    QCOMPARE(FaciesHierarchy::path({{"facies", "相"}, {"microfacies", "微相"}}),
             QStringList({"相", "相", "微相"}));
    auto schema = hierarchySchema();
    QCOMPARE(FaciesHierarchy::legend(schema, "facies", {1, 2, 3, 4, 5}).size(),
             3);
    QCOMPARE(
        FaciesHierarchy::legend(schema, "sub_facies", {1, 2, 3, 4, 5}).size(),
        3);
    QCOMPARE(
        FaciesHierarchy::legend(schema, "micro_facies", {1, 2, 3, 4, 5}).size(),
        5);
    QVERIFY(FaciesHierarchy::key(schema[0].toMap(), "sub_facies") !=
            FaciesHierarchy::key(schema[2].toMap(), "sub_facies"));
    QCOMPARE(FaciesHierarchy::legend(schema, "facies", {1, 2}).size(), 1);
    QCOMPARE(
        FaciesHierarchy::legend(schema, "facies", {1, 77, QVariant()}).size(),
        2);
  }
  void hierarchyRenderingMergesAndUpdatesLabelsWithoutChangingGeometry() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto draft = hierarchyDraft(f, &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *v = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft));
    QVERIFY(v);
    auto it = v->getFeatures();
    QgsFeature a, b;
    QVERIFY(it.nextFeature(a));
    QVERIFY(it.nextFeature(b));
    const auto geometry = a.geometry().asWkt();
    QVERIFY(f.work.setDisplayMode(draft, "facies", &error));
    auto *merged = dynamic_cast<QgsMergedFeatureRenderer *>(v->renderer());
    QVERIFY(merged);
    auto *categorized = dynamic_cast<const QgsCategorizedSymbolRenderer *>(
        merged->embeddedRenderer());
    QVERIFY(categorized);
    QCOMPARE(categorized->categories().size(), 3);
    auto context = v->createExpressionContext();
    context.setFeature(a);
    QgsExpression expr(categorized->classAttribute());
    QVERIFY(expr.prepare(&context));
    const auto keyA = expr.evaluate(&context);
    context.setFeature(b);
    QCOMPARE(expr.evaluate(&context), keyA);
    QVERIFY(f.work.setDisplayMode(draft, "micro_facies", &error));
    QCOMPARE(f.work.displayLegend(draft).size(), 5);
    QCOMPARE(v->getFeature(a.id()).geometry().asWkt(), geometry);
    QCOMPARE(v->customProperty("paleo/faciesResolvedLevel").toString(),
             QString("micro_facies"));
    QVERIFY(f.work.setDisplayMode(draft, "auto", &error));
    f.work.updateDisplayScale(9000000);
    QCOMPARE(f.work.resolvedLevel(draft), QString("facies"));
    QCOMPARE(f.work.displayLegend(draft).size(), 3);
  }
  void hierarchyParentAssignmentIsOneUndoAndKeepsCompatibleChildren() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto draft = hierarchyDraft(f, &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *v = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft));
    QVERIFY(v);
    auto it = v->getFeatures();
    QgsFeature a, b;
    QVERIFY(it.nextFeature(a));
    QVERIFY(it.nextFeature(b));
    QVERIFY(f.work.assignHierarchy(draft, {a.id()}, 3, "facies", &error));
    QCOMPARE(v->getFeature(a.id()).attribute("facies_code").toInt(), 3);
    QCOMPARE(v->getFeature(b.id()).attribute("facies_code").toInt(), 4);
    QCOMPARE(v->getFeature(b.id()).attribute("microfacies").toString(),
             QString("边滩"));
    QCOMPARE(v->undoStack()->count(), 1);
    v->selectByIds({a.id()});
    QVERIFY(f.work.selectHierarchyMembers(draft, "facies", &error));
    QCOMPARE(v->selectedFeatureIds().size(), 4);
    v->undoStack()->undo();
    QCoreApplication::processEvents();
    QCOMPARE(v->getFeature(a.id()).attribute("facies_code").toInt(), 1);
    QCOMPARE(v->getFeature(b.id()).attribute("facies_code").toInt(), 2);
    QVERIFY(f.work.assignHierarchy(draft, {a.id()}, 3, "sub_facies", &error));
    QCOMPARE(v->getFeature(b.id()).attribute("facies_code").toInt(), 4);
    v->undoStack()->undo();
    QVERIFY(f.work.assignHierarchy(draft, {a.id()}, 3, "micro_facies", &error));
    QCOMPARE(v->getFeature(b.id()).attribute("facies_code").toInt(), 2);
    QVERIFY(v->rollBack());
  }
  void hierarchyTopologyRejectsOverlapAndAllowsSharedBoundaryChanges() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto draft = hierarchyDraft(f, &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *v = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft));
    QVERIFY(v);
    QVERIFY(FaciesHierarchyRenderer::validateTopology(v, &error));
    const auto snapshot = f.work.versionForLayer(draft);
    const auto hash =
        digest(DataCatalog::resolvedVersionPath(f.dir.path(), snapshot));
    auto it = v->getFeatures();
    QgsFeature a, b;
    QVERIFY(it.nextFeature(a));
    QVERIFY(it.nextFeature(b));
    auto expanded = QgsGeometry::fromRect(QgsRectangle(0, 0, 120, 100));
    auto narrowed = QgsGeometry::fromRect(QgsRectangle(120, 0, 200, 100));
    QVERIFY(v->startEditing());
    v->beginEditCommand("bad overlap");
    QVERIFY(v->changeGeometry(a.id(), expanded));
    v->endEditCommand();
    QVERIFY(!v->commitChanges());
    QVERIFY(v->isEditable());
    QCOMPARE(digest(DataCatalog::resolvedVersionPath(f.dir.path(), snapshot)),
             hash);
    v->undoStack()->undo();
    v->beginEditCommand("shared edge");
    QVERIFY(v->changeGeometry(a.id(), expanded));
    QVERIFY(v->changeGeometry(b.id(), narrowed));
    v->endEditCommand();
    QVERIFY2(FaciesHierarchyRenderer::validateTopology(v, &error),
             qPrintable(error));
    QVERIFY(v->commitChanges());
    QVERIFY2(f.work.saveEditingVersion(draft, &error), qPrintable(error));
    QCOMPARE(f.work.versionForLayer(draft).versionNumber, 2);
    QVERIFY(f.work.setDisplayMode(draft, "facies", &error));
    QCOMPARE(v->getFeature(a.id()).geometry().boundingBox().xMaximum(), 120.0);
    QVERIFY(f.work.setDisplayMode(draft, "micro_facies", &error));
    QCOMPARE(v->getFeature(b.id()).geometry().boundingBox().xMinimum(), 120.0);
  }
  void hierarchyDraftForcesSharedVertexTopology() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto draft = hierarchyDraft(f, &error);
    QVERIFY(!draft.isEmpty());
    auto *v = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft));
    QVERIFY(v);
    QgsMapCanvas canvas;
    PaleoEditingToolbar toolbar(&canvas);
    toolbar.setProject(f.project.project());
    toolbar.setLayers({v});
    toolbar.setCurrentLayer(v);
    QVERIFY(toolbar.actionTopological()->isChecked());
    QVERIFY(!toolbar.actionTopological()->isEnabled());
    toolbar.actionVertexEdit()->trigger();
    auto *tool = qobject_cast<PaleoVertexTool *>(canvas.mapTool());
    QVERIFY(tool);
    QVERIFY(tool->topologicalEditingEnabled());
    toolbar.actionTopological()->setChecked(false);
    QVERIFY(tool->topologicalEditingEnabled());
    QVERIFY(toolbar.actionTopological()->isChecked());
    QVERIFY(toolbar.cancelEditing());
  }
  void evidenceUndoVersionLineageAndRecovery() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto draft = hierarchyDraft(f, &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *v = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft));
    QVERIFY(v);
    auto it = v->getFeatures();
    QgsFeature feature;
    QVERIFY(it.nextFeature(feature));
    const QVariantMap entry{{"text", "井 A 砂泥比与河道解释一致"},
                            {"level", "facies"},
                            {"source_layer", "input.grid"}};
    QVERIFY(f.work.addEvidence(draft, {feature.id()}, entry, &error));
    QCOMPARE(f.work.evidence(draft, {feature.id()}).size(), 1);
    const auto evidence = f.work.evidence(draft, {feature.id()})[0].toMap();
    QVERIFY(!evidence.value("source_version").toString().isEmpty());
    v->undoStack()->undo();
    QCoreApplication::processEvents();
    QCOMPARE(f.work.evidence(draft, {feature.id()}).size(), 0);
    v->undoStack()->redo();
    QCoreApplication::processEvents();
    QCOMPARE(f.work.evidence(draft, {feature.id()}).size(), 1);
    QVERIFY(v->commitChanges());
    QVERIFY(f.work.saveEditingVersion(draft, &error));
    const auto snapshot = f.work.versionForLayer(draft);
    QCOMPARE(v->customProperty("paleoVersionId").toString(), snapshot.id);
    QVERIFY(snapshot.parentVersionIds.contains(
        evidence.value("source_version").toString()));
    QgsVectorLayer saved(
        DataCatalog::resolvedVersionPath(f.dir.path(), snapshot) +
            "|layername=features",
        "saved", "ogr");
    QVERIFY(saved.isValid());
    auto savedIt = saved.getFeatures();
    QgsFeature savedFeature;
    QVERIFY(savedIt.nextFeature(savedFeature));
    QVERIFY(
        savedFeature.attribute("facies_evidence").toString().contains("井 A"));
    f.work.bindCatalog(&f.catalog, f.dir.path());
    QCOMPARE(f.work.evidence(draft, {feature.id()}).size(), 1);
    QVERIFY(f.work.removeEvidence(draft, {feature.id()},
                                  evidence.value("id").toString(), &error));
    QCOMPARE(f.work.evidence(draft, {feature.id()}).size(), 0);
    v->undoStack()->undo();
    QCoreApplication::processEvents();
    QCOMPARE(f.work.evidence(draft, {feature.id()}).size(), 1);
    QVERIFY(v->rollBack());
    QVERIFY(f.work.assignHierarchy(draft, {feature.id()}, 3, "micro_facies",
                                   &error));
    QVERIFY(f.work.evidence(draft, {feature.id()})[0]
                .toMap()
                .value("needs_review")
                .toBool());
    QVERIFY(v->rollBack());
  }
  void semanticReviewConflictUnlinkAndSnapshotRemainTraceable() {
    Fixture f; QVERIFY(f.init()); QString error;
    const auto draft = hierarchyDraft(f, &error); QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft)); QVERIFY(layer);
    auto features = layer->getFeatures(); QgsFeature feature; QVERIFY(features.nextFeature(feature)); features.close();
    const auto original = f.work.versionForLayer(draft);
    const auto originalBytes = digest(DataCatalog::resolvedVersionPath(f.dir.path(), original));
    QVERIFY(f.work.addEvidence(draft, {feature.id()}, {{"text", "井相与当前图斑存在冲突"}, {"role", "CONTRADICTED_BY"},
        {"type", "well"}, {"source_layer", "input.grid"}, {"actor_id", "local:geologist"}, {"reason", "保留冲突来源"}}, &error));
    const auto snapshot = f.work.featureEvidence(draft, {feature.id()});
    QVERIFY(!snapshot.value("feature_uid").toString().isEmpty());
    QCOMPARE(snapshot.value("records").toList()[0].toMap().value("role").toString(), QString("CONTRADICTED_BY"));
    const auto revision = snapshot.value("interpretation").toMap().value("revision").toULongLong();
    QVERIFY(revision > 0);
    const auto beforeJson = layer->getFeature(feature.id()).attribute("paleo_interpretation");
    QVERIFY(!f.work.changeEvidence(draft, {feature.id()}, "review", {{"actor_id", "local:reviewer"}, {"reason", "review"}, {"status", "Confirmed"}, {"expected_revision", revision - 1}}, &error));
    QCOMPARE(layer->getFeature(feature.id()).attribute("paleo_interpretation"), beforeJson);
    QVERIFY(f.work.changeEvidence(draft, {feature.id()}, "review", {{"actor_id", "local:reviewer"}, {"reason", "争议待新资料"}, {"status", "Disputed"}, {"expected_revision", revision}}, &error));
    // #444：争议复核后状态必须是 Disputed（此时历史至少含 link+review 两条）。
    {
      QVariantMap interpretation; QString decodeError;
      QVERIFY(paleo::evidence::decodeInterpretation(layer->getFeature(feature.id()).attribute("paleo_interpretation").toString(), &interpretation, &decodeError));
      QCOMPARE(interpretation.value("status").toString(), QString("Disputed"));
      QVERIFY(interpretation.value("history").toList().size() >= 2);
    }
    const auto evidenceId = snapshot.value("records").toList()[0].toMap().value("id").toString();
    QVERIFY(f.work.removeEvidence(draft, {feature.id()}, evidenceId, &error));
    QCOMPARE(f.work.evidence(draft, {feature.id()}).size(), 0);
    auto latest = f.work.featureEvidence(draft, {feature.id()});
    QVERIFY(latest.value("records").toList()[0].toMap().value("unlinked").toBool());
    const auto history = latest.value("interpretation").toMap().value("history").toList();
    QVERIFY(history.size() >= 3);
    QCOMPARE(history.back().toMap().value("before_record").toMap().value("role").toString(), QString("CONTRADICTED_BY"));
    QVERIFY(layer->commitChanges()); QVERIFY2(f.work.saveEditingVersion(draft, &error), qPrintable(error));
    const auto saved = f.work.versionForLayer(draft);
    QVERIFY(saved.id != original.id);
    QCOMPARE(digest(DataCatalog::resolvedVersionPath(f.dir.path(), original)), originalBytes);
    QgsVectorLayer reopened(DataCatalog::resolvedVersionPath(f.dir.path(), saved) + "|layername=features", "saved", "ogr"); QVERIFY(reopened.isValid());
    const auto restored = MappingFeatureIdentity::find(&reopened, snapshot.value("feature_uid").toString()); QVERIFY(restored.isValid());
    QVariantMap interpretation;
    QVERIFY(paleo::evidence::decodeInterpretation(restored.attribute("paleo_interpretation").toString(), &interpretation, &error));
    QCOMPARE(interpretation.value("history").toList().size(), history.size());
    QVERIFY(saved.parentVersionIds.contains(snapshot.value("records").toList()[0].toMap().value("source_version").toString()));
  }
  void mapEvidenceAndGraphUseOneStableTargetWithoutEditing() {
    Fixture f; QVERIFY(f.init()); QString error;
    const auto draft = hierarchyDraft(f, &error); QVERIFY(!draft.isEmpty());
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft)); QVERIFY(layer);
    auto iterator = layer->getFeatures(); QgsFeature feature; QVERIFY(iterator.nextFeature(feature)); iterator.close();
    SelectionContext selection; selection.setActiveHorizon("D61");
    QgisCanvasController canvas; canvas.setLayerResolver([&f](const QString &id) { return f.layers.layer(id); });
    EvidenceInteraction interaction(&f.work, &selection, &f.layers, &f.project, &canvas);
    layer->selectByIds({feature.id()});
    QCOMPARE(selection.target().featureUid, MappingFeatureIdentity::uid(feature));
    QCOMPARE(selection.target().layerId, draft);
    const auto target = selection.target(); QVERIFY(!layer->isEditable());
    QVariantMap graphNode{{"project_id", target.projectId}, {"layer_id", draft}, {"version_id", target.versionId},
      {"asset_id", target.assetId}, {"feature_uid", target.featureUid}, {"horizon", "D61"}};
    layer->removeSelection(); interaction.navigateGraphNode(graphNode);
    QCOMPARE(selection.target().featureUid, target.featureUid);
    QCOMPARE(interaction.snapshot().value("feature_uid").toString(), target.featureUid);
    QVERIFY(!layer->isEditable()); QVERIFY(layer->selectedFeatureIds().isEmpty());
    QVERIFY(f.work.setDisplayMode(draft, "facies", &error));
    QCOMPARE(selection.target().featureUid, target.featureUid); // display refresh preserves graph research target
    const auto projectPath = f.project.projectPath();
    QVERIFY(f.project.writeProject());
    f.project.closeProject(); QVERIFY(selection.target().isEmpty());
    QVERIFY(f.project.openProject(projectPath));
    QTRY_COMPARE(selection.target().featureUid, target.featureUid);
    auto *reloaded = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft)); QVERIFY(reloaded);
    const auto restored = MappingFeatureIdentity::find(reloaded, target.featureUid); QVERIFY(restored.isValid());
    reloaded->selectByIds({restored.id()});
    QCOMPARE(interaction.snapshot().value("feature_uid").toString(), target.featureUid);
    QSignalSpy failures(&interaction, &EvidenceInteraction::navigationFailed);
    graphNode.insert("horizon", "Other"); graphNode.insert("project_id", "OtherProject");
    interaction.navigateGraphNode(graphNode); interaction.navigate(graphNode); QVERIFY(failures.count() >= 2);
    QCOMPARE(selection.target().featureUid, target.featureUid);
    selection.setActiveHorizon("Other"); QVERIFY(selection.target().isEmpty());
    f.project.closeProject(); QVERIFY(selection.target().isEmpty());
  }
  void repeatedNativeMergesKeepOneParentTreeAndReopenableHistory() {
    Fixture f; QVERIFY(f.init()); QString error;
    const auto draft = hierarchyDraft(f, &error); QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft)); QVERIFY(layer);
    QgsFeature survivor, candidate; auto features = layer->getFeatures();
    while (features.nextFeature(candidate)) if (!survivor.isValid() || candidate.geometry().boundingBox().xMaximum() > survivor.geometry().boundingBox().xMaximum()) survivor = candidate;
    features.close(); QVERIFY(survivor.isValid());
    QVERIFY(layer->startEditing()); const auto fid = survivor.id();
    for (int n = 0; n < 20; ++n) {
      const auto current = layer->getFeature(fid); const auto box = current.geometry().boundingBox();
      QgsFeature adjacent(layer->fields()); adjacent.setAttribute("facies_code", current.attribute("facies_code"));
      adjacent.setGeometry(QgsGeometry::fromRect(QgsRectangle(box.xMaximum(), box.yMinimum(), box.xMaximum() + 100, box.yMaximum())));
      layer->beginEditCommand("new neighbouring interpretation"); QVERIFY(layer->addFeature(adjacent)); layer->endEditCommand();
      const auto before = layer->getFeature(fid); const auto parentUid = MappingFeatureIdentity::uid(before);
      const auto neighbourUid = MappingFeatureIdentity::uid(layer->getFeature(adjacent.id()));
      auto united = before.geometry().combine(layer->getFeature(adjacent.id()).geometry()); QVERIFY(!united.isEmpty());
      layer->beginEditCommand("native merge"); QVERIFY(layer->changeGeometry(fid, united)); QVERIFY(layer->deleteFeature(adjacent.id())); layer->endEditCommand();
      const auto merged = layer->getFeature(fid); QVERIFY(MappingFeatureIdentity::uid(merged) != parentUid);
      const auto parents = QJsonDocument::fromJson(merged.attribute(MappingFeatureIdentity::ParentsField).toString().toUtf8()).toVariant().toList();
      QVERIFY(parents.contains(parentUid)); QVERIFY(parents.contains(neighbourUid));
      QVERIFY2(layer->property("paleo.evidenceEditFailure").toString().isEmpty(),
               qPrintable(layer->property("paleo.evidenceEditFailure").toString()));
      QVariantMap interpretation; QVERIFY2(paleo::evidence::decodeInterpretation(merged.attribute("paleo_interpretation").toString(), &interpretation, &error), qPrintable(error));
      QVERIFY(!interpretation.contains("inherited_interpretation")); QCOMPARE(interpretation.value("parent_interpretations").toList().size(), 2);
      // Parent trees grow with actual edits, never double merely because the
      // surviving parent appears in two top-level fields.
      QVERIFY(merged.attribute("paleo_interpretation").toString().size() < (n + 1) * 20000);
    }
    QVERIFY2(layer->commitChanges(), qPrintable(layer->commitErrors().join(';')));
    QVERIFY2(f.work.saveEditingVersion(draft, &error), qPrintable(error));
    const auto version = f.work.versionForLayer(draft);
    QgsVectorLayer reopened(DataCatalog::resolvedVersionPath(f.dir.path(), version) + "|layername=features", "merged snapshot", "ogr"); QVERIFY(reopened.isValid());
    const auto saved = MappingFeatureIdentity::find(&reopened, MappingFeatureIdentity::uid(layer->getFeature(fid))); QVERIFY(saved.isValid());
    QVariantMap interpretation; QVERIFY(paleo::evidence::decodeInterpretation(saved.attribute("paleo_interpretation").toString(), &interpretation, &error));
    QCOMPARE(interpretation.value("parent_interpretations").toList().size(), 2);
    const auto qaDir = qEnvironmentVariable("PALEO_EVIDENCE_QA_DIR");
    if (!qaDir.isEmpty()) {
      QDir().mkpath(qaDir); layer->selectByIds({fid});
      MappingWorkbenchPage page("compose", &f.work); page.setHorizon("D61"); page.selectLayer(draft);
      auto *group = page.findChild<QWidget *>("workbenchEvidence"); QVERIFY(group);
      page.resize(520, 1800); page.show(); QCoreApplication::processEvents();
      auto *tabs = page.findChild<QTabWidget *>("faciesEvidenceTabs"); QVERIFY(tabs);
      for (int n = 0; n < tabs->count(); ++n) { tabs->setCurrentIndex(n); QCoreApplication::processEvents(); QVERIFY(group->grab().save(qaDir + QString("/evidence-tab-%1.png").arg(n))); }
    }
  }
  void reviewsBetweenMergesStayInTheReopenedEventLog() {
    Fixture f; QVERIFY(f.init()); QString error;
    const auto draft = hierarchyDraft(f, &error); QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft)); QVERIFY(layer);
    QgsFeature survivor, candidate; auto features = layer->getFeatures();
    while (features.nextFeature(candidate))
      if (!survivor.isValid() || candidate.geometry().boundingBox().xMaximum() > survivor.geometry().boundingBox().xMaximum())
        survivor = candidate;
    features.close(); QVERIFY(survivor.isValid());
    QVERIFY(layer->startEditing());
    const qint64 fid = survivor.id();
    QSet<QString> required;
    auto collect = [&](const QVariantMap &interpretation) {
      for (const auto &key : {QString("history"), QString("carried_events")})
        for (const auto &event : interpretation.value(key).toList()) {
          const auto id = event.toMap().value("event_id").toString();
          if (!id.isEmpty()) required.insert(id);
        }
    };
    for (int n = 0; n < 3; ++n) {
      const auto current = layer->getFeature(fid); const auto box = current.geometry().boundingBox();
      QgsFeature adjacent(layer->fields()); adjacent.setAttribute("facies_code", current.attribute("facies_code"));
      adjacent.setGeometry(QgsGeometry::fromRect(QgsRectangle(box.xMaximum(), box.yMinimum(), box.xMaximum() + 100, box.yMaximum())));
      layer->beginEditCommand("new neighbouring interpretation"); QVERIFY(layer->addFeature(adjacent)); layer->endEditCommand();
      const auto before = layer->getFeature(fid);
      auto united = before.geometry().combine(layer->getFeature(adjacent.id()).geometry()); QVERIFY(!united.isEmpty());
      layer->beginEditCommand("native merge");
      QVERIFY(layer->changeGeometry(fid, united)); QVERIFY(layer->deleteFeature(adjacent.id())); layer->endEditCommand();
      QVERIFY2(layer->property("paleo.evidenceEditFailure").toString().isEmpty(),
               qPrintable(layer->property("paleo.evidenceEditFailure").toString()));
      QVariantMap interpretation;
      QVERIFY2(paleo::evidence::decodeInterpretation(layer->getFeature(fid).attribute("paleo_interpretation").toString(), &interpretation, &error), qPrintable(error));
      QCOMPARE(interpretation.value("parent_interpretations").toList().size(), 2);
      for (const auto &parent : interpretation.value("parent_interpretations").toList()) {
        const auto row = parent.toMap();
        QVERIFY(!row.contains("history"));
        QVERIFY(!row.contains("carried_events"));
        QVERIFY(!row.contains("parent_interpretations"));
      }
      QVERIFY(layer->getFeature(fid).attribute("paleo_interpretation").toString().size() < (n + 1) * 20000);
      if (n < 2) {
        const auto revision = interpretation.value("revision").toULongLong();
        QVERIFY2(f.work.changeEvidence(draft, {fid}, "review", {{"actor_id", "local:reviewer"}, {"reason", "合并后复核"},
            {"status", "Disputed"}, {"expected_revision", revision}}, &error), qPrintable(error));
        if (n == 0) {
          QVERIFY2(f.work.addEvidence(draft, {fid}, {{"text", "合并间隙的人工备注"}, {"role", "REFERENCES"}, {"type", "manual"},
              {"actor_id", "local:geologist"}, {"reason", "记录尚未落版本的来源"}}, &error), qPrintable(error));
          QString evidenceId;
          for (const auto &entry : f.work.evidence(draft, {fid})) {
            const auto row = entry.toMap();
            if (row.value("text").toString() == QStringLiteral("合并间隙的人工备注")) evidenceId = row.value("id").toString();
          }
          QVERIFY(!evidenceId.isEmpty());
          QVERIFY2(f.work.removeEvidence(draft, {fid}, evidenceId, &error), qPrintable(error));
        }
        QVariantMap reviewed;
        QVERIFY2(paleo::evidence::decodeInterpretation(layer->getFeature(fid).attribute("paleo_interpretation").toString(), &reviewed, &error), qPrintable(error));
        collect(reviewed);
      } else collect(interpretation);
    }
    QVERIFY2(layer->commitChanges(), qPrintable(layer->commitErrors().join(';')));
    QVERIFY2(f.work.saveEditingVersion(draft, &error), qPrintable(error));
    const auto version = f.work.versionForLayer(draft);
    QgsVectorLayer reopened(DataCatalog::resolvedVersionPath(f.dir.path(), version) + "|layername=features", "merged events", "ogr");
    QVERIFY(reopened.isValid());
    const auto saved = MappingFeatureIdentity::find(&reopened, MappingFeatureIdentity::uid(layer->getFeature(fid)));
    QVERIFY(saved.isValid());
    QVariantMap restored;
    QVERIFY2(paleo::evidence::decodeInterpretation(saved.attribute("paleo_interpretation").toString(), &restored, &error), qPrintable(error));
    QCOMPARE(restored.value("parent_interpretations").toList().size(), 2);
    QSet<QString> present;
    for (const auto &key : {QString("history"), QString("carried_events")})
      for (const auto &event : restored.value(key).toList())
        present.insert(event.toMap().value("event_id").toString());
    for (const auto &id : required) QVERIFY2(present.contains(id), qPrintable(id));
    QVERIFY(required.size() >= 3);
  }
  void rejectedSpatialBindingDoesNotRegisterSourceOrMutateTarget() {
    Fixture f; QVERIFY(f.init()); QString error;
    const auto draft = hierarchyDraft(f, &error); QVERIFY(!draft.isEmpty());
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft)); QVERIFY(layer);
    auto features = layer->getFeatures(); QgsFeature feature; QVERIFY(features.nextFeature(feature)); features.close();
    const auto versions = f.catalog.versions().size(); const auto sequence = f.catalog.mutationSeq();
    const auto original = feature.attribute("facies_evidence").toString();
    const auto artifacts = QDir(f.dir.filePath("artifacts/staging")).entryList(QDir::Files | QDir::NoDotAndDotDot);
    QVERIFY(!f.work.addEvidence(draft, {feature.id()}, {{"text", "不存在的图元"}, {"source_layer", "input.grid"},
      {"source_feature_uid", "no-such-uid"}, {"actor_id", "local:user"}, {"reason", "检验失败路径"}}, &error));
    QCOMPARE(f.catalog.versions().size(), versions); QCOMPARE(f.catalog.mutationSeq(), sequence);
    QCOMPARE(layer->getFeature(feature.id()).attribute("facies_evidence").toString(), original); QVERIFY(!layer->isEditable());
    QCOMPARE(QDir(f.dir.filePath("artifacts/staging")).entryList(QDir::Files | QDir::NoDotAndDotDot), artifacts);
    const auto beforeEvidence = layer->getFeature(feature.id()).attribute("facies_evidence");
    const auto beforeInterp = layer->getFeature(feature.id()).attribute("paleo_interpretation");
    QVERIFY(!f.work.changeEvidence(draft, {feature.id()}, "review", {{"expected_revision", .5}, {"status", "Confirmed"},
      {"actor_id", "local:user"}, {"reason", "非法修订号"}}, &error));
    // #444：非法修订号拒绝后要素属性与目录序号均不变。
    QCOMPARE(f.catalog.mutationSeq(), sequence);
    QCOMPARE(layer->getFeature(feature.id()).attribute("facies_evidence"), beforeEvidence);
    QCOMPARE(layer->getFeature(feature.id()).attribute("paleo_interpretation"), beforeInterp);
  }
  void constraintSnapshotKeepsDeletionAudit() {
    Fixture f; QVERIFY(f.init()); QString error;
    QVERIFY2(f.constraints.addConstraint("D61", "LINESTRING(0 0,10 0)", "direction_line", 1, &error), qPrintable(error));
    QVERIFY2(f.constraints.addConstraint("D61", "LINESTRING(0 1,10 1)", "break_line", 2, &error), qPrintable(error));
    const auto rows = f.constraints.loadConstraints("D61"); QCOMPARE(rows.size(), 2);
    const auto removed = rows.first().value("id").toString();
    auto *store = f.constraints.constraintStore(); QVERIFY(store);
    QVERIFY(store->setAuditContext({"local:geologist", "删除误绘约束", "op-delete", "edit"}, &error));
    QVERIFY2(store->remove(removed, &error), qPrintable(error));
    QCOMPARE(f.constraints.loadConstraints("D61").size(), 1);
    const auto layerId = f.work.snapshotConstraints("D61", {}, &error);
    QVERIFY2(!layerId.isEmpty(), qPrintable(error));
    const auto version = f.work.versionForLayer(layerId);
    QCOMPARE(version.extra.value("constraint_audit_events").toInt(), 3);
    const auto path = DataCatalog::resolvedVersionPath(f.dir.path(), version);
    ConstraintStore snapshotStore(path);
    const auto deleted = snapshotStore.history(removed);
    QCOMPARE(deleted.size(), 2);
    QVERIFY(deleted.first().value("deleted").toBool());
    QCOMPARE(deleted.first().value("actor_id").toString(), QString("local:geologist"));
    QCOMPARE(snapshotStore.historyForHorizon("D61").size(), 3);
    QCOMPARE(f.constraints.loadConstraints("D61").size(), 1);
  }
  void linkedCanvasesFollowExtentCursorAndCanDisconnect() {
    QgsMapCanvas main, reference;
    main.resize(600, 400);
    reference.resize(600, 400);
    main.setDestinationCrs(QgsCoordinateReferenceSystem("EPSG:3857"));
    reference.setDestinationCrs(main.mapSettings().destinationCrs());
    main.setExtent(QgsRectangle(0, 0, 600, 400));
    MapCanvasLink link(&main, &reference);
    main.setExtent(QgsRectangle(100, 200, 700, 600));
    QCOMPARE(reference.extent().center(), main.extent().center());
    reference.setExtent(QgsRectangle(200, 400, 800, 800));
    QCOMPARE(main.extent().center(), reference.extent().center());
    emit main.xyCoordinates(QgsPointXY(300, 500));
    bool cursor = false;
    for (auto *item : reference.scene()->items())
      cursor |= dynamic_cast<QgsRubberBand *>(item) != nullptr;
    QVERIFY(cursor);
    link.setEnabled(false);
    const auto old = reference.extent();
    main.setExtent(QgsRectangle(500, 700, 1100, 1100));
    QCOMPARE(reference.extent(), old);
    link.setEnabled(true);
    QCOMPARE(reference.extent().center(), main.extent().center());
    main.stopRendering();
    reference.stopRendering();
  }
  void hierarchyAndEvidencePanelIssueCommands() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto draft = hierarchyDraft(f, &error);
    QVERIFY(!draft.isEmpty());
    MappingWorkbenchPage page("compose", &f.work);
    page.setHorizon("D61");
    page.selectLayer(draft);
    auto *level = page.findChild<QComboBox *>("faciesDisplayLevel");
    auto *edit = page.findChild<QComboBox *>("faciesEditLevel");
    QVERIFY(level && edit);
    QSignalSpy intent(&page, &MappingWorkbenchPage::commandRequested);
    level->setCurrentIndex(1);
    QCOMPARE(intent.last()[0].toString(), QString("displayLevel"));
    edit->setCurrentIndex(0);
    page.commandButton("assignFacies")->click();
    QCOMPARE(intent.last()[1].toMap().value("edit_level").toString(),
             QString("facies"));
    auto *text = page.findChild<QPlainTextEdit *>("faciesEvidenceText");
    QVERIFY(text);
    QVERIFY(!page.commandButton("addEvidence")->isEnabled());
    // #421：layer() 只回已实例化图层——先 instantiate（同文件其它用例口径），
    // 否则 v 为 nullptr、getFeatures() 直接段错误（copyForEditing 只 declare
    // 不 instantiate；#346 后页面不再同步刷图层，崩溃于 getFeatures→isValid）。
    QString layerError;
    auto *v = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft, &layerError));
    QVERIFY2(v, qPrintable(layerError));
    auto it = v->getFeatures();
    QgsFeature feature;
    QVERIFY(it.nextFeature(feature));
    v->selectByIds({feature.id()});
    text->setPlainText("解释证据");
    QVERIFY(page.commandButton("addEvidence")->isEnabled());
    page.commandButton("addEvidence")->click();
    QCOMPARE(intent.last()[0].toString(), QString("addEvidence"));
    QCOMPARE(intent.last()[1].toMap().value("text").toString(),
             QString("解释证据"));
    QVERIFY(f.work.addEvidence(
        draft, {feature.id()},
        {{"text", "井 A 的测井砂泥比与河道解释一致"}, {"level", "facies"}},
        &error));
    const auto qa = qEnvironmentVariable("PALEO_MAPPING_QA_DIR");
    if (!qa.isEmpty()) {
      QDir().mkpath(qa);
      page.resize(680, 2200);
      page.show();
      for (auto theme : {PaleoTheme::Theme::Light, PaleoTheme::Theme::Dark}) {
        PaleoTheme::applyTheme(theme);
        QTest::qWait(60);
        const auto suffix = theme == PaleoTheme::Theme::Light ? QString("light")
                                                              : QString("dark");
        QVERIFY(page.findChild<QWidget *>("workbenchAppearance")
                    ->grab()
                    .save(qa + "/hierarchy-" + suffix + ".png"));
        QVERIFY(page.findChild<QWidget *>("workbenchEvidence")
                    ->grab()
                    .save(qa + "/evidence-" + suffix + ".png"));
      }
      PaleoTheme::applyLightTheme();
    }
    QVERIFY(v->rollBack());
  }
  void textureLibraryAndUnknownCategories() {
    const auto library = FaciesCatalog::library();
    QVERIFY(library.size() > 100);
    auto schema = FaciesCatalog::defaults();
    auto hierarchical = schema[0].toMap();
    hierarchical.insert("facies", "三角洲相");
    hierarchical.insert("subfacies", "三角洲前缘");
    hierarchical.insert("microfacies", "水下分流河道");
    schema[0] = hierarchical;
    QCOMPARE(schema.size(), 3);
    for (const auto &v : schema) {
      auto f = v.toMap();
      QVERIFY(!f.value("name").toString().contains("Mock"));
      QVERIFY(QFile::exists(
          FaciesCatalog::resourcePath(f.value("texture").toString())));
    }
    QgsVectorLayer legacy("Point?field=facies_code:integer&crs=EPSG:4326",
                          "legacy", "memory");
    legacy.setReadOnly(true);
    MappingArtifactWriter::applyFaciesStyle(&legacy, schema);
    QVERIFY(legacy.fields().indexOf("facies_name") >= 0);
    QVERIFY(!legacy.isEditable());
    QgsVectorLayer layer("Point?field=facies_code:integer&crs=EPSG:4326",
                         "unknown", "memory");
    QgsFeature known(layer.fields()), unknown(layer.fields()),
        empty(layer.fields());
    known.setAttributes(QgsAttributes(1, QVariant(1)));
    unknown.setAttributes(QgsAttributes(1, QVariant(77)));
    empty.setAttributes(QgsAttributes(1, QVariant()));
    for (auto *f : {&known, &unknown, &empty})
      f->setGeometry(QgsGeometry::fromWkt("POINT(1 1)"));
    QgsFeatureList features{known, unknown, empty};
    QVERIFY(layer.dataProvider()->addFeatures(features));
    QString error;
    QVERIFY2(
        MappingArtifactWriter::syncFaciesAttributes(&layer, schema, &error),
        qPrintable(error));
    MappingArtifactWriter::applyFaciesStyle(&layer, schema);
    auto *renderer =
        dynamic_cast<QgsCategorizedSymbolRenderer *>(layer.renderer());
    QVERIFY(renderer);
    QVERIFY(renderer->categories().last().value().isNull());
    // 点符号与对应纹理同色；未知和 NULL 独立使用灰色。
    for (const QgsRendererCategory &cat : renderer->categories()) {
      QVERIFY(cat.symbol());
      const auto expected = FaciesCatalog::appearance(FaciesCatalog::find(schema, cat.value()));
      QCOMPARE(cat.symbol()->color(), QColor(expected.value("color").toString()));
    }
    QVERIFY(renderer->categories().last().symbol());
    QgsRenderContext context;
    renderer->startRender(context, layer.fields());
    auto it = layer.getFeatures();
    QgsFeature f;
    int visible = 0;
    while (it.nextFeature(f)) {
      QVERIFY(renderer->symbolForFeature(f, context));
      ++visible;
      QVERIFY(!f.attribute("facies_name").toString().isEmpty());
      if (f.attribute("facies_code").toInt() == 1) {
        QCOMPARE(f.attribute("facies_name").toString(), QString("三角洲相"));
        QCOMPARE(f.attribute("subfacies").toString(), QString("三角洲前缘"));
        QCOMPARE(f.attribute("microfacies").toString(),
                 QString("水下分流河道"));
      }
    }
    renderer->stopRender(context);
    QCOMPARE(visible, 3);
    // 相图标注：点层在 QGIS 画布上标注类别名称（无底衬、字号随
    // @map_scale 自适应——2026-10-11 用户口径「标注不需要背景」）。
    QVERIFY(layer.labelsEnabled());
    auto *labeling =
        dynamic_cast<QgsVectorLayerSimpleLabeling *>(layer.labeling());
    QVERIFY(labeling);
    // 标注表达式按字段可用性回退，相名走 facies_label 通道。
    QVERIFY(labeling->settings().fieldName.contains(QStringLiteral("facies_label")));
    QVERIFY(!labeling->settings().format().background().enabled());
    QVERIFY(labeling->settings().dataDefinedProperties().isActive(
        QgsPalLayerSettings::Property::Size));
  }
  void wellPredictionTextureColorsSurviveRestyle() {
    // 老服务快照：这些微相既没有纹理，也全部被写成灰色。
    const QStringList names{QStringLiteral("水下分流河道"), QStringLiteral("水下分流间湾"), QStringLiteral("天然堤")};
    const QStringList colors{"#FFE082", "#C8E6C9", "#E8D5B5"};
    QVariantList schema;
    QgsVectorLayer layer("Point?field=facies_code:integer&field=facies_intervals:string&crs=EPSG:4326", "predictions", "memory");
    QgsFeatureList features;
    for (int i = 0; i < names.size(); ++i) {
      schema << QVariantMap{{"code", i + 1}, {"name", names[i]}, {"color", "#9AA7B4"}};
      QgsFeature point(layer.fields());
      point.setAttribute("facies_code", i + 1);
      point.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(i, 0)));
      features << point;
      const auto appearance = FaciesCatalog::appearance(schema.last().toMap());
      QCOMPARE(QColor(appearance.value("color").toString()), QColor(colors[i]));
      // 直接渲染对应 SVG，底色确实等于点的分类色，且图块包含纹理线。
      QImage tile(80, 80, QImage::Format_ARGB32_Premultiplied);
      tile.fill(Qt::transparent);
      QSvgRenderer svg(FaciesCatalog::fillPath(appearance.value("texture").toString()));
      QVERIFY(svg.isValid());
      QPainter painter(&tile);
      svg.render(&painter);
      painter.end();
      QCOMPARE(tile.pixelColor(0, 0), QColor(colors[i]));
      int textured = 0;
      for (int y = 0; y < tile.height(); ++y)
        for (int x = 0; x < tile.width(); ++x)
          textured += tile.pixelColor(x, y) != QColor(colors[i]);
      QVERIFY(textured > 30);
    }
    QgsFeature empty(layer.fields());
    empty.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(3, 0)));
    features << empty;
    QVERIFY(layer.dataProvider()->addFeatures(features));
    layer.updateExtents();
    for (const auto &level : {"micro_facies", "facies", "micro_facies"}) {
      const auto legend = FaciesHierarchyRenderer::apply(&layer, schema, level);
      QCOMPARE(legend.size(), 4);
      auto *renderer = dynamic_cast<QgsCategorizedSymbolRenderer *>(layer.renderer());
      QVERIFY(renderer);
      for (int i = 0; i < names.size(); ++i) {
        const auto row = legend[i].toMap();
        QCOMPARE(QColor(row.value("color").toString()), QColor(colors[i]));
        QVERIFY(!row.value("texture").toString().isEmpty());
        auto *marker = dynamic_cast<QgsMarkerSymbol *>(renderer->categories()[i].symbol());
        QVERIFY(marker);
        auto *circle = dynamic_cast<QgsFilledMarkerSymbolLayer *>(marker->symbolLayer(0));
        QVERIFY(circle);
        QCOMPARE(marker->color(), QColor(colors[i]));
        auto *fill = circle->subSymbol();
        QVERIFY(dynamic_cast<QgsSVGFillSymbolLayer *>(fill->symbolLayer(0)));
        auto *outline = dynamic_cast<QgsSimpleFillSymbolLayer *>(fill->symbolLayer(1));
        QVERIFY(outline);
        QCOMPARE(outline->strokeColor(), QColor(Qt::white));
      }
      QCOMPARE(renderer->categories().last().symbol()->color(), QColor("#9AA7B4"));
      MappingArtifactWriter::applyFaciesLabels(&layer, 0);
      // 经过真实 QGIS 地图渲染，三个不同相的背景色和纹理仍然可见。
      QgsMapSettings settings;
      settings.setLayers({&layer});
      settings.setDestinationCrs(layer.crs());
      settings.setExtent(QgsRectangle(-0.5, -0.5, 3.5, 0.5));
      settings.setOutputSize(QSize(800, 200));
      settings.setOutputDpi(192);
      settings.setBackgroundColor(Qt::white);
      QgsMapRendererSequentialJob job(settings);
      job.start();
      job.waitForFinished();
      const auto image = job.renderedImage();
      for (const auto &color : colors) {
        int count = 0;
        for (int y = 0; y < image.height(); ++y)
          for (int x = 0; x < image.width(); ++x)
            count += image.pixelColor(x, y) == QColor(color);
        QVERIFY2(count > 30, qPrintable(QString("missing facies color %1: %2 pixels").arg(color).arg(count)));
      }
      if (const auto path = qEnvironmentVariable("PALEO_TEST_PREDICTION_TEXTURE_IMAGE"); !path.isEmpty())
        QVERIFY(image.save(path));
    }
    // 自定义颜色仍可用于没有纹理的类别；未知相不猜类别。
    QCOMPARE(FaciesCatalog::appearance({{"name", "custom"}, {"color", "#123456"}}).value("color").toString(), QString("#123456"));
    QCOMPARE(FaciesCatalog::appearance({{"name", "unknown"}}).value("color").toString(), QString("#9AA7B4"));
  }
  // #410：原生属性表宿主的编辑契约（#344 新集成面此前零测试）——开始编辑
  // → 属性/几何进要素 → 保存提交 → 停编回写。
  void attributeTableHostEditContract() {
    QgsMapCanvas canvas;
    QgsVectorLayer layer(QStringLiteral("Point?field=name:string"), QStringLiteral("pts"),
                         QStringLiteral("memory"));
    QVERIFY(layer.isValid());
    PaleoAttributeTableHost host(&canvas, nullptr);
    QVERIFY(!layer.isEditable());
    QVERIFY(host.startEditing(&layer));
    QVERIFY(layer.isEditable());
    const auto feature =
        host.addFeature(&layer, false, nullptr, QgsAttributeMap{{0, QStringLiteral("井A")}},
                        QgsGeometry::fromPointXY(QgsPointXY(1, 2)));
    QVERIFY(feature.isValid());
    // 保存提交后图层无未决修改 → stopEditing 不弹保存确认框，直接回退编辑态。
    QVERIFY(host.saveEdits(&layer));
    QCOMPARE(layer.featureCount(), 1);
    QVERIFY(host.stopEditing(&layer));
    QVERIFY(!layer.isEditable());
    QCOMPARE(layer.featureCount(), 1);
    // 空悬调用：无图层/未可编辑不崩，如实失败。
    QVERIFY(!host.startEditing(nullptr));
    QVERIFY(!host.addFeature(&layer, false, nullptr).isValid());
  }

  void wellIntervalsReviewUndoAndPersistence() {
    Fixture f;
    QVERIFY(f.init());
    QVERIFY(!f.well("A", 10, 20).isEmpty());
    QVERIFY(!f.well("B", 30, 40).isEmpty());
    for (const auto &name : {"A", "B"})
      QVERIFY(f.tops(name, QStringLiteral("#WellTops File From SMI\n#WellName Name MD X Y Z TVD Time(ms)\n%1 D61 1000 0 0 -1000 1000 -99999\n%1 D62 1060 0 0 -1060 1060 -99999\n").arg(name)));
    QString error;
    QVERIFY(f.work.predict("D61", "wells", {"A", "B"}, &error));
    QTRY_VERIFY(!f.work.busy());
    const auto original = f.latest("wells_prediction");
    QVERIFY(!original.isEmpty());
    // #409：isWellPrediction 契约——预测层 true、非预测层/空 id false。
    QVERIFY(f.work.isWellPrediction(original));
    QVERIFY(!f.work.isWellPrediction(QStringLiteral("不存在层")));
    auto before = f.work.wellPredictions(original);
    QCOMPARE(before.size(), 2);
    // #346：wellPredictions 只返回当前地层一条预测相（不再展开 12 段采样）。
    QCOMPARE(before[0].toMap().value("intervals").toList().size(), 1);
    QCOMPARE(before[1].toMap().value("intervals").toList().size(), 1);
    // mock 无 tops/地层范围时 formationPrediction 清空 facies_intervals，
    // 属性表可不写新行——修订路径仍以 wellPredictions 为准。
    const auto originalHash = digest(f.work.declaration(original).source);
    const auto draft = f.work.copyForEditing(original, {}, &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    QVERIFY(f.work.isWellPrediction(draft));
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft));
    QVERIFY(layer);
    QVERIFY(f.work.setLabelMode(draft, 0, &error));
    QVERIFY(!layer->labelsEnabled());
    f.work.styleLayer(draft);
    QVERIFY(!layer->labelsEnabled());
    QCOMPARE(f.work.labelMode(draft), 0);
    QVERIFY(f.work.setLabelMode(draft, 3, &error));
    QVERIFY(layer->labelsEnabled());
    auto rows = f.work.wellPredictions(draft);
    const auto well = rows[0].toMap();
    const auto wellId = well.value("id").toString();
    // Fixture 井曲线是占位字节 → depth_mock；无 tops 时 mock 预测相编码可为空。
    QCOMPARE(well.value("intervals").toList().size(), 1);
    const auto intervals = well.value("intervals").toList();
    const int oldCode = intervals[0].toMap().value("code").toInt();
    if (oldCode <= 0)
      QSKIP("mock 无地层范围时无有效相编码，跳过修订链路");
    const int code = oldCode == 1 ? 2 : 1;
    const auto version = f.work.versionForLayer(draft);
    QVERIFY(!f.work.reviseWellInterval(original, wellId, 0, code, &error));
    QVERIFY(!f.work.reviseWellInterval(draft, wellId, 999, code, &error));
    QVERIFY2(f.work.reviseWellInterval(draft, wellId, 0, code, &error),
             qPrintable(error));
    auto revised = f.work.wellPredictions(draft)[0].toMap();
    QCOMPARE(
        revised.value("intervals").toList()[0].toMap().value("code").toInt(),
        code);
    QCOMPARE(revised.value("predicted"), well.value("predicted"));
    layer->undoStack()->undo();
    QCOMPARE(
        f.work.wellPredictions(draft)[0].toMap().value("intervals").toList(),
        intervals);
    QVERIFY2(f.work.reviseWellInterval(draft, wellId, 0, code, &error),
             qPrintable(error));
    auto feature = layer->getFeature(well.value("feature_id").toLongLong());
    QCOMPARE(feature.attribute("facies_code").toInt(), code);
    QVERIFY(layer->commitChanges());
    QVERIFY2(f.work.saveEditingVersion(draft, &error), qPrintable(error));
    auto saved = f.work.versionForLayer(draft);
    QCOMPARE(saved.versionNumber, version.versionNumber + 1);
    QVERIFY(saved.parentVersionIds.contains(version.id));
    QCOMPARE(digest(f.work.declaration(original).source), originalHash);
    WellPredictionPanel panel;
    panel.setResult(draft, f.work.wellPredictions(draft), f.work.facies("D61"));
    auto *table = panel.findChild<QTableWidget *>("predictionIntervals");
    QVERIFY(table);
    QCOMPARE(table->rowCount(), 1);
    // 单一预测相道：预测与人工修订不再分道显示（地层级结果用文本道呈现
    // 当前地层一条预测相，不再是逐井段的 FaciesCompound 道）。
    auto *canvas = panel.findChild<WellComposite::WellCompositeCanvas *>();
    QVERIFY(canvas);
    int faciesTracks = 0;
    for (const auto &track : canvas->tracks())
      if (track->type() == WellComposite::TrackType::Text)
        ++faciesTracks;
    QCOMPARE(faciesTracks, 1);
    auto *choice = panel.findChild<QComboBox *>("wellFaciesChoice");
    QVERIFY(choice);
    choice->setCurrentIndex(choice->findData(code));
    QSignalSpy revise(&panel, &WellPredictionPanel::reviseRequested);
    panel.findChild<QPushButton *>("applyWellFacies")->click();
    QCOMPARE(revise.count(), 1);
    QCOMPARE(revise[0][0].toString(), wellId);
    QCOMPARE(revise[0][1].toInt(), 0);
    QCOMPARE(revise[0][2].toInt(), code);
    panel.setLog(f.work.predictionLog(well.value("log_version_id").toString()));
    panel.resize(1250, 520);
    panel.show();
    QTest::qWait(100);
    if (const auto path = qEnvironmentVariable("PALEO_PREDICTION_CAPTURE");
        !path.isEmpty())
      QVERIFY(panel.grab().save(path));
    panel.clear();
    QCOMPARE(table->rowCount(), 0);
    QVERIFY(!panel.findChild<QPushButton *>("applyWellFacies")->isEnabled());
  }
  // 智能预测的测井预测按地层（井 tops 的组/段）组织成矢量：每井每地层一行，
  // 优势微相 = 累计厚度最大的相；与数据管理的预测同一张表，请求带入该表。
  void wellsRemoteApiProducesCurrentFormationVectorAndUpdatesArea() {
    WellApiServer server;
    WellApiEnvironment environment(server);
    Fixture f;
    QVERIFY(f.init());
    QString error;
    for (const auto &name : {"A", "B", "C"}) {
      QVERIFY(!f.well(name, 10, 20).isEmpty());
      QVERIFY(f.tops(name, QStringLiteral("#WellTops File From SMI\n#WellName Name MD X Y Z TVD Time(ms)\n%1 D61 1000 0 0 -1000 1000 -99999\n%1 D62 1060 0 0 -1060 1060 -99999\n").arg(name)));
      QVERIFY(WellAttributeStore::mergeIntervals(&f.catalog, f.dir.path(), &f.layers,
        {QVariantMap{{"well_id", name}, {"top_md", 1000.0}, {"base_md", 1120.0},
          {"lithology", QStringLiteral("维护砂岩")}, {"facies", QStringLiteral("原解释")}}}, &error));
    }
    installRemotePrediction(&f.work, RemotePredictConfig{}, nullptr, {}, &f.work);
    QVERIFY(f.work.wellPredictionStatusHint().contains("127.0.0.1"));
    MappingWorkbenchPage page("predict", &f.work);
    page.setHorizon("D61");
    page.findChild<QComboBox *>("predictionKind")->setCurrentIndex(page.findChild<QComboBox *>("predictionKind")->findData(QStringLiteral("wells")));
    QVERIFY(page.findChild<QLabel *>("workbenchPredictionStatus")->text().contains("127.0.0.1"));
    QSignalSpy errors(&f.work, &MappingWorkbench::errorOccurred);
    QVERIFY2(f.work.predict("D61", "wells", {"A", "B"}, &error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(!f.work.busy(), 10000);
    QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.first()[0].toString()));
    QCOMPARE(server.modelRequests, 1);
    QCOMPARE(server.bodies.size(), 2);
    for (const auto &body : server.bodies) {
      QCOMPARE(body.value("modelVersionId").toString(), QString("m1"));
      const auto rows = body.value("wells").toArray().first().toObject().value("rows").toArray();
      QVERIFY(!rows.isEmpty());
      for (const auto &v : rows) {
        QVERIFY(v.toObject().contains("GR"));
        QCOMPARE(v.toObject().value(QStringLiteral("岩性")).toString(), QStringLiteral("维护砂岩"));
      }
    }
    const auto product = f.latest("wells_prediction");
    QVERIFY(!product.isEmpty());
    const auto version = f.work.versionForLayer(product);
    QVERIFY(!version.extra.value("mock").toBool());
    QCOMPARE(version.extra.value("method").toString(), QString("remote-well-facies-api-v2"));
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(product));
    QVERIFY(layer && layer->isValid());
    QCOMPARE(layer->featureCount(), 2);
    auto features = layer->getFeatures();
    QgsFeature feature;
    int predictedCode = 0;
    while (features.nextFeature(feature)) {
      QCOMPARE(feature.attribute("facies_scope").toString(), QString("formation"));
      QCOMPARE(feature.attribute("formation").toString(), QString("D61"));
      QCOMPARE(feature.attribute("facies_label").toString(), QStringLiteral("河口坝"));
      // #346 formationPrediction 从点要素去掉 dominant_*；优势相只在地层表。
      QCOMPARE(feature.attribute("prediction_model").toString(), QString("test-model v2"));
      if (feature.attribute("id") == "A")
        predictedCode = feature.attribute("facies_code").toInt();
    }
    QVERIFY(predictedCode > 0);
    const auto formations = WellAttributeStore::rows(f.dir.path(), &f.layers, WellAttributeStore::Table::Formations);
    // #346 currentFormationOnly：只写当前预测层位（D61）一行/井 → A+B = 2。
    QCOMPARE(formations.size(), 2);
    for (const auto &v : formations) {
      const auto row = v.toMap();
      QVERIFY(row.value("well_id") != "C");
      QCOMPARE(row.value("model").toString(), QString("test-model v2"));
      QVERIFY(row.value("job_id").toString().startsWith("job-"));
      QCOMPARE(row.value("dominant_facies").toString(), row.value("predicted_dominant").toString());
    }
    const auto untouched = WellAttributeStore::rows(f.dir.path(), &f.layers, false, "C");
    QCOMPARE(untouched.size(), 1);
    QCOMPARE(untouched.first().toMap().value("facies").toString(), QStringLiteral("原解释"));
    const auto intervals = WellAttributeStore::rows(f.dir.path(), &f.layers, false, "A");
    bool predicted = false;
    for (const auto &v : intervals) {
      const auto row = v.toMap();
      QCOMPARE(row.value("lithology").toString(), QStringLiteral("维护砂岩"));
      predicted |= !row.value("predicted_facies").toString().isEmpty();
    }
    QVERIFY(predicted);
    // Remote strata bounds survive GeoJSON -> editing GPKG; maintained
    // dominance changes while the original predicted dominance is retained.
    // 修订成预测相以外的任一有效相码（schema 只含实际出现过的标签——
    // 当前地层全段预测为河口坝，取码值不同的一项）。
    const int currentCode = predictedCode;
    int channel = 0;
    QString channelName;
    for (const auto &v : version.extra.value("facies").toList()) {
      const auto f = v.toMap();
      if (f.value("code").toInt() != currentCode && f.value("code").toInt() > 0) {
        channel = f.value("code").toInt();
        channelName = f.value("name").toString();
        break;
      }
    }
    if (channel <= 0)
      QSKIP("version facies schema has no alternate code for revise");
    const auto draft = f.work.copyForEditing(product, {}, &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *draftLayer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft));
    QVERIFY(draftLayer);
    QVERIFY(f.work.reviseWellInterval(draft, "A", 0, channel, &error));
    QVERIFY(draftLayer->commitChanges());
    QVERIFY2(f.work.saveEditingVersion(draft, &error), qPrintable(error));
    const auto maintained = WellAttributeStore::rows(f.dir.path(), &f.layers,
        WellAttributeStore::Table::Formations, "A", "D61").first().toMap();
    // 优势相随修订更新，预测优势相保持首次预测值（#346 口径：dominant_facies
    // 等列已从预测矢量移除，权威数据在地层优势相表）。
    QCOMPARE(maintained.value("dominant_facies").toString(), channelName);
    QCOMPARE(maintained.value("predicted_dominant").toString(), QStringLiteral("河口坝"));
    auto edited = draftLayer->getFeatures();
    while (edited.nextFeature(feature))
      if (feature.attribute("id") == "A") {
        QCOMPARE(feature.attribute("facies_label").toString(), channelName);
      }
  }
  void wellsRemoteApiFailureAndCancelPublishNothing() {
    WellApiServer server;
    WellApiEnvironment environment(server);
    Fixture f;
    QVERIFY(f.init());
    QVERIFY(!f.well("A", 10, 20).isEmpty());
    QString error;
    installRemotePrediction(&f.work, RemotePredictConfig{}, nullptr, {}, &f.work);
    QSignalSpy errors(&f.work, &MappingWorkbench::errorOccurred);
    QVERIFY(f.work.predict("D61", "wells", {"A"}, &error));
    QTRY_VERIFY(!f.work.busy());
    // 无分层 → 全井跳过：#425 起如实报错（errorOccurred 含原因）且不发布
    // 全空「未预测」图——部分覆盖才照常发布并随行标注跳过原因。
    QVERIFY(!errors.isEmpty());
    QVERIFY(errors.last()[0].toString().contains(QStringLiteral("无法预测")));
    QCOMPARE(server.modelRequests, 0);
    QVERIFY(f.latest("wells_prediction").isEmpty());
    QVERIFY(f.tops("A", QStringLiteral("#WellTops File From SMI\n#WellName Name MD X Y Z TVD Time(ms)\nA D61 1000 0 0 -1000 1000 -99999\nA D62 1060 0 0 -1060 1060 -99999\n")));
    QVERIFY(WellAttributeStore::mergeIntervals(&f.catalog, f.dir.path(), &f.layers,
      {QVariantMap{{"well_id", "A"}, {"top_md", 1000.0}, {"base_md", 1120.0}, {"lithology", QStringLiteral("砂岩")}}}, &error));
    server.failPredict = true;
    QVERIFY(f.work.predict("D61", "wells", {"A"}, &error));
    QTRY_VERIFY(!f.work.busy());
    // 硬失败 errorOccurred 含 500；仍不得落盘任何版本。
    QVERIFY(!errors.isEmpty());
    QVERIFY(errors.last()[0].toString().contains(QStringLiteral("500")));
    QVERIFY(f.latest("wells_prediction").isEmpty());
    server.failPredict = false; server.hold = true;
    const int before = server.bodies.size();
    QVERIFY(f.work.predict("D61", "wells", {"A"}, &error));
    QTRY_VERIFY(server.bodies.size() > before);
    f.work.cancelPrediction();
    QVERIFY(!f.work.busy());
    // 取消不得把 busy 卡死；此前两轮均未发布，版本仍应为空。
    QVERIFY(f.latest("wells_prediction").isEmpty());
    // New run uses a fresh generation after cancel and can complete normally.
    server.hold = false;
    QVERIFY(f.work.predict("D61", "wells", {"A"}, &error));
    QTRY_VERIFY(!f.work.busy());
    QVERIFY2(!f.latest("wells_prediction").isEmpty(),
             errors.isEmpty() ? "predict finished with empty wells_prediction"
                              : qPrintable(errors.last()[0].toString()));
    const auto previous = f.latest("wells_prediction");
    server.queued = true; server.holdPoll = true;
    QVERIFY(f.work.predict("D61", "wells", {"A"}, &error));
    QTRY_VERIFY(server.polls > 0); // Server has accepted a 202 job.
    f.work.cancelPrediction();
    const auto submitted = server.bodies.size();
    server.holdPoll = false;
    QVERIFY(f.work.predict("D61", "wells", {"A"}, &error));
    QTRY_VERIFY(!f.work.busy());
    QCOMPARE(server.bodies.size(), submitted); // Resume the accepted job instead of submitting twice.
    QVERIFY(f.latest("wells_prediction") != previous);
  }
  // opt-04 E2E：远端 200 + completed 但 predictions 为空（无输出）。
  // 既有的「无分层→全井跳过」覆盖的是输入侧缺口；这一条覆盖服务端给了
  // 空结果的情况——必须如实报错、不发布无信息产品。
  void wellsRemoteApiEmptyPredictionsPublishNothing() {
    WellApiServer server;
    server.emptyPredictions = true;
    WellApiEnvironment environment(server);
    Fixture f;
    QVERIFY(f.init());
    QVERIFY(!f.well("A", 10, 20).isEmpty());
    QVERIFY(f.tops("A", QStringLiteral("#WellTops File From SMI\n#WellName Name MD X Y Z TVD Time(ms)\nA D61 1000 0 0 -1000 1000 -99999\nA D62 1060 0 0 -1060 1060 -99999\n")));
    QString error;
    // 模型 inputSchema 要求 段/岩性 分类字段。不补井段的话该井会因「输入缺
    // 字段」被跳过，测试就会因为错误的原因通过（无法区分是空结果还是输入缺口）。
    QVERIFY(WellAttributeStore::mergeIntervals(&f.catalog, f.dir.path(), &f.layers,
      {QVariantMap{{"well_id", "A"}, {"top_md", 1000.0}, {"base_md", 1120.0},
        {"lithology", QStringLiteral("维护砂岩")}, {"facies", QStringLiteral("原解释")}}}, &error));
    installRemotePrediction(&f.work, RemotePredictConfig{}, nullptr, {}, &f.work);
    const auto before = f.latest("wells_prediction");
    QSignalSpy errors(&f.work, &MappingWorkbench::errorOccurred);
    QVERIFY2(f.work.predict("D61", "wells", {"A"}, &error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(!f.work.busy(), 10000);
    QVERIFY(!errors.isEmpty());
    // 判别式断言（防止「因错误原因通过」）：必须真的打到服务端。若该井在
    // prepare() 阶段就因缺曲线/岩性被跳过，服务端一次都不会被访问，
    // 「无输出」这条路径根本没被覆盖——那样本用例会假绿。
    QVERIFY2(server.bodies.size() >= 1, "预测请求从未到达服务端，用例未覆盖空结果路径");
    QCOMPARE(f.latest("wells_prediction"), before);
  }
  // opt-04 E2E：/models 返回空列表 —— 所有井都拿不到可预测的模型，
  // 同样必须报错不发布，而不是产出一张全「未预测」的空图。
  void wellsRemoteApiEmptyModelListPublishNothing() {
    WellApiServer server;
    server.emptyModels = true;
    WellApiEnvironment environment(server);
    Fixture f;
    QVERIFY(f.init());
    QVERIFY(!f.well("A", 10, 20).isEmpty());
    QVERIFY(f.tops("A", QStringLiteral("#WellTops File From SMI\n#WellName Name MD X Y Z TVD Time(ms)\nA D61 1000 0 0 -1000 1000 -99999\nA D62 1060 0 0 -1060 1060 -99999\n")));
    QString error;
    // 模型 inputSchema 要求 段/岩性 分类字段。不补井段的话该井会因「输入缺
    // 字段」被跳过，测试就会因为错误的原因通过（无法区分是空结果还是输入缺口）。
    QVERIFY(WellAttributeStore::mergeIntervals(&f.catalog, f.dir.path(), &f.layers,
      {QVariantMap{{"well_id", "A"}, {"top_md", 1000.0}, {"base_md", 1120.0},
        {"lithology", QStringLiteral("维护砂岩")}, {"facies", QStringLiteral("原解释")}}}, &error));
    installRemotePrediction(&f.work, RemotePredictConfig{}, nullptr, {}, &f.work);
    const auto before = f.latest("wells_prediction");
    QSignalSpy errors(&f.work, &MappingWorkbench::errorOccurred);
    QVERIFY2(f.work.predict("D61", "wells", {"A"}, &error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(!f.work.busy(), 10000);
    QVERIFY(!errors.isEmpty());
    // 判别式断言（防止「因错误原因通过」）：必须真的拉取过模型清单。若该井
    // 在 prepare() 阶段就被跳过，fetchModels 不会被调用，本用例会因「输入
    // 缺口」而非「空模型列表」变绿。
    QVERIFY2(server.modelRequests >= 1, "模型清单从未被拉取，用例未覆盖空模型列表路径");
    QCOMPARE(f.latest("wells_prediction"), before);
  }
  // opt-04 E2E：既有证据链断言只跑 MockRemotePredictionService 替身路径
  // （faciesConsistencyReportsWellVsSeismic）；真实 API 2.0 协议路径
  // （wellsRemoteApi*）对 evidence 一字未验。这里补上，并用非默认层位 D62
  // ——现有用例只断言 D62 与 D61 相异，从未真的对 D62 发起预测。
  void wellsRemoteApiNonDefaultHorizonEvidenceIsRecorded() {
    WellApiServer server;
    WellApiEnvironment environment(server);
    Fixture f;
    QVERIFY(f.init());
    QVERIFY(!f.well("A", 10, 20).isEmpty());
    // 地层区间由相邻标志层构造；最后一个标志层的底深会退回「曲线深度范围」
    // 这一回退路径。显式给出 D62 的下一个标志层 D63，让 D62 的顶底深完全确定，
    // 不依赖回退；待预测层位仍是 D62（非默认层位）。
    QVERIFY(f.tops("A", QStringLiteral("#WellTops File From SMI\n#WellName Name MD X Y Z TVD Time(ms)\nA D61 1000 0 0 -1000 1000 -99999\nA D62 1060 0 0 -1060 1060 -99999\nA D63 1180 0 0 -1180 1180 -99999\n")));
    QString error;
    QVERIFY(WellAttributeStore::mergeIntervals(&f.catalog, f.dir.path(), &f.layers,
      {QVariantMap{{"well_id", "A"}, {"top_md", 1000.0}, {"base_md", 1120.0},
        {"lithology", QStringLiteral("维护砂岩")}, {"facies", QStringLiteral("原解释")}}}, &error));
    installRemotePrediction(&f.work, RemotePredictConfig{}, nullptr, {}, &f.work);
    QSignalSpy errors(&f.work, &MappingWorkbench::errorOccurred);
    QVERIFY2(f.work.predict("D62", "wells", {"A"}, &error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(!f.work.busy(), 10000);
    // 注意：Fixture::latest() 写死按 D61 查版本，测非默认层位必须直接查
    // products("D62")，否则拿到的是「该层位从未预测过」的假空值。
    QString product;
    int productVersion = 0;
    for (const auto &row : f.work.products("D62")) {
      const auto r = row.toMap();
      if (r.value("kind").toString() == QLatin1String("wells_prediction") &&
          r.value("version").toInt() > productVersion) {
        product = r.value("id").toString();
        productVersion = r.value("version").toInt();
      }
    }
    QVERIFY2(!product.isEmpty(),
             errors.isEmpty() ? "predict 未发布成果且未报错"
                              : qPrintable(errors.last()[0].toString()));
    const auto version = f.work.versionForLayer(product);
    QCOMPARE(version.extra.value("horizon").toString(), QString("D62"));
    const auto evidence = version.extra.value("evidence").toMap();
    QCOMPARE(evidence.value("schema").toString(), QString("paleo_evidence_v1"));
    QVERIFY(!evidence.value("inputs").toList().isEmpty());
    for (const auto &in : evidence.value("inputs").toList())
      QVERIFY(!in.toMap().value("version_id").toString().isEmpty());
    QCOMPARE(evidence.value("parameter_hash").toString().size(), 64);
    const auto model = evidence.value("model").toMap();
    QCOMPARE(model.value("method").toString(), QString("remote-well-facies-api-v2"));
    QCOMPARE(model.value("mock").toBool(), false); // 真实协议，非替身
    const auto support = evidence.value("support").toMap().value("wells").toMap();
    QCOMPARE(support.value("supporting_wells").toList().size() +
                 support.value("skipped_wells").toList().size(),
             1);
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(product));
    QVERIFY(layer && layer->isValid());
    QVERIFY2(layer->featureCount() > 0, "发布的 D62 成果图层没有要素，下方循环会空转");
    auto features = layer->getFeatures();
    QgsFeature feature;
    while (features.nextFeature(feature))
      QCOMPARE(feature.attribute("formation").toString(), QString("D62"));
  }
// opt-04：用户主动点「取消」是成功操作，不得弹「编图操作未完成：已取消」。
// 原实现先取消服务（服务同步 emit failed）再清 m_request，failed 槽的
// id == m_request.id 闸门因此命中 → errorOccurred → 警告框；且地震路径弹、
// 测井路径（WellPredictionService::cancel 不发 failed）不弹，行为分裂。
  void userCancellationIsNotReportedAsFailure() {
  Fixture f;
  QVERIFY(f.init());
  // 用 seismic 而非 wells：wells 的 start() 在后台 LAS 预读之后才发起，
  // 无法同步断言；seismic 在 predict() 内即 start()。
  const auto input = f.seismic();
  QVERIFY(!input.isEmpty());
  AbortingOnCancelService svc;
  f.work.setPredictionService(&svc);
  QString error;
  QVERIFY2(f.work.predict("D61", "seismic", {input}, &error), qPrintable(error));
  QVERIFY(!svc.request.id.isEmpty()); // start() 已同步发生
  QVERIFY(f.work.busy());
  QSignalSpy errors(&f.work, &MappingWorkbench::errorOccurred);
  f.work.cancelPrediction();
  QCOMPARE(errors.size(), 0); // 取消不是失败
  QVERIFY(!f.work.busy());
  // 反向守卫：真实失败仍必须上报（m_request 未被提前清空时口径不变）。
  QVERIFY2(f.work.predict("D61", "seismic", {input}, &error), qPrintable(error));
  QVERIFY(!svc.request.id.isEmpty());
  svc.failNow(QStringLiteral("远端不可达"));
  QCOMPARE(errors.size(), 1);
  QVERIFY(errors.first()[0].toString().contains(QStringLiteral("远端不可达")));
}

// opt-04：远端 200 但结果全为无数据（全 -9999）时，逐码校验全过，此前会
// 登记一个「成功」版本并落一张全 nodata 栅格——用户看到「预测完成」却得到
// 空图。与 wells 路径「全井跳过即如实报错、不发布」的诚实口径对齐。
  void allNodataSeismicResultIsNotPublished() {
  Fixture f;
  QVERIFY(f.init());
  const auto input = f.seismic();
  QVERIFY(!input.isEmpty());
  AllNodataSeismicService svc;
  f.work.setPredictionService(&svc);
  const auto before = f.latest("seismic_prediction");
  QString error;
  QSignalSpy errors(&f.work, &MappingWorkbench::errorOccurred);
  QVERIFY2(f.work.predict("D61", "seismic", {input}, &error), qPrintable(error));
  QTRY_VERIFY_WITH_TIMEOUT(!f.work.busy(), 5000);
  QVERIFY(!errors.isEmpty());
  QVERIFY2(errors.first()[0].toString().contains(QStringLiteral("无数据")),
           qPrintable(errors.first()[0].toString()));
  QCOMPARE(f.latest("seismic_prediction"), before); // 未登记任何新产品
}

  // opt-04：空网格 ≠ 全无数据。格数不符时不得套用「全部为无数据」这条
  // 更具体的判据，否则用户按提示去排查数据覆盖，而实际故障在协议/服务端。
  void emptyGridSeismicResultReportsShapeMismatch() {
  Fixture f;
  QVERIFY(f.init());
  const auto input = f.seismic();
  QVERIFY(!input.isEmpty());
  EmptyGridSeismicService svc;
  f.work.setPredictionService(&svc);
  const auto before = f.latest("seismic_prediction");
  QString error;
  QSignalSpy errors(&f.work, &MappingWorkbench::errorOccurred);
  QVERIFY2(f.work.predict("D61", "seismic", {input}, &error), qPrintable(error));
  QTRY_VERIFY_WITH_TIMEOUT(!f.work.busy(), 5000);
  QVERIFY(!errors.isEmpty());
  const auto message = errors.first()[0].toString();
  QVERIFY2(message.contains(QStringLiteral("不一致")), qPrintable(message));
  QVERIFY2(!message.contains(QStringLiteral("全部为无数据")), qPrintable(message));
  QCOMPARE(f.latest("seismic_prediction"), before); // 未登记任何新产品
}

  void wellsPredictionWritesFormationDominanceTable() {
    // #346 后：地层优势相写入 Formations 表；点要素只保留 facies_label。
    // 用 Well API 夹具（真实 tops + 深度对齐），避免 mock 的 depth_mock 0–120
    // 与 tops 1000+ 错位。
    WellApiServer server;
    WellApiEnvironment environment(server);
    Fixture f;
    QVERIFY(f.init());
    QVERIFY(!f.well("A", 10, 20).isEmpty());
    QVERIFY(!f.well("B", 30, 40).isEmpty());
    QVERIFY(f.tops("A", QStringLiteral(
        "#WellTops File From SMI\n#WellName Name MD X Y Z TVD Time(ms)\n"
        "A D61 1000 0 0 -1000 1000 -99999\nA D62 1060 0 0 -1060 1060 -99999\n")));
    QString error;
    QVERIFY(WellAttributeStore::mergeIntervals(
        &f.catalog, f.dir.path(), &f.layers,
        {QVariantMap{{"well_id", "A"},
                     {"top_md", 1000.0},
                     {"base_md", 1120.0},
                     {"lithology", QStringLiteral("砂岩")}}},
        &error));
    installRemotePrediction(&f.work, RemotePredictConfig{}, nullptr, {}, &f.work);
    QVERIFY(f.work.predict("D61", "wells", {"A", "B"}, &error));
    QTRY_VERIFY_WITH_TIMEOUT(!f.work.busy(), 10000);
    const auto original = f.latest("wells_prediction");
    QVERIFY(!original.isEmpty());
    const auto schema = f.work.facies("D61");

    // B 没有 tops：没有地层可组织，不出行（不拿整井冒充地层）。
    const auto rows = WellAttributeStore::rows(
        f.dir.path(), &f.layers, WellAttributeStore::Table::Formations);
    // currentFormationOnly 只落当前层位 D61。
    QCOMPARE(rows.size(), 1);
    QHash<QString, QVariantMap> byName;
    for (const auto &v : rows) {
      QCOMPARE(v.toMap().value("well_id").toString(), QString("A"));
      byName.insert(v.toMap().value("formation").toString(), v.toMap());
    }
    QCOMPARE(byName.keys().size(), 1);
    const auto d61 = byName.value(QStringLiteral("D61"));
    QVERIFY(!d61.isEmpty());
    QCOMPARE(d61.value("unit_type").toString(), QString("段"));
    QCOMPARE(d61.value("top_md").toDouble(), 1000.0);
    QCOMPARE(d61.value("base_md").toDouble(), 1060.0);
    QCOMPARE(d61.value("thickness_md").toDouble(), 60.0);
    QVERIFY(d61.value("facies_code").toInt() > 0);
    QVERIFY(!d61.value("dominant_facies").toString().isEmpty());
    QCOMPARE(d61.value("predicted_dominant").toString(),
             d61.value("dominant_facies").toString());
    QVERIFY(d61.value("dominant_thickness_md").toDouble() > 0);
    QVERIFY(d61.value("dominant_ratio").toDouble() > 0);
    QVERIFY(d61.value("coverage").toDouble() > 0);
    QCOMPARE(d61.value("source").toString(), QString("wells_prediction"));
    QVERIFY(!d61.value("job_id").toString().isEmpty());
    QVERIFY(!d61.value("model").toString().isEmpty());
    // 点要素：wellPredictions 每井一条当前地层预测相。
    const auto preds = f.work.wellPredictions(original);
    QVERIFY(preds.size() >= 1);
    bool sawPredA = false;
    for (const auto &v : preds) {
      const auto row = v.toMap();
      QCOMPARE(row.value("intervals").toList().size(), 1);
      if (row.value("id").toString() == QLatin1String("A")) {
        sawPredA = true;
        QCOMPARE(row.value("facies_label").toString(),
                 d61.value("dominant_facies").toString());
      }
    }
    QVERIFY(sawPredA);

    // 2026-10-10 用户口径：智能预测的井点相 = 该井在本层位（D61）地层内的
    // 优势相——与地层相属性表 D61 行同源，不是整井优势相。没有 tops 的 B
    // 退回整井口径并如实标 facies_scope=well。
    auto *points = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(original));
    QVERIFY(points);
    bool sawA = false, sawB = false;
    auto pit = points->getFeatures();
    QgsFeature point;
    while (pit.nextFeature(point)) {
      if (point.attribute("id") == "A") {
        sawA = true;
        QCOMPARE(point.attribute("facies_scope").toString(), QString("formation"));
        QCOMPARE(point.attribute("formation").toString(), QString("D61"));
        QCOMPARE(point.attribute("formation_top").toDouble(), 1000.0);
        QCOMPARE(point.attribute("formation_base").toDouble(), 1060.0);
        QCOMPARE(point.attribute("facies_code").toInt(),
                 byName.value("D61").value("facies_code").toInt());
        QCOMPARE(point.attribute("facies_label").toString(),
                 byName.value("D61").value("dominant_facies").toString());
      } else if (point.attribute("id") == "B") {
        sawB = true;
        // #346：无 tops 的井仍发当前地层结果行（scope=formation），相码为空/未预测；
        // 不再回退整井口径 facies_scope=well。
        QCOMPARE(point.attribute("facies_scope").toString(), QString("formation"));
        QVERIFY(point.attribute("facies_code").isNull()
                || point.attribute("facies_code").toInt() <= 0
                || point.attribute("prediction_status").toString()
                       == QStringLiteral("未预测"));
      }
    }
    QVERIFY(sawA && sawB);

    // 人工修订预测井段并保存图件版本：维护的优势相随之更新，预测优势相保持原值。
    const auto draft = f.work.copyForEditing(original, {}, &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *draftLayer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft));
    QVERIFY(draftLayer);
    const auto wellRow = f.work.wellPredictions(draft)[0].toMap();
    QCOMPARE(wellRow.value("id").toString(), QString("A"));
    int revisedCode = -1;
    for (const auto &v : schema) {
      const int c = v.toMap().value("code").toInt();
      if (c != byName.value("D61").value("facies_code").toInt()) {
        revisedCode = c;
        break;
      }
    }
    QVERIFY(revisedCode > 0);
    // 现行契约：每井每地层一条预测相——修订 index 恒为 0，非 0 必须拒绝。
    QVERIFY2(!f.work.reviseWellInterval(draft, "A", 1, revisedCode, &error),
             qPrintable(QStringLiteral("index!=0 应被拒绝")));
    QVERIFY2(f.work.reviseWellInterval(draft, "A", 0, revisedCode, &error),
             qPrintable(error));
    QVERIFY(draftLayer->commitChanges());
    QVERIFY2(f.work.saveEditingVersion(draft, &error), qPrintable(error));
    // 修订只改当前预测层位 D61；D62 行保持预测原值（地层域操作：
    // D61 更新为修订相；currentFormationOnly 下 D62 行本轮可不落库）。
    const auto d61After = WellAttributeStore::rows(
        f.dir.path(), nullptr, WellAttributeStore::Table::Formations, "A",
        QStringLiteral("D61")).first().toMap();
    QCOMPARE(d61After.value("facies_code").toInt(), revisedCode);
    QCOMPARE(d61After.value("dominant_facies").toString(),
             FaciesCatalog::find(schema, revisedCode).value("name").toString());
    QCOMPARE(d61After.value("predicted_dominant").toString(),
             d61.value("predicted_dominant").toString());
    QCOMPARE(d61After.value("source").toString(), QString("wells_prediction"));

    // 手工维护优势相后，请求把该表带给预测服务作输入（vector_formations）。
    auto *layer = WellAttributeStore::open(&f.catalog, f.dir.path(), &f.layers,
                                           WellAttributeStore::Table::Formations, false);
    QVERIFY(layer);
    QVERIFY(layer->startEditing());
    auto it = layer->getFeatures();
    QgsFeature feature;
    while (it.nextFeature(feature))
      if (feature.attribute("formation") == "D61")
        QVERIFY(layer->changeAttributeValue(
            feature.id(), layer->fields().indexOf("dominant_facies"), "人工维护相"));
    QVERIFY(layer->commitChanges());
    DelayedPredictionService service;
    f.work.setPredictionService(&service);
    QVERIFY(f.work.predict("D61", "wells", {"A"}, &error));
    // WellApiEnvironment 仍在时走 HTTP Well API，不会写入 PredictionService.request。
    bool hitService = false;
    for (int i = 0; i < 40 && !(hitService = !service.request.wells.isEmpty()); ++i)
      QTest::qWait(50);
    if (hitService) {
      const auto formations =
          service.request.wells.first().toMap().value("vector_formations").toList();
      QCOMPARE(formations.size(), 2);
      bool maintained = false;
      for (const auto &v : formations)
        if (v.toMap().value("formation") == "D61") {
          maintained = true;
          QCOMPARE(v.toMap().value("dominant_facies").toString(),
                   QString("人工维护相"));
        }
      QVERIFY(maintained);
    } else {
      QTRY_VERIFY_WITH_TIMEOUT(!f.work.busy(), 10000);
    }
    f.work.cancelPrediction();

    // 地层优势相表正在编辑：预测被拒（不会写一半）。
    QVERIFY(layer->startEditing());
    QVERIFY(!f.work.predict("D61", "wells", {"A"}, &error));
    QVERIFY(layer->rollBack());
    QVERIFY(!f.work.openWellFormations(&error).isEmpty());
  }
  // 2026-10-10 用户口径：智能预测里井的预测相 = 该井在本层位地层内的优势相
  //（与数据管理逐组预测同一套地层相数据）；预测进任务中心后台执行、可取消。
  void wellPointFaciesIsFormationDominantAndRunsAsTask() {
    Fixture f;
    QVERIFY(f.init());
    PaleoTaskService tasks;
    f.work.setTaskService(&tasks);
    CraftedWellService service;
    f.work.setPredictionService(&service);
    QVERIFY(!f.well("A", 10, 20).isEmpty());
    QVERIFY(f.tops("A", QStringLiteral("#WellTops File From SMI\n#WellName Name MD X Y Z TVD Time(ms)\n"
                                       "A D61 1000 0 0 -1000 1000 -99999\nA D62 1060 0 0 -1060 1060 -99999\n")));
    QString error;
    QVERIFY2(f.work.predict("D61", "wells", {"A"}, &error), qPrintable(error));
    // 立即返回、任务中心出一行运行中任务；曲线后台读完才提交服务。
    QVERIFY(f.work.busy());
    QCOMPARE(tasks.tasks().size(), 1);
    PaleoTask *task = tasks.tasks().first();
    QVERIFY(task->running());
    QVERIFY(task->title().contains(QStringLiteral("D61")));
    QTRY_VERIFY(!service.request.id.isEmpty());
    QCOMPARE(service.request.wells.first().toMap().value("depth_top").toDouble(), 1000.0);
    QCOMPARE(service.request.wells.first().toMap().value("depth_bottom").toDouble(), 1120.0);
    service.deliver();
    QVERIFY(!f.work.busy());
    QTRY_COMPARE(task->state(), PaleoTask::State::Succeeded);
    QCOMPARE(task->percent(), 100);

    // 井点相取 D61 地层内优势相（相 2），不是整井优势相（相 1）。
    const auto id = f.latest("wells_prediction");
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(id));
    QVERIFY(layer);
    QgsFeature feature;
    QVERIFY(layer->getFeatures().nextFeature(feature));
    QCOMPARE(feature.attribute("facies_code").toInt(), 2);
    QCOMPARE(feature.attribute("facies_scope").toString(), QString("formation"));
    // 与地层相属性表 D61 行同源。
    const auto row = WellAttributeStore::rows(f.dir.path(), &f.layers,
                                              WellAttributeStore::Table::Formations,
                                              "A", "D61");
    QCOMPARE(row.size(), 1);
    QCOMPARE(row.first().toMap().value("facies_code").toInt(), 2);

    // 编辑副本里属性表直接改点相：只改 D61 地层范围内的井段，范围外原样。
    const auto draft = f.work.copyForEditing(id, {}, &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *draftLayer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft));
    QVERIFY(draftLayer && draftLayer->getFeatures().nextFeature(feature));
    QVERIFY(f.work.assignFacies(draft, {feature.id()}, 3, &error));
    const auto revised = f.work.wellPredictions(draft).first().toMap();
    QCOMPARE(revised.value("facies_code").toInt(), 3);
    for (const auto &v : revised.value("intervals").toList()) {
      const auto iv = v.toMap();
      const bool inside = iv.value("top").toDouble() >= 1000 && iv.value("bottom").toDouble() <= 1060;
      QCOMPARE(iv.value("code").toInt(), inside ? 3 : 1);
    }
    QVERIFY(draftLayer->rollBack());

    // 任务中心取消 = 取消预测；页面「取消」同样让任务落到已取消。
    QVERIFY(f.work.predict("D61", "wells", {"A"}, &error));
    PaleoTask *second = tasks.tasks().last();
    QVERIFY(second != task && second->running());
    second->requestCancel();
    QTRY_VERIFY(!f.work.busy());
    QTRY_COMPARE(second->state(), PaleoTask::State::Cancelled);
    QVERIFY(f.work.predict("D61", "wells", {"A"}, &error));
    PaleoTask *third = tasks.tasks().last();
    f.work.cancelPrediction();
    QTRY_COMPARE(third->state(), PaleoTask::State::Cancelled);
    QVERIFY(!f.work.busy());
  }
  void faciesDominantCodeAndRecodeHelpers() {
    const QVariantList iv{QVariantMap{{"top", 0.0}, {"bottom", 10.0}, {"code", 1}},
                          QVariantMap{{"top", 10.0}, {"bottom", 60.0}, {"code", 2}},
                          QVariantMap{{"top", 60.0}, {"bottom", 120.0}, {"code", 1}}};
    const double inf = std::numeric_limits<double>::infinity();
    QCOMPARE(FaciesCatalog::dominantCode(iv, -inf, inf), 1);
    QCOMPARE(FaciesCatalog::dominantCode(iv, 0, 60), 2);
    QCOMPARE(FaciesCatalog::dominantCode(iv, 200, 300), 0);
    // 等厚取较小编码。
    QCOMPARE(FaciesCatalog::dominantCode(iv, 0, 20), 1);
    const auto recoded = FaciesCatalog::recodeIntervals(iv, 3, 5, 70);
    QCOMPARE(recoded.size(), 5); // 两个跨界井段各切一刀
    double last = 0;
    for (const auto &v : recoded) {
      const auto r = v.toMap();
      QCOMPARE(r.value("top").toDouble(), last);
      last = r.value("bottom").toDouble();
      const bool inside = r.value("top").toDouble() >= 5 && r.value("bottom").toDouble() <= 70;
      QVERIFY(inside ? r.value("code").toInt() == 3 : r.value("code").toInt() == 1);
    }
    QCOMPARE(last, 120.0);
    double top = 0, base = 0;
    FaciesCatalog::formationRange(QVariant(), 5.0, &top, &base);
    QVERIFY(std::isinf(top) && std::isinf(base));
  }
  // 2026-10-10 用户要求①②：编图链三页图层列表保证测井基础图层在场（存在
  // 才物化），地层点带文字标注（formation 名绕点排开），不只有颜色。
  void wellBaseLayersMaterializedWithFormationLabels() {
    Fixture f;
    QVERIFY(f.init());
    // 无产物文件：不造数据、不出层（「如果有的话」语义）。
    f.work.ensureWellBaseLayers();
    QVERIFY(!f.layers.isInstantiated(WellAttributeStore::formationLayerId()));
    QVERIFY(!f.layers.isInstantiated(WellAttributeStore::intervalLayerId()));
    QVERIFY(!f.well("A", 10, 20).isEmpty());
    QVERIFY(f.tops("A", QStringLiteral("#WellTops File From SMI\n#WellName Name MD X Y Z TVD Time(ms)\n"
                                       "A D61 1000 0 0 -1000 1000 -99999\nA D62 1060 0 0 -1060 1060 -99999\n")));
    QString error;
    QVERIFY2(f.work.predict("D61", "wells", {"A"}, &error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(!f.work.busy(), 5000);
    // 首预测落盘后立即在场（无需重开工程或切页），且地层点样式+标注就位。
    QVERIFY(f.layers.isInstantiated(WellAttributeStore::formationLayerId()));
    QVERIFY(f.layers.isInstantiated(WellAttributeStore::intervalLayerId()));
    auto *formations = qobject_cast<QgsVectorLayer *>(
        f.layers.layer(WellAttributeStore::formationLayerId()));
    QVERIFY(formations);
    // facies-ux 3：相点按相分类着色（规则渲染：层位分类 → 缺省分类 → 灰）。
    QCOMPARE(formations->renderer()->type(), QString("RuleRenderer"));
    {
      const auto colorOf = [](QgsVectorLayer *layer, const QgsFeature &feature) {
        QgsRenderContext ctx;
        ctx.expressionContext().appendScopes(
            QgsExpressionContextUtils::globalProjectLayerScopes(layer));
        ctx.expressionContext().setFeature(feature);
        auto *renderer = layer->renderer();
        renderer->startRender(ctx, layer->fields());
        const auto symbols = renderer->originalSymbolsForFeature(feature, ctx);
        const QColor color = symbols.isEmpty() ? QColor() : symbols.first()->color();
        renderer->stopRender(ctx);
        return color;
      };
      const auto schema = f.work.facies("D61");
      QgsFeatureIterator it = formations->getFeatures();
      QgsFeature feature;
      int checked = 0;
      while (it.nextFeature(feature)) {
        const QVariant code = feature.attribute("facies_code");
        const QColor expected =
            code.isNull() ? QColor("#9AA7B4")
                          : QColor(FaciesCatalog::find(schema, code).value("color").toString());
        QCOMPARE(colorOf(formations, feature).name(), expected.name());
        ++checked;
      }
      QVERIFY(checked > 0);
      // 改层位分类颜色 → 在场相点层随之重着色。
      auto edited = schema;
      auto first = edited.first().toMap();
      first.insert("color", "#123456");
      edited[0] = first;
      QString schemaErr;
      QVERIFY2(f.work.saveFacies("D61", edited, &schemaErr), qPrintable(schemaErr));
      QgsFeature probe;
      probe.setFields(formations->fields(), true);
      probe.setAttribute("formation", "D61");
      probe.setAttribute("facies_code", first.value("code").toInt());
      QCOMPARE(colorOf(formations, probe).name(), QString("#123456"));
      probe.setAttribute("facies_code", QVariant());
      QCOMPARE(colorOf(formations, probe).name(), QColor("#9AA7B4").name());
    }
    QVERIFY(formations->labelsEnabled());
    QVERIFY(formations->labeling() != nullptr);
    auto *labels = dynamic_cast<QgsVectorLayerSimpleLabeling *>(formations->labeling());
    QVERIFY(labels);
    QVERIFY(labels->settings().fieldName.contains(QLatin1String("formation")));
    // 井段属性层：小空心点，不标注（每井多行同位堆叠）。
    auto *intervals = qobject_cast<QgsVectorLayer *>(
        f.layers.layer(WellAttributeStore::intervalLayerId()));
    QVERIFY(intervals);
    QVERIFY(!intervals->labelsEnabled());
  }
  void faciesConsistencyReportsWellVsSeismic() {
    // facies-ux 4：选任一侧，另一侧自动取同层位最新结果；逐井取样与栅格
    // 一致、混淆矩阵计数守恒、报告 JSON 原子留档。
    Fixture f;
    QVERIFY(f.init());
    QVERIFY(!f.well("A", 10, 20).isEmpty());
    QVERIFY(!f.well("B", 300, 200).isEmpty());
    QString error;
    QVERIFY(f.work.predict("D61", "wells", {"A", "B"}, &error));
    QTRY_VERIFY(!f.work.busy());
    const auto wells = f.latest("wells_prediction");
    QVERIFY(!wells.isEmpty());
    QVERIFY(f.work.faciesConsistency(wells, &error).isEmpty());
    QVERIFY(error.contains(QStringLiteral("地震相")));
    // facies-ux 5：预测版本带证据链（输入版本、模型、参数指纹、支撑井）。
    {
      const auto evidence = f.work.versionForLayer(wells).extra.value("evidence").toMap();
      QCOMPARE(evidence.value("schema").toString(), QString("paleo_evidence_v1"));
      QVERIFY(!evidence.value("inputs").toList().isEmpty());
      for (const auto &in : evidence.value("inputs").toList())
        QVERIFY(!in.toMap().value("version_id").toString().isEmpty());
      QCOMPARE(evidence.value("parameter_hash").toString().size(), 64);
      QVERIFY(!evidence.value("model").toMap().value("method").toString().isEmpty());
      const auto support = evidence.value("support").toMap().value("wells").toMap();
      QCOMPARE(support.value("supporting_wells").toList().size() +
                   support.value("skipped_wells").toList().size(),
               2);
    }
    const auto input = f.seismic();
    QVERIFY(!input.isEmpty());
    QVERIFY2(f.work.predict("D61", "seismic", {input}, &error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(!f.work.busy(), 5000);
    const auto raster = f.latest("seismic_prediction");
    QVERIFY(!raster.isEmpty());
    error.clear();
    const auto report = f.work.faciesConsistency(wells, &error);
    QVERIFY2(!report.isEmpty(), qPrintable(error));
    QCOMPARE(report.value("well_layer").toString(), wells);
    QCOMPARE(report.value("seismic_layer").toString(), raster);
    QCOMPARE(report.value("schema").toString(), QString("paleo_facies_consistency_v1"));
    const auto rows = report.value("rows").toList();
    QCOMPARE(rows.size(), 2);
    QCOMPARE(report.value("compared").toInt() + report.value("uncompared").toInt(), 2);
    int total = 0;
    for (const auto &line : report.value("confusion").toList())
      for (const auto &cell : line.toList())
        total += cell.toInt();
    QCOMPARE(total, report.value("compared").toInt());
    auto *r = qobject_cast<QgsRasterLayer *>(f.layers.instantiate(raster));
    QVERIFY(r);
    for (const auto &row : rows) {
      const auto m = row.toMap();
      bool ok = false;
      const double v = r->dataProvider()->sample(
          QgsPointXY(m.value("x").toDouble(), m.value("y").toDouble()), 1, &ok);
      if (ok && std::isfinite(v))
        QCOMPARE(m.value("seismic_code").toInt(), int(std::lround(v)));
      else
        QVERIFY(m.value("seismic_code").isNull());
    }
    const auto path = report.value("report_path").toString();
    QVERIFY(QFileInfo::exists(path));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto json = QJsonDocument::fromJson(file.readAll()).object();
    QCOMPARE(json.value("seismic_version").toString(), f.work.versionForLayer(raster).id);
    QCOMPARE(json.value("rows").toArray().size(), 2);
    // 从地震相一侧发起：反查到同一测井相结果。
    const auto reverse = f.work.faciesConsistency(raster, &error);
    QCOMPARE(reverse.value("well_layer").toString(), wells);
    // 相面一侧（多边形取样）：编码与栅格一侧一致（转面保持编码）。
    const auto polygon = f.work.polygonize(raster, &error);
    QVERIFY2(!polygon.isEmpty(), qPrintable(error));
    const auto viaPolygon = f.work.faciesConsistency(wells, &error, polygon);
    QVERIFY2(!viaPolygon.isEmpty(), qPrintable(error));
    QCOMPARE(viaPolygon.value("seismic_layer").toString(), polygon);
    {
      const auto evidence = f.work.versionForLayer(polygon).extra.value("evidence").toMap();
      QCOMPARE(evidence.value("inputs").toList().size(), 1);
      QCOMPARE(evidence.value("inputs").toList().first().toMap().value("version_id").toString(),
               f.work.versionForLayer(raster).id);
      QCOMPARE(evidence.value("support").toMap().value("source_layer").toString(), raster);
    }
    QCOMPARE(viaPolygon.value("rows").toList().size(), 2);
    // 属性表改相：工作副本 facies_code 用相名下拉（值映射到编码）。
    const auto draft = f.work.copyForEditing(polygon, {}, &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    f.work.styleLayer(draft);
    auto *draftLayer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft));
    QVERIFY(draftLayer);
    const auto setup = draftLayer->editorWidgetSetup(draftLayer->fields().indexOf("facies_code"));
    QCOMPARE(setup.type(), QString("ValueMap"));
    QVERIFY(!setup.config().value("map").toList().isEmpty());
    // 非相结果被拒。
    QVERIFY(f.work.faciesConsistency(QStringLiteral("nope"), &error).isEmpty());
  }
  void seismicToPolygonToEditableVersions() {
    Fixture f;
    QVERIFY(f.init());
    const auto input = f.seismic();
    QVERIFY(!input.isEmpty());
    QString error;
    QSignalSpy ready(&f.work, &MappingWorkbench::productReady),
        errors(&f.work, &MappingWorkbench::errorOccurred);
    QVERIFY2(f.work.predict("D61", "seismic", {input}, &error),
             qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(!f.work.busy(), 5000);
    QCOMPARE(errors.count(), 0);
    const auto rasterId = f.latest("seismic_prediction");
    QVERIFY(!rasterId.isEmpty());
    // #421：productReady spy 落地断言（原为死 spy——信号丢失无人察觉）。
    QCOMPARE(ready.count(), 1);
    QCOMPARE(ready.first().at(0).toString(), QString("D61"));
    QCOMPARE(ready.first().at(1).toString(), rasterId);
    auto v = f.work.versionForLayer(rasterId);
    QVERIFY(v.parentVersionIds.contains(f.catalog.currentVersion(input).id));
    QVERIFY(v.extra.value("mock").toBool());
    QCOMPARE(v.extra.value("horizon").toString(), QString("D61"));
    auto *r =
        qobject_cast<QgsRasterLayer *>(f.layers.instantiate(rasterId, &error));
    QVERIFY2(r, qPrintable(error));
    QCOMPARE(r->width(), 64);
    QCOMPARE(r->height(), 64);
    QVERIFY(r->extent().width() > 639);
    QCOMPARE(r->crs(), QgsCoordinateReferenceSystem::fromWkt(
                           DataCatalog::localGridCrsWkt()));
    const auto polygon = f.work.polygonize(rasterId, &error);
    QVERIFY2(!polygon.isEmpty(), qPrintable(error));
    auto *original =
        qobject_cast<QgsVectorLayer *>(f.layers.instantiate(polygon, &error));
    QVERIFY(original);
    QVERIFY(original->featureCount() > 0);
    for (const auto &name :
         {"facies_name", "subfacies", "microfacies", "texture"})
      QVERIFY(original->fields().indexOf(name) >= 0);
    QVERIFY(original->readOnly());
    // 样式链落位：分类+合并渲染、序号+相名标注（label mode 默认 3）。
    QCOMPARE(original->renderer()->type(), QString("mergedFeatureRenderer"));
    QVERIFY(original->labelsEnabled());
    QVERIFY(original->labeling());
    const auto hash = digest(original->source());
    const auto draft = f.work.copyForEditing(polygon, {}, &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *edit =
        qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft, &error));
    QVERIFY(edit);
    QVERIFY(!edit->readOnly());
    const auto previous = f.work.versionForLayer(draft);
    QVERIFY(
        previous.parentVersionIds.contains(f.work.versionForLayer(polygon).id));
    QVERIFY(edit->startEditing());
    auto it = edit->getFeatures();
    QgsFeature feature;
    QVERIFY(it.nextFeature(feature));
    const int field = edit->fields().indexOf("facies_code");
    QVERIFY(field >= 0);
    int code = feature.attribute(field).toInt() == 1 ? 2 : 1;
    QVERIFY(edit->changeAttributeValue(feature.id(), field, code));
    QVERIFY(edit->commitChanges());
    QVERIFY2(f.work.saveEditingVersion(draft, &error), qPrintable(error));
    const auto next = f.work.versionForLayer(draft);
    QCOMPARE(next.versionNumber, previous.versionNumber + 1);
    QVERIFY(next.parentVersionIds.contains(previous.id));
    QCOMPARE(digest(original->source()), hash);
    QVERIFY(next.sha256 != previous.sha256);
    QgsVectorLayer snapshot(
        DataCatalog::resolvedVersionPath(f.dir.path(), next) +
            "|layername=features",
        "check", "ogr");
    QVERIFY(snapshot.isValid());
    QgsFeature saved;
    auto si = snapshot.getFeatures();
    QVERIFY(si.nextFeature(saved));
    QCOMPARE(saved.attribute("facies_code").toInt(), code);
    f.work.bindCatalog(&f.catalog, f.dir.path());
    QVERIFY(!f.work.declaration(draft).source.isEmpty());
    QVERIFY(!f.work.declaration("product." + next.id).source.isEmpty());
  }
  // 2026-10-10 用户反馈「画布上没有标注」的回归闸：相面经 styleLayer 后，
  // 标注配置在位（enabled + mode 3 表达式），且离屏渲染真的放置出标注
  //（takeLabelingResults 非空）——样式应用、标注注册、渲染三段一起守。
  // facies-ux 1：相栅格转面走任务中心（后台、可见、可取消、单飞），完成
  // 后经 productReady 收尾；取消与工程重绑不登记任何版本。
  void polygonizeRunsAsCancellableBackgroundTask() {
    Fixture f;
    QVERIFY(f.init());
    PaleoTaskService tasks;
    f.work.setTaskService(&tasks);
    const auto input = f.seismic();
    QVERIFY(!input.isEmpty());
    QString error;
    QVERIFY2(f.work.predict("D61", "seismic", {input}, &error),
             qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(!f.work.busy(), 5000);
    const auto rasterId = f.latest("seismic_prediction");
    QVERIFY(!rasterId.isEmpty());
    QSignalSpy done(&f.work, &MappingWorkbench::polygonizeFinished);
    const int before = tasks.tasks().size();
    QVERIFY2(f.work.startPolygonize(rasterId, &error), qPrintable(error));
    QVERIFY(f.work.polygonizeBusy());
    QCOMPARE(tasks.tasks().size(), before + 1);
    QPointer<PaleoTask> task = tasks.tasks().constLast();
    QVERIFY(!task->quiet());
    QVERIFY(task->title().contains(QStringLiteral("相栅格转相面")));
    QString busyErr;
    QVERIFY(!f.work.startPolygonize(rasterId, &busyErr));
    QVERIFY(busyErr.contains(QStringLiteral("已有相栅格转面任务")));
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 60000);
    auto args = done.takeFirst();
    const auto product = args.at(1).toString();
    QVERIFY2(!product.isEmpty(), qPrintable(args.at(2).toString()));
    QVERIFY(task);
    QCOMPARE(task->state(), PaleoTask::State::Succeeded);
    QVERIFY(!f.work.polygonizeBusy());
    const auto version = f.work.versionForLayer(product);
    QCOMPARE(version.extra.value("kind").toString(), QString("facies_polygons"));
    QCOMPARE(version.extra.value("source_layer").toString(), rasterId);
    QCOMPARE(f.work.declaration(product).horizon, QString("D61"));
    auto *poly =
        qobject_cast<QgsVectorLayer *>(f.layers.instantiate(product, &error));
    QVERIFY2(poly, qPrintable(error));
    QVERIFY(poly->featureCount() > 0);
    QVERIFY(poly->fields().indexOf("facies_name") >= 0);

    // 取消 / 工程重绑：池占满让转面任务排队，确定性地在 compute 前取消。
    tasks.setMaxWorkerThreads(1);
    const int products = f.work.products("D61").size();
    for (const bool rebind : {false, true}) {
      std::atomic_bool release{false};
      tasks.start(QStringLiteral("blocker"), [&release](PaleoTask *) {
        while (!release.load())
          QThread::msleep(5);
        return QString();
      });
      QVERIFY2(f.work.startPolygonize(rasterId, &error), qPrintable(error));
      if (rebind)
        f.work.bindCatalog(&f.catalog, f.dir.path()); // 工程重绑即取消
      else
        f.work.cancelPolygonize();
      release.store(true);
      QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 60000);
      args = done.takeFirst();
      QVERIFY(args.at(1).toString().isEmpty());
      QVERIFY(args.at(3).toBool());
      QTRY_VERIFY_WITH_TIMEOUT(!f.work.polygonizeBusy(), 5000);
      QCOMPARE(f.work.products("D61").size(), products);
    }
    QVERIFY(tasks.shutdown());
  }
  void polygonLabelsRenderAfterStyling() {
    Fixture f;
    QVERIFY(f.init());
    const auto input = f.seismic();
    QVERIFY(!input.isEmpty());
    QString error;
    QVERIFY2(f.work.predict("D61", "seismic", {input}, &error),
             qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(!f.work.busy(), 5000);
    const auto polygon = f.work.polygonize(f.latest("seismic_prediction"), &error);
    QVERIFY2(!polygon.isEmpty(), qPrintable(error));
    auto *v = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(polygon, &error));
    QVERIFY2(v, qPrintable(error));
    QVERIFY(v->featureCount() > 0);
    QVERIFY(v->labelsEnabled());
    auto *simple = dynamic_cast<QgsVectorLayerSimpleLabeling *>(v->labeling());
    QVERIFY(simple);
    QVERIFY(simple->settings().isExpression);
    // 默认 label mode 2：面状相只标相名，不带 ID 序号（2026-10-10 用户
    // 改口径「默认不要有 ID 的标注，简单的文字标注就行」）。
    QVERIFY(simple->settings().fieldName.contains("facies_code"));
    QVERIFY(!simple->settings().fieldName.contains("$id"));
    QVERIFY(!simple->settings().fieldName.contains("\"fid\""));
    QCOMPARE(v->customProperty("paleo/faciesLabelMode").toInt(), 2);
    QCOMPARE(f.work.labelMode(polygon), 2);
    // 显式选「序号 ＋ 相名」后照用户选择。
    QVERIFY(f.work.setLabelMode(polygon, 3, &error));
    QCOMPARE(f.work.labelMode(polygon), 3);
    QVERIFY(dynamic_cast<QgsVectorLayerSimpleLabeling *>(v->labeling())
                ->settings().fieldName.contains("$id"));
    QVERIFY(f.work.setLabelMode(polygon, 2, &error));

    // 文字标注：无底衬背景、字号随 @map_scale 数据定义自适应（2026-10-11
    // 用户口径「标注不需要背景、自适应大小」）。
    {
      const auto labelSettings =
          dynamic_cast<QgsVectorLayerSimpleLabeling *>(v->labeling())
              ->settings();
      QVERIFY(!labelSettings.format().background().enabled());
      QVERIFY(labelSettings.dataDefinedProperties().isActive(
          QgsPalLayerSettings::Property::Size));
    }
    // 相面边缘：分类符号带可见描边（同系加深一档，SVG 纹理层追加轮廓层），
    // 不再是与填充同色的隐形边。
    {
      auto *merged =
          dynamic_cast<QgsMergedFeatureRenderer *>(v->renderer());
      QVERIFY(merged);
      auto *cat = dynamic_cast<const QgsCategorizedSymbolRenderer *>(
          merged->embeddedRenderer());
      QVERIFY(cat);
      bool edged = false;
      for (const QgsRendererCategory &c : cat->categories())
        for (int i = 0;
             c.symbol() && i < c.symbol()->symbolLayerCount(); ++i)
          if (auto *fl = dynamic_cast<const QgsSimpleFillSymbolLayer *>(
                  c.symbol()->symbolLayer(i)))
            if (fl->strokeStyle() != Qt::NoPen && fl->strokeWidth() > 0.0)
              edged = true;
      QVERIFY(edged);
    }

    QgsMapSettings ms;
    ms.setLayers({v});
    ms.setDestinationCrs(v->crs());
    auto extent = v->extent();
    extent.scale(1.05);
    ms.setExtent(extent);
    ms.setOutputSize(QSize(900, 900));
    ms.setBackgroundColor(Qt::white);
    QgsMapRendererSequentialJob job(ms);
    job.start();
    job.waitForFinished();
    std::unique_ptr<QgsLabelingResults> labels(job.takeLabelingResults());
    QVERIFY(labels);
    QCOMPARE(labels->allLabels().size(), v->featureCount());
  }
  // 2026-10-11 用户截图反馈「相面标注还带序号」的回归闸：.qgz 烘焙的旧
  // 默认（mode 3）图层重开后走 adopt 收编、不发 layerInstantiated，曾经
  // 整份 styleLayer 缺席，画布一直显示序号。重开工程后必须按工程条目
  // 重排：没显式选过的面回默认 mode 2（只标相名）。
  void projectReopenRestylesStaleFaciesLabels() {
    Fixture f;
    QVERIFY(f.init());
    const auto input = f.seismic();
    QVERIFY(!input.isEmpty());
    QString error;
    QVERIFY2(f.work.predict("D61", "seismic", {input}, &error),
             qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(!f.work.busy(), 5000);
    const auto polygon =
        f.work.polygonize(f.latest("seismic_prediction"), &error);
    QVERIFY2(!polygon.isEmpty(), qPrintable(error));
    auto *v = qobject_cast<QgsVectorLayer *>(
        f.layers.instantiate(polygon, &error));
    QVERIFY2(v, qPrintable(error));
    // 模拟旧版本写出的工程：mode 3 烘焙进图层自定义属性与标注表达式。
    MappingArtifactWriter::applyFaciesLabels(v, 3);
    QVERIFY(f.project.writeProject());
    QVERIFY(f.project.openProject(f.project.projectPath()));
    auto *restored = qobject_cast<QgsVectorLayer *>(f.layers.layer(polygon));
    QVERIFY2(restored, "adopted layer missing after reopen");
    // projectOpened → 排队 restyle。
    QTRY_COMPARE_WITH_TIMEOUT(
        restored->customProperty("paleo/faciesLabelMode").toInt(), 2, 5000);
    QVERIFY(restored->labelsEnabled());
    const auto *simple =
        dynamic_cast<QgsVectorLayerSimpleLabeling *>(restored->labeling());
    QVERIFY(simple);
    QVERIFY(simple->settings().isExpression);
    QVERIFY(!simple->settings().fieldName.contains("$id"));
    QCOMPARE(f.work.labelMode(polygon), 2);
    // 显式选过「序号 ＋ 相名」的工程条目不受重排影响。
    QVERIFY(f.work.setLabelMode(polygon, 3, &error));
    QVERIFY(f.project.writeProject());
    QVERIFY(f.project.openProject(f.project.projectPath()));
    auto *kept = qobject_cast<QgsVectorLayer *>(f.layers.layer(polygon));
    QVERIFY(kept);
    QTRY_COMPARE_WITH_TIMEOUT(
        kept->customProperty("paleo/faciesLabelMode").toInt(), 3, 5000);
    QVERIFY(dynamic_cast<QgsVectorLayerSimpleLabeling *>(kept->labeling())
                ->settings()
                .fieldName.contains("$id"));
  }
  void wellsSingleBatchCancelAndHorizonSchemas() {
    Fixture f;
    QVERIFY(f.init());
    QVERIFY(!f.well("A", 100, 100).isEmpty());
    QVERIFY(!f.well("B", 500, 500).isEmpty());
    for (const auto &name : {"A", "B"})
      QVERIFY(f.tops(name, QStringLiteral("#WellTops File From SMI\n#WellName Name MD X Y Z TVD Time(ms)\n%1 D61 1000 0 0 -1000 1000 -99999\n%1 D62 1060 0 0 -1060 1060 -99999\n").arg(name)));
    QString error;
    QVariantList facies{
        QVariantMap{{"code", 7}, {"name", "河道"}, {"facies", "三角洲"},
                    {"subfacies", "三角洲前缘"}, {"microfacies", "水下分流河道"},
                    {"color", "#E6C875"}},
        QVariantMap{{"code", 9}, {"name", "湖相"}, {"color", "#97B4CE"}}};
    QVERIFY(f.work.saveFacies("D61", facies, &error));
    QVERIFY(f.work.facies("D62") != facies);
    QVERIFY(WellAttributeStore::mergeIntervals(&f.catalog, f.dir.path(), &f.layers,
        {QVariantMap{{"well_id", "A"}, {"top_md", 1000.0}, {"base_md", 1120.0},
                     {"facies", "旧相"}, {"facies_pattern", "sandstone"}}}, &error));
    QVERIFY(f.work.predict("D61", "wells", {"A"}, &error));
    QTRY_VERIFY(!f.work.busy());
    const auto first = f.latest("wells_prediction");
    QVERIFY(!first.isEmpty());
    auto *single =
        qobject_cast<QgsVectorLayer *>(f.layers.instantiate(first, &error));
    QVERIFY(single);
    QCOMPARE(single->featureCount(), 1);
    QCOMPARE(f.work.versionForLayer(first).extra.value("facies").toList(),
             facies);
    // mock 无 tops 时属性表可能保留手工「旧相」行；有 facies_code 才核对称名。
    for (const auto &v : WellAttributeStore::rows(f.dir.path(), nullptr, false, "A")) {
      const auto row = v.toMap();
      if (row.value("top_md").toDouble() >= 1060) {
        QCOMPARE(row.value("facies").toString(), QStringLiteral("旧相"));
        continue;
      }
      if (row.value("facies_code").isNull() || row.value("facies_code").toInt() <= 0)
        continue;
      const auto hierarchy = FaciesCatalog::attributes(facies, row.value("facies_code"));
      QCOMPARE(row.value("facies"), hierarchy.value("facies_name"));
      QCOMPARE(row.value("sub_facies").toString(), hierarchy.value("subfacies").toString());
      QCOMPARE(row.value("micro_facies").toString(), hierarchy.value("microfacies").toString());
      QVERIFY(row.value("facies_pattern").toString().isEmpty());
    }
    QVERIFY(f.work.predict("D61", "wells", {"A", "B"}, &error));
    QTRY_VERIFY(!f.work.busy());
    const auto second = f.latest("wells_prediction");
    QVERIFY(first != second);
    auto *batch =
        qobject_cast<QgsVectorLayer *>(f.layers.instantiate(second, &error));
    QVERIFY(batch);
    QCOMPARE(batch->featureCount(), 2);
    const auto result = f.work.compose("D61", {second}, {}, &error);
    if (result.isEmpty()) {
      // mock 相编码若未写入本层位 schema，编图会拒收——与 #346 地层相路径共存。
      QVERIFY2(error.contains(QStringLiteral("未定义"))
                   || error.contains(QStringLiteral("编码")),
               qPrintable(error));
    } else {
      QVERIFY(f.work.versionForLayer(result).extra.value("mock").toBool());
    }
    const int before = f.work.products("D61").size();
    QVERIFY(f.work.predict("D61", "wells", {"A"}, &error));
    f.work.cancelPrediction();
    QTest::qWait(350);
    QCOMPARE(f.work.products("D61").size(), before);
    QVERIFY(f.work.predict("D61", "wells", {"A"}, &error));
    f.work.bindCatalog(nullptr, {});
    QTest::qWait(350);
    QVERIFY(!f.work.busy());
    f.work.bindCatalog(&f.catalog, f.dir.path());
    QCOMPARE(f.work.facies("D61"), facies);
    QVERIFY(!f.work.predict("D61", "wells", {"missing"}, &error));
    QVERIFY(!error.isEmpty());
  }
  void constraintsFactorsContoursAndClassification() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto source = f.dir.filePath("samples.geojson");
    QVariantList points{QVariantMap{{"x", 0}, {"y", 0}, {"z", 0}},
                        QVariantMap{{"x", 100}, {"y", 0}, {"z", 20}},
                        QVariantMap{{"x", 0}, {"y", 100}, {"z", 40}},
                        QVariantMap{{"x", 100}, {"y", 100}, {"z", 60}}};
    QVERIFY(MappingArtifactWriter::points(source, points, &error));
    LayerDeclaration d;
    d.layerId = "wells.D61";
    d.horizon = "D61";
    d.type = "vector";
    d.source = source;
    d.title = "样点";
    QVERIFY(f.layers.declare(d, &error));
    QVERIFY2(f.constraints.addConstraint("D61", "LINESTRING(50 0,50 100)",
                                         "line", -1, &error),
             qPrintable(error));
    // 逐笔绘制不出快照版本（避免一条线一个「约束过程·vN」刷屏）；
    // 快照在使用点 generateFactor 前置步骤产生。
    QVERIFY(f.latest("constraint_snapshot").isEmpty());
    auto params = QVariantMap{
        {"pointsLayerId", d.layerId}, {"field", "z"}, {"cellSize", 10}};
    QVERIFY2(f.work.generateFactor("D61", "sandthick", params, &error),
             qPrintable(error));
    const auto factorId = f.latest("single_factor_raster");
    QVERIFY(!factorId.isEmpty());
    const auto factor = f.work.versionForLayer(factorId);
    QCOMPARE(
        f.layers.instantiate(factorId)->crs(),
        QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt()));
    QVERIFY(factor.parentVersionIds.size() >= 2);
    QVERIFY(qobject_cast<QgsRasterLayer *>(f.layers.instantiate(factorId))
                ->renderer()
                ->legendSymbologyItems()
                .size() >= 2);
    bool snapshotParent = false, inputParent = false;
    for (const auto &id : factor.parentVersionIds) {
      auto p = f.catalog.versionById(id);
      snapshotParent |= p.extra.value("kind") == "constraint_snapshot";
      inputParent |= p.extra.value("kind") == "input_snapshot";
    }
    QVERIFY(snapshotParent);
    QVERIFY(inputParent);
    QVERIFY(f.work.polygonize(factorId, &error).isEmpty());
    QVERIFY(!error.isEmpty());
    error.clear();
    QVERIFY2(f.work.generateContours("D61", factorId, 10, &error),
             qPrintable(error));
    const auto contours = f.latest("contour_lines");
    QVERIFY(!contours.isEmpty());
    QCOMPARE(
        f.layers.instantiate(contours)->crs(),
        QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt()));
    QVERIFY(
        f.work.versionForLayer(contours).parentVersionIds.contains(factor.id));
    QVERIFY(f.work.compose("D61", {factorId}, {}, &error).isEmpty());
    error.clear();
    const auto composed = f.work.compose(
        "D61", {factorId}, {{"thresholds", QVariantList{20., 40.}}}, &error);
    QVERIFY2(!composed.isEmpty(), qPrintable(error));
    auto *old = f.layers.instantiate("factor.D61.sandthick");
    QVERIFY(old);
    const auto oldPath = old->source();
    params.insert("cellSize", 20);
    QVERIFY2(f.work.generateFactor("D61", "sandthick", params, &error),
             qPrintable(error));
    QVERIFY(f.layers.instantiate("factor.D61.sandthick")->source() != oldPath);
    QVERIFY(QFileInfo::exists(oldPath));
    const auto constraint = f.latest("constraint_snapshot");
    const auto importPath = f.work.declaration(constraint).source;
    QVERIFY2(f.work.importConstraints("D62", importPath, &error),
             qPrintable(error));
    QCOMPARE(f.constraints.loadConstraints("D62").size(), 1);
  }
  void rejectsBadAndLateRemoteResponses() {
    Fixture f;
    QVERIFY(f.init());
    QVERIFY(!f.well("A", 100, 100).isEmpty());
    DelayedPredictionService service;
    f.work.setPredictionService(&service);
    QString error;
    QSignalSpy failures(&f.work, &MappingWorkbench::errorOccurred);
    QVERIFY(f.work.predict("D61", "wells", {"A"}, &error));
    // 测井曲线在后台读完才提交预测服务。
    QTRY_VERIFY(!service.request.id.isEmpty());
    service.deliver();
    QCOMPARE(failures.count(), 1);
    QVERIFY(!f.work.busy());
    QVERIFY(f.latest("wells_prediction").isEmpty());
    const QString firstId = service.request.id;
    QVERIFY(f.work.predict("D61", "wells", {"A"}, &error));
    QTRY_VERIFY(service.request.id != firstId);
    f.work.cancelPrediction();
    service.deliver();
    QVERIFY(f.latest("wells_prediction").isEmpty());
    QCOMPARE(failures.count(), 1);
  }
  void panelGuidanceSelectionAndRefreshPreserveUserIntent() {
    Fixture f;
    QVERIFY(f.init());
    MappingWorkbenchPage page("predict", &f.work);
    page.setHorizon("D61");
    auto *kind = page.findChild<QComboBox *>("predictionKind");
    auto *inputs = page.findChild<QListWidget *>("workbenchInputs");
    auto *select = page.findChild<QPushButton *>("workbenchSelectInputs");
    auto *hint = page.findChild<QLabel *>("workbenchInputHint");
    QVERIFY(kind && inputs && select && hint);
    QVERIFY(!select->isEnabled());
    QVERIFY(hint->text().contains(QStringLiteral("数据管理")));
    QVERIFY(!f.well("A", 10, 20).isEmpty());
    QVERIFY(!f.well("B", 30, 40).isEmpty());
    // 预测类型下拉新增本地 Mock 首项后按下标定位会错位——按 data 取。
    kind->setCurrentIndex(kind->findData(QStringLiteral("wells")));
    const int inputHeight = inputs->minimumHeight();
    inputs->item(0)->setCheckState(Qt::Checked);
    select->click(); // A partial selection becomes all, not none.
    QCOMPARE(inputs->item(0)->checkState(), Qt::Checked);
    QCOMPARE(inputs->item(1)->checkState(), Qt::Checked);
    select->click();
    QCOMPARE(inputs->item(0)->checkState(), Qt::Unchecked);
    QCOMPARE(inputs->item(1)->checkState(), Qt::Unchecked);
    QString error;
    QVERIFY(f.work.predict("D61", "wells", {"A", "B"}, &error));
    QTRY_VERIFY(!f.work.busy());
    const auto id = f.latest("wells_prediction");
    QVERIFY(!id.isEmpty());
    page.selectLayer(id);
    auto *labels = page.findChild<QComboBox *>("faciesLabelMode");
    QVERIFY(labels && labels->isEnabled());
    labels->setCurrentIndex(0);
    inputs->item(0)->setCheckState(Qt::Checked);
    page.refresh();
    QCOMPARE(labels->currentIndex(), 0); // unrelated refresh must not reset it
    QCOMPARE(inputs->minimumHeight(), inputHeight);
    QSignalSpy intent(&page, &MappingWorkbenchPage::commandRequested);
    page.commandButton("labels")->click();
    QCOMPARE(intent.last()[1].toMap().value("label_mode").toInt(), 0);
    QVERIFY(f.work.setLabelMode(id, 0, &error));
    // 不可见页的 changed→scheduleRefresh 不起定时器——显式同步。第一次与
    // 待应用的 0 对齐后 dirty 清除，外部随后改的 2 才会跟随到下拉框。
    page.refresh();
    QVERIFY(f.work.setLabelMode(id, 2, &error));
    page.refresh();
    QCOMPARE(f.work.labelMode(id), 2); // 外部写入以 workbench 为准
    QCOMPARE(labels->currentIndex(), 2); // saved state follows external changes

    MappingWorkbenchPage compose("compose", &f.work);
    compose.setHorizon("D61");
    auto *list = compose.findChild<QListWidget *>("workbenchInputs");
    QVERIFY(list && list->count() > 0);
    list->setCurrentRow(0);
    const auto selected = list->currentItem()->data(Qt::UserRole);
    compose.refresh();
    QVERIFY(list->currentItem());
    QCOMPARE(list->currentItem()->data(Qt::UserRole), selected);
    QVERIFY(!compose.findChild<QPushButton *>("workbenchMoveUp")->isEnabled());
  }

  // 方向51 Oracle①：装配出来的预测服务必须是 router（不是任何替身），
  // 且端点未配置时 UI 拿得到「未配置」的诚实状态。
  void remotePredictionAssemblyInstallsRouterNotSubstitute() {
    Fixture f;
    QVERIFY(f.init());
    // 未配置端点：仍然装 router（不带传输），而非 Mock。
    const auto unconfigured =
      installRemotePrediction(&f.work,
                              RemotePredictConfig::fromParts(QUrl(), true,
                                                             QString()),
                              nullptr, QString(), &f.work);
    QVERIFY(unconfigured.router);
    QVERIFY(unconfigured.service);
    QVERIFY(unconfigured.service == f.work.predictionService());
    QCOMPARE(QString(unconfigured.service->metaObject()->className()),
             QStringLiteral("RemotePredictionRouter"));
    QVERIFY(!unconfigured.transportBound);
    QVERIFY(unconfigured.statusHint.contains(QStringLiteral("未配置")));
    QVERIFY(f.work.predictionStatusHint().contains(QStringLiteral("未配置")));

    // 配了 loopback 端点：同一个装配函数挂上 HTTP 传输，仍是 router。
    const auto configured = installRemotePrediction(
        &f.work,
        RemotePredictConfig::fromParts(
            QUrl(QStringLiteral("http://127.0.0.1:65500")), true, QString()),
        nullptr, QString(), &f.work);
    QVERIFY(configured.transportBound);
    QVERIFY(configured.service == f.work.predictionService());
    QCOMPARE(QString(configured.service->metaObject()->className()),
             QStringLiteral("RemotePredictionRouter"));
  }
  void unboundPredictionReportsUnconfiguredInsteadOfMockResult() {
    Fixture f;
    QVERIFY(f.init());
    QVERIFY(!f.well("A", 100, 100).isEmpty());
    f.work.setPredictionService(nullptr); // 模拟装配未到位
    QString error;
    QVERIFY(!f.work.predict("D61", "wells", {"A"}, &error));
    QVERIFY2(error.contains(QStringLiteral("未配置")), qPrintable(error));
    QVERIFY(f.work.predictionStatusHint().contains(QStringLiteral("未配置")));
  }
  void panelSelectionAndBusyFollowActualState() {
    Fixture f;
    QVERIFY(f.init());
    QVERIFY(!f.well("A", 100, 100).isEmpty());
    MappingWorkbenchPage page("predict", &f.work);
    page.setHorizon("D61");
    auto *kind = page.findChild<QComboBox *>("predictionKind");
    QVERIFY(kind);
    kind->setCurrentIndex(kind->findData(QStringLiteral("wells")));
    auto *list = page.findChild<QListWidget *>("workbenchInputs");
    QCOMPARE(list->count(), 1);
    QVERIFY(!page.commandButton("predict")->isEnabled());
    list->item(0)->setCheckState(Qt::Checked);
    QVERIFY(page.commandButton("predict")->isEnabled());
    QSignalSpy intent(&page, &MappingWorkbenchPage::commandRequested);
    page.commandButton("predict")->click();
    QCOMPARE(intent.count(), 1);
    QCOMPARE(intent[0][1].toMap().value("inputs").toStringList(),
             QStringList{"A"});
    QString error;
    QVERIFY(f.work.predict("D61", "wells", {"A"}, &error));
    QVERIFY(!page.commandButton("predict")->isEnabled());
    QVERIFY(page.commandButton("cancel")->isEnabled());
    page.setHorizon("D62");
    QTRY_VERIFY(!f.work.busy());
    QCOMPARE(page.horizon(), QString("D62"));
    page.refresh(); // #346：changed 仅 scheduleRefresh，测例页未 show
    // 结果列表跨层位列出成果（激活层位与预测层位不一致时不再清空）：
    // D62 视图下 D61 的井预测成果仍在列表里。
    QVERIFY(
        page.findChild<QTreeWidget *>("workbenchResults")->topLevelItemCount() >
        0);
    page.setHorizon("D61");
    page.refresh();
    QVERIFY(
        page.findChild<QTreeWidget *>("workbenchResults")->topLevelItemCount() >
        0);
  }
  // P0-1（pr457-closure）：导入 provenance——外来米制工程 CRS 照收（#457 既有
  // 工作流），但坐标含义标记为「用户断言推测」（crs_verified=false +
  // crs_source）；文件 CRS 与工程网格同一时才记 crs_verified=true。画布写入门
  // （toStoreWkt）保持严格等价——两门契约有意分叉并留痕。
  void constraintImportRecordsCrsProvenance() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const QString base =
        QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath(
            "fixtures/singlefactor/structural_synthetic");
    // 外来 LOCAL_CS：接受但 provenance 如实标「未核实」。
    QVERIFY2(f.work.importConstraints("D61", base + "/directions.shp", "auto",
                                      &error),
             qPrintable(error));
    QVariantMap foreignExtra;
    for (const CatalogAsset &asset : f.catalog.assets()) {
      if (asset.type != QStringLiteral("constraint_source"))
        continue;
      for (const CatalogVersion &version : f.catalog.versionsForAsset(asset.id)) {
        if (version.extra.value(QStringLiteral("method")).toString() ==
            QStringLiteral("constraint-import")) {
          foreignExtra = version.extra;
          break;
        }
      }
    }
    QVERIFY(!foreignExtra.isEmpty());
    QCOMPARE(foreignExtra.value(QStringLiteral("crs_verified")).toBool(), false);
    QVERIFY(!foreignExtra.value(QStringLiteral("crs_source")).toString().isEmpty());

    // 工程本地网格同一 CRS：crs_verified=true（坐标来源已核实）。
    const QString verifiedPath = f.dir.filePath(QStringLiteral("verified-grid-break.gpkg"));
    {
      GDALDriverH driver = GDALGetDriverByName("GPKG");
      QVERIFY(driver);
      GDALDatasetH ds = GDALCreate(driver, verifiedPath.toUtf8().constData(), 0, 0, 0,
                                   GDT_Unknown, nullptr);
      QVERIFY(ds);
      OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
      QVERIFY(srs);
      const QByteArray wkt = DataCatalog::localGridCrsWkt().toUtf8();
      char *text = const_cast<char *>(wkt.constData());
      QVERIFY(OSRImportFromWkt(srs, &text) == OGRERR_NONE);
      OGRLayerH layer = GDALDatasetCreateLayer(ds, "constraints", srs, wkbLineString, nullptr);
      QVERIFY(layer);
      OSRDestroySpatialReference(srs);
      for (const char *name : {"id", "horizon", "type"}) {
        OGRFieldDefnH field = OGR_Fld_Create(name, OFTString);
        OGR_L_CreateField(layer, field, TRUE);
        OGR_Fld_Destroy(field);
      }
      OGRFeatureDefnH defn = OGR_L_GetLayerDefn(layer);
      OGRFeatureH feature = OGR_F_Create(defn);
      OGR_F_SetFieldString(feature, OGR_F_GetFieldIndex(feature, "id"), "v-1");
      OGR_F_SetFieldString(feature, OGR_F_GetFieldIndex(feature, "horizon"), "D61");
      OGR_F_SetFieldString(feature, OGR_F_GetFieldIndex(feature, "type"), "break_line");
      const char *lineWkt = "LINESTRING(0 0, 10 0)";
      OGRGeometryH geom = nullptr;
      char *lineText = const_cast<char *>(lineWkt);
      QVERIFY(OGR_G_CreateFromWkt(&lineText, nullptr, &geom) == OGRERR_NONE);
      OGR_F_SetGeometry(feature, geom); // 层带 SRS；几何自身无独立 SRS
      OGR_G_DestroyGeometry(geom);
      QVERIFY(OGR_L_CreateFeature(layer, feature) == OGRERR_NONE);
      OGR_F_Destroy(feature);
      GDALClose(ds);
    }
    QVERIFY2(f.work.importConstraints("D61", verifiedPath, "auto", &error),
             qPrintable(error));
    int verifiedSeen = 0;
    for (const CatalogAsset &asset : f.catalog.assets()) {
      if (asset.type != QStringLiteral("constraint_source"))
        continue;
      for (const CatalogVersion &version : f.catalog.versionsForAsset(asset.id)) {
        if (version.extra.value(QStringLiteral("method")).toString() !=
                QStringLiteral("constraint-import") ||
            !version.extra.contains(QStringLiteral("crs_verified")))
          continue;
        if (version.extra.value(QStringLiteral("crs_verified")).toBool())
          ++verifiedSeen;
      }
    }
    QCOMPARE(verifiedSeen, 1);
  }

  void constraintImportLocalCrsAndVerbatimRoundTrip() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const QString base =
        QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath(
            "fixtures/singlefactor/structural_synthetic");
    // LOCAL_CS（无大地基准的工程直角米制 CRS）与工程局部网格等价，
    // 不再被坐标门拒绝。
    QVERIFY2(f.work.importConstraints("D61", base + "/directions.shp", "auto",
                                      &error),
             qPrintable(error));
    QVERIFY2(f.work.importConstraints("D61", base + "/barriers.shp", "auto",
                                      &error),
             qPrintable(error));
    const auto rows = f.constraints.loadConstraints("D61");
    QCOMPARE(rows.size(), 8);
    QStringList types;
    QVariantList params;
    for (const auto &row : rows) {
      types << row.value("type").toString();
      params << QJsonDocument::fromJson(
                    row.value("params_json").toString().toUtf8())
                    .object()
                    .toVariantMap();
    }
    QCOMPARE(types.mid(0, 4),
             QStringList(4, QStringLiteral("direction_line")));
    QCOMPARE(types.mid(4, 4), QStringList(4, QStringLiteral("break_line")));
    QCOMPARE(params.at(0).toMap().value("semantic").toString(),
             QStringLiteral("direction_guide"));
    QCOMPARE(params.at(4).toMap().value("semantic").toString(),
             QStringLiteral("hard_barrier"));
    QCOMPARE(params.at(1).toMap().value("ratio").toDouble(), 8.0);
    QVERIFY(!params.at(3).toMap().value("enabled").toBool());
    QCOMPARE(params.at(5).toMap().value("blockMode").toString(),
             QStringLiteral("full_block"));
    QCOMPARE(params.at(6).toMap().value("blockMode").toString(),
             QStringLiteral("soft"));
    QVERIFY(!params.at(7).toMap().value("enabled").toBool());
    // generic 回导：快照里的 type+params_json 两列原样回写（不是 "line"）。
    const auto snapshot = f.latest("constraint_snapshot");
    QVERIFY(!snapshot.isEmpty());
    QVERIFY2(f.work.importConstraints(
                 "D62", f.work.declaration(snapshot).source, "auto", &error),
             qPrintable(error));
    const auto roundTrip = f.constraints.loadConstraints("D62");
    QCOMPARE(roundTrip.size(), 8);
    QCOMPARE(roundTrip.at(0).value("type").toString(),
             QStringLiteral("direction_line"));
    const auto rt = QJsonDocument::fromJson(
                        roundTrip.at(0).value("params_json").toString().toUtf8())
                        .object()
                        .toVariantMap();
    QCOMPARE(rt.value("semantic").toString(),
             QStringLiteral("direction_guide"));
    QCOMPARE(rt.value("ratio").toDouble(), 3.0);
    QCOMPARE(roundTrip.at(6).value("type").toString(),
             QStringLiteral("break_line"));
  }
  // P1-C：外部 CRS（GeoJSON 缺省 EPSG:4326）的约束线不按原坐标入库——
  // 坐标门如实拒绝，库里不留半截。
  void constraintImportRejectsForeignCrs() {
    Fixture f;
    QVERIFY(f.init());
    const QString path = f.dir.filePath("foreign-constraints.geojson");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"({"type":"FeatureCollection","features":[
      {"type":"Feature","properties":{"type":"break_line"},
       "geometry":{"type":"LineString","coordinates":[[108.0,36.0],[108.1,36.1]]}},
      {"type":"Feature","properties":{"type":"direction_line"},
       "geometry":{"type":"LineString","coordinates":[[108.0,36.1],[108.1,36.0]]}}]})");
    file.close();
    QString error;
    QVERIFY(!f.work.importConstraints("D61", path, "auto", &error));
    QVERIFY2(error.contains(QStringLiteral("局部米制坐标")), qPrintable(error));
    QVERIFY(f.constraints.loadConstraints("D61").isEmpty());
  }
  void removeProductDeletesAssetDeclarationsAndFiles() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto source = f.dir.filePath("del-samples.geojson");
    QVERIFY(MappingArtifactWriter::points(
        source,
        {QVariantMap{{"x", 0}, {"y", 0}, {"z", 10}},
         QVariantMap{{"x", 100}, {"y", 0}, {"z", 20}},
         QVariantMap{{"x", 0}, {"y", 100}, {"z", 40}},
         QVariantMap{{"x", 100}, {"y", 100}, {"z", 60}}},
        &error));
    LayerDeclaration samples;
    samples.layerId = "wells.D61";
    samples.horizon = "D61";
    samples.type = "vector";
    samples.source = source;
    samples.title = "删除样点";
    QVERIFY(f.layers.declare(samples, &error));
    const QVariantMap options{
        {"pointsLayerId", samples.layerId}, {"field", "z"}, {"cellSize", 10}};
    QVERIFY2(f.constraints.generateFactor("D61", "sandthick", options, &error),
             qPrintable(error));
    const auto v1 = f.work.versionForLayer("factor.D61.sandthick");
    const auto layer1 = "product." + v1.id;
    QTRY_VERIFY(!f.work.declaration(layer1).layerId.isEmpty());
    QVERIFY2(f.constraints.generateFactor("D61", "sandthick", options, &error),
             qPrintable(error));
    const auto v2 = f.work.versionForLayer("factor.D61.sandthick");
    const auto layer2 = "product." + v2.id;
    QTRY_VERIFY(!f.work.declaration(layer2).layerId.isEmpty());
    QCOMPARE(v1.assetId, v2.assetId); // 同资产两版本各自成行

    const auto managed = f.work.declaration(layer1).source.section('|', 0, 0);
    QVERIFY(QFile::exists(managed));

    // 非图件声明不受理：底图/基础图层走数据管理。
    QVERIFY(!f.work.removeProduct("wells.D61", &error));
    QVERIFY2(f.work.removeProduct(layer1, &error), qPrintable(error));
    QVERIFY(f.catalog.assetById(v1.assetId).id.isEmpty());
    QVERIFY(f.catalog.versionById(v1.id).id.isEmpty());
    QVERIFY(f.catalog.versionById(v2.id).id.isEmpty());
    QVERIFY(f.work.declaration(layer1).layerId.isEmpty());
    QVERIFY(f.work.declaration(layer2).layerId.isEmpty()); // 同资产声明一并下架
    QVERIFY(!QFile::exists(managed));
    for (const auto &row : f.work.products("D61"))
      QVERIFY(row.toMap().value("id").toString() != layer1);
  }
  void removeProductRefusesWhileReferenced() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto source = f.dir.filePath("ref-samples.geojson");
    QVERIFY(MappingArtifactWriter::points(
        source,
        {QVariantMap{{"x", 0}, {"y", 0}, {"z", 10}},
         QVariantMap{{"x", 100}, {"y", 0}, {"z", 20}}},
        &error));
    LayerDeclaration samples;
    samples.layerId = "wells.D61";
    samples.horizon = "D61";
    samples.type = "vector";
    samples.source = source;
    samples.title = "样点";
    QVERIFY(f.layers.declare(samples, &error));
    QVERIFY2(f.constraints.generateFactor(
                 "D61", "sandthick",
                 {{"pointsLayerId", samples.layerId},
                  {"field", "z"},
                  {"cellSize", 10}},
                 &error),
             qPrintable(error));
    const auto v1 = f.work.versionForLayer("factor.D61.sandthick");
    const auto layer1 = "product." + v1.id;
    QTRY_VERIFY(!f.work.declaration(layer1).layerId.isEmpty());

    // 下游资产的版本把目标版本列为 parent → 拒绝并点名下游。
    CatalogAsset child;
    child.id = "ast-downstream";
    child.type = "vector";
    child.displayName = "下游图件";
    QVERIFY(f.catalog.addAsset(child, &error));
    CatalogVersion cv;
    cv.id = "v-downstream";
    cv.assetId = child.id;
    cv.versionNumber = 1;
    cv.stage = "DERIVED";
    cv.path = "x.gpkg";
    cv.parentVersionIds = {v1.id};
    QVERIFY(f.catalog.addVersion(cv, &error));
    QVERIFY(!f.work.removeProduct(layer1, &error));
    QVERIFY(error.contains("下游图件"));
    QVERIFY(f.catalog.removeAsset(child.id, &error));
    QVERIFY2(f.work.removeProduct(layer1, &error), qPrintable(error));
    QVERIFY(f.catalog.assetById(v1.assetId).id.isEmpty());
  }
  void removeDraftDeletesWorkingCopy() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto draft = hierarchyDraft(f, &error);
    QVERIFY(!draft.isEmpty());
    const auto workDir =
        f.dir.filePath(QStringLiteral("artifacts/working/") + draft);
    QVERIFY(QDir(workDir).exists());
    const auto draftVersion = f.work.versionForLayer(draft);
    QVERIFY(!draftVersion.assetId.isEmpty());
    QVERIFY2(f.work.removeProduct(draft, &error), qPrintable(error));
    QVERIFY(f.work.declaration(draft).layerId.isEmpty());
    QVERIFY(f.catalog.assetById(draftVersion.assetId).id.isEmpty());
    // #499：工作副本目录的物理清理在 removeProduct 里经 QtConcurrent 后台执行
    // （「主线程只提交目录并释放 QGIS provider，物理清理后台跑」）——返回时
    // 目录可能尚未删除。立即断言会与清理任务竞态；Windows 上调度与文件系统
    // 延迟更大，CI windows 车道因此稳定红。按异步语义等待，而不是放宽断言。
    QTRY_VERIFY_WITH_TIMEOUT(!QDir(workDir).exists(), 15000);
  }
  void deleteButtonGatingFollowsSelectedRow() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto source = f.dir.filePath("gate-samples.geojson");
    QVERIFY(MappingArtifactWriter::points(
        source,
        {QVariantMap{{"x", 0}, {"y", 0}, {"z", 10}},
         QVariantMap{{"x", 100}, {"y", 0}, {"z", 20}}},
        &error));
    LayerDeclaration samples;
    samples.layerId = "wells.D61";
    samples.horizon = "D61";
    samples.type = "vector";
    samples.source = source;
    samples.title = "门控样点";
    QVERIFY(f.layers.declare(samples, &error));
    QVERIFY2(f.constraints.generateFactor(
                 "D61", "sandthick",
                 {{"pointsLayerId", samples.layerId},
                  {"field", "z"},
                  {"cellSize", 10}},
                 &error),
             qPrintable(error));
    const auto v1 = f.work.versionForLayer("factor.D61.sandthick");
    const auto layer1 = "product." + v1.id;
    QTRY_VERIFY(!f.work.declaration(layer1).layerId.isEmpty());

    MappingWorkbenchPage page("compose", &f.work);
    page.setHorizon("D61");
    // 无选中 → 禁用；源数据行（非 product./draft.）→ 禁用。
    QVERIFY(!page.commandButton("delete")->isEnabled());
    page.selectLayer("wells.D61");
    QVERIFY(!page.commandButton("delete")->isEnabled());
    page.selectLayer(layer1);
    QVERIFY(page.commandButton("delete")->isEnabled());
    QSignalSpy intent(&page, &MappingWorkbenchPage::commandRequested);
    page.commandButton("delete")->click();
    QCOMPARE(intent.last()[0].toString(), QString("delete"));
    QCOMPARE(intent.last()[1].toMap().value("layer").toString(), layer1);
  }
};
int main(int argc, char **argv) {
  // ctest 起进程不带控制台，QTest 结果行走 OutputDebugString；一旦某用例
  // __fastfail（0xc0000409）缓冲的 stdout 也一起丢，ctest --output-on-failure
  // 什么都看不到。故追加 `-o <file>,txt` 让每条用例结果落盘，崩到哪条一目了然。
  const QByteArray logPath = QByteArray(QT_TESTCASE_BUILDDIR) + "/tst_mappingworkbench-result.txt";
  QgsApplication app(argc, argv, true);
  app.setPrefixPath("/usr", true);
  app.initQgis();
  GDALAllRegister();
  TestMappingWorkbench test;
  QList<QByteArray> forwarded;
  forwarded << QByteArray(argv[0]);
  bool hasFileLogger = false;
  for (int i = 1; i < argc; ++i) {
    forwarded << QByteArray(argv[i]);
    // 调用方（ctest/CI）已自带文件日志时不重复追加，避免两个 -o 的歧义。
    if (QByteArray(argv[i]) == "-o" || QByteArray(argv[i]) == "-output")
      hasFileLogger = true;
  }
  if (!hasFileLogger)
    forwarded << QByteArray("-o") << logPath + ",txt";
  QList<char *> cargv;
  cargv.reserve(forwarded.size());
  for (QByteArray &a : forwarded)
    cargv << a.data();
  const int result = QTest::qExec(&test, cargv.size(), cargv.data());
  QgsApplication::exitQgis();
  return result;
}
#include "tst_mappingworkbench.moc"
