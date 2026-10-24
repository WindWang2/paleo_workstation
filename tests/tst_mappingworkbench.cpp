#include "../src/ai/remotepredictconfig.h"
#include "../src/app/aiwiring.h"
#include "../src/domain/faciescatalog.h"
#include "../src/domain/faciestaxonomy.h"
#include "../src/domain/facieshierarchy.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/facieshierarchyrenderer.h"
#include "../src/qgis/faciespartitioneditor.h"
#include "../src/qgis/faciespartitionstate.h"
#include "../src/qgis/faciespartitiontool.h"
#include "../src/qgis/mapcanvaslink.h"
#include "../src/qgis/mappingreferencecanvas.h"
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
#include "../src/ui/pages/mappingeditbasis.h"
#include "../src/ui/dialogs/dataexportdialog.h"
#include <QLineEdit>
#include "../src/ui/faciespinmenu.h"
#include "../src/ui/faciespinnedlevellist.h"
#include <QMenu>
#include <qgscollapsiblegroupbox.h>
#include <QGroupBox>
#include <QScrollBar>
#include <QWheelEvent>
#include "../src/ui/pages/mappingreferencepanel.h"
#include "../src/ui/pages/wellpredictionpanel.h"
#include "../src/ui/paleotheme.h"
#include "../src/ui/faciesdisplaycontrol.h"
#include "../src/services/faciesdisplaysettings.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QScrollArea>
#include <QSpinBox>
#include <QPushButton>
#include <QSettings>
#include <QScopeGuard>
#include <QProcess>
#include <QProcessEnvironment>
#include "../src/ui/paleomainwindow.h"
#include "../src/ui/decorations/paleodecorations.h"
#include "../src/ui/wellcomposite/wellcompositecanvas.h"
#include "../src/io/constraintstore.h"
#include "../src/workflow/derivedassets.h"
#include "../src/workflow/mappingworkbench.h"
#include "../src/workflow/wellpredictionservice.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QThreadPool>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <cmath>
#include <qgslayertree.h>
#include <qgslayertreemodellegendnode.h>
#include <qgsmaplayerlegend.h>
#include <qgsmaplayer.h>
#include <qgsproject.h>
#include <qgsrasterdataprovider.h>
#include <qgseditorwidgetsetup.h>
#include "../src/workflow/workflows.h"
#include "mockremotepredictionservice.h"
#include "seismichorizonfixture.h"
#include "../src/qgis/seismicwellcalibration.h"
#include <QAction>
#include <QComboBox>
#include <QCheckBox>
#include <QToolButton>
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
  bool hold = false, failPredict = false, queued = false, holdPoll = false, holdModels = false;
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
            if (holdModels) { buffer->clear(); return; }
            if (emptyModels)
              response = {{"models", QJsonArray{}}};
            else
              response = {{"models", QJsonArray{QJsonObject{
                {"id", "m1"}, {"name", "test-model"}, {"version", "v2"},
                {"inputSchema", QJsonObject{{"formationGroup", ""}, {"curves", QJsonArray{"GR"}},
                  {"labels", QJsonArray{QStringLiteral("河口坝"), QStringLiteral("分流河道")}},
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
    const QStringList children{"河道", "河道", "河道", "河道", "浅湖"};
    const QStringList leaves{"主槽", "边滩", "主槽", "边滩", "湖泥"};
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
  void directWellPredictionRejectsUnsafeTextIndex_data() {
    QTest::addColumn<QByteArray>("axis");
    QTest::newRow("unit-missing") << QByteArray("MD");
    QTest::newRow("unit-unknown") << QByteArray("MD[league]");
    QTest::newRow("time") << QByteArray("TIME[ms]");
    QTest::newRow("tvd") << QByteArray("TVD[m]");
  }
  void directWellPredictionRejectsUnsafeTextIndex() {
    QFETCH(QByteArray, axis);
    WellApiServer server;
    WellApiEnvironment environment(server);
    Fixture fixture;
    QVERIFY(fixture.init());
    QVERIFY(!fixture.well(QStringLiteral("A"), 10, 20).isEmpty());
    for (const auto &asset : fixture.catalog.assets()) {
      if (asset.type != QLatin1String("well_log")) continue;
      const auto versions = fixture.catalog.versionsForAsset(asset.id);
      QVERIFY(!versions.isEmpty());
      const QString managedLogPath =
          DataCatalog::resolvedVersionPath(fixture.dir.path(), versions.first());
      // 受管字节落盘即只读（dataimport_session）——模拟来源内容被替换前先放开写权限。
      QFile::setPermissions(managedLogPath,
                            QFile::permissions(managedLogPath) | QFileDevice::WriteOwner);
      QFile file(managedLogPath);
      QVERIFY(file.open(QIODevice::WriteOnly));
      const QByteArray text = axis + ",GR[API]\n1000,20\n1001,30\n";
      QCOMPARE(file.write(text), qint64(text.size()));
    }
    WellPredictionService service;
    service.bindCatalog(&fixture.catalog, fixture.dir.path());
    qRegisterMetaType<RemotePredictionResult>();
    QSignalSpy completed(&service, &RemotePredictionService::completed);
    QSignalSpy failed(&service, &RemotePredictionService::failed);
    RemotePredictionRequest request;
    request.id = QStringLiteral("strict-md");
    request.kind = QStringLiteral("wells");
    request.horizon = QStringLiteral("D61");
    request.facies = fixture.work.facies(request.horizon);
    const QVariantMap formation{{QStringLiteral("formation"), QStringLiteral("D61")},
        {QStringLiteral("top_md"), 1000.0}, {QStringLiteral("base_md"), 1100.0},
        {QStringLiteral("unit_type"), QStringLiteral("段")}};
    request.wells = {QVariantMap{{QStringLiteral("id"), QStringLiteral("A")},
        {QStringLiteral("name"), QStringLiteral("A")},
        {QStringLiteral("vector_formations"), QVariantList{formation}}}};
    service.start(request);
    QTRY_VERIFY_WITH_TIMEOUT(!completed.isEmpty() || !failed.isEmpty(), 10000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().at(1).toString()));
    QCOMPARE(server.modelRequests, 0);
    QVERIFY(server.bodies.isEmpty());
    const auto result = completed.first().first().value<RemotePredictionResult>();
    QCOMPARE(result.points.size(), 1);
    const auto point = result.points.first().toMap();
    QCOMPARE(point.value(QStringLiteral("prediction_status")).toString(), QStringLiteral("未预测"));
    QVERIFY(point.value(QStringLiteral("prediction_reason")).toString().contains(QStringLiteral("MD")));
  }
  void predictionPreparationRejectsNonMdTextWithoutMockFallback() {
    Fixture fixture;
    QVERIFY(fixture.init());
    QVERIFY(!fixture.well(QStringLiteral("A"), 10, 20).isEmpty());
    bool replaced = false;
    for (const auto &asset : fixture.catalog.assets()) {
      if (asset.type != QLatin1String("well_log")) continue;
      const auto versions = fixture.catalog.versionsForAsset(asset.id);
      QVERIFY(!versions.isEmpty());
      const QString managedLogPath =
          DataCatalog::resolvedVersionPath(fixture.dir.path(), versions.first());
      // 受管字节落盘即只读（dataimport_session）——模拟来源内容被替换前先放开写权限。
      QFile::setPermissions(managedLogPath,
                            QFile::permissions(managedLogPath) | QFileDevice::WriteOwner);
      QFile file(managedLogPath);
      QVERIFY(file.open(QIODevice::WriteOnly));
      const QByteArray text("TIME[ms],GR[API]\n1000,20\n1001,30\n");
      QCOMPARE(file.write(text), qint64(text.size()));
      file.close();
      replaced = true;
    }
    QVERIFY(replaced);
    QSignalSpy errors(&fixture.work, &MappingWorkbench::errorOccurred);
    QString error;
    QVERIFY2(fixture.work.predict(QStringLiteral("D61"), QStringLiteral("wells"), {QStringLiteral("A")}, &error), qPrintable(error));
    QTRY_VERIFY(!fixture.work.busy());
    QCOMPARE(errors.count(), 1);
    QVERIFY(errors.first().first().toString().contains(QStringLiteral("MD")));
    QVERIFY(fixture.latest(QStringLiteral("wells_prediction")).isEmpty());
  }
  void predictionWellWithoutLogFallsBackToMockDepth() {
    // R2-M1 回归：有坐标、无测井链接的井必须走 depth_mock 合成井段（0/120），
    // 不以空文件名报「需要 MD 测深」（C-8 重构曾丢 !path.isEmpty() 守卫）。
    Fixture fixture;
    QVERIFY(fixture.init());
    CatalogEntity e;
    e.id = QStringLiteral("MOCKW");
    e.name = QStringLiteral("MOCKW");
    e.entityType = QStringLiteral("well");
    e.hasSurface = true;
    e.surfaceX = 10;
    e.surfaceY = 20;
    e.coordinateStatus = QStringLiteral("untransformed");
    QVERIFY(fixture.catalog.addEntity(e));
    QVERIFY(fixture.tops(e.id, QStringLiteral(
        "#WellTops File From SMI\n#WellName Name MD X Y Z TVD Time(ms)\n"
        "MOCKW D61 1000 0 0 -1000 1000 -99999\n")));
    QSignalSpy errors(&fixture.work, &MappingWorkbench::errorOccurred);
    QString error;
    QVERIFY2(fixture.work.predict(QStringLiteral("D61"), QStringLiteral("wells"), {e.id}, &error), qPrintable(error));
    QTRY_VERIFY(!fixture.work.busy());
    QCOMPARE(errors.count(), 0);
  }
  void predictionLogAcceptsUnifiedMdAndRejectsOtherAxes() {
    Fixture fixture;
    QVERIFY(fixture.init());
    const auto add = [&](const QString &id, const QByteArray &bytes) -> QString {
      const QString path = fixture.dir.filePath(id + QStringLiteral(".csv"));
      QFile file(path);
      if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return {};
      file.close();
      CatalogAsset asset;
      asset.id = id;
      asset.type = QStringLiteral("well_log");
      asset.format = QStringLiteral("csv");
      CatalogVersion version;
      version.id = id + QStringLiteral("-v");
      version.assetId = id;
      version.stage = QStringLiteral("RAW");
      version.versionNumber = 1;
      version.managed = false;
      version.path = path;
      version.fileName = QFileInfo(path).fileName();
      if (!fixture.catalog.addAsset(asset) || !fixture.catalog.addVersion(version)) return {};
      return version.id;
    };
    const auto md = add(QStringLiteral("md"), "MD[ft],GR[API]\n1000,20\n1001,30\n");
    QVERIFY(!md.isEmpty());
    const auto log = fixture.work.predictionLog(md);
    QVERIFY2(log.ok, qPrintable(log.error));
    QCOMPARE(log.curveNames, QStringList({"MD", "GR"}));
    QVERIFY(std::abs(log.curves.first().values.first() - 304.8) < 1e-8);
    QCOMPARE(log.curves.first().unit, QStringLiteral("m"));
    // C-7：英尺 LAS 与文本曲线同口径归一到米——预测区间入库侧已归一，
    // 对比视图跳过 LAS 会把英尺深度与米制相带画在同一根轴上。
    const auto lasOf = [&fixture](const QString &id, const char *unit) -> QString {
      const QString path = fixture.dir.filePath(id + QStringLiteral(".las"));
      QFile file(path);
      const QByteArray body =
          QByteArray("~V\nVERS. 2.0:\n~W\nWELL. FTWELL:\n~C\nDEPT.") + unit +
          QByteArray(" :\nGR.API :\n~A\n1000 20\n1001 30\n");
      if (!file.open(QIODevice::WriteOnly) || file.write(body) != body.size()) return {};
      file.close();
      CatalogAsset asset;
      asset.id = id;
      asset.type = QStringLiteral("well_log");
      asset.format = QStringLiteral("las");
      CatalogVersion version;
      version.id = id + QStringLiteral("-v");
      version.assetId = id;
      version.stage = QStringLiteral("RAW");
      version.versionNumber = 1;
      version.managed = false;
      version.path = path;
      version.fileName = QFileInfo(path).fileName();
      if (!fixture.catalog.addAsset(asset) || !fixture.catalog.addVersion(version)) return {};
      return version.id;
    };
    const auto ftLas = lasOf(QStringLiteral("ftlas"), "FT");
    QVERIFY(!ftLas.isEmpty());
    const auto ftLog = fixture.work.predictionLog(ftLas);
    QVERIFY2(ftLog.ok, qPrintable(ftLog.error));
    QVERIFY(std::abs(ftLog.curves.first().values.first() - 304.8) < 1e-8);
    QCOMPARE(ftLog.curves.first().unit, QStringLiteral("m"));
    // 未知非空单位（DM）如实拒收，不静默当米。
    const auto dmLas = lasOf(QStringLiteral("dmlas"), "DM");
    QVERIFY(!dmLas.isEmpty());
    const auto dmLog = fixture.work.predictionLog(dmLas);
    QVERIFY(!dmLog.ok);
    QVERIFY(dmLog.error.contains(QStringLiteral("DM")));
    QVERIFY(dmLog.curves.isEmpty());
    for (const auto &axis : {QByteArray("TIME[ms]"), QByteArray("TVD[m]"), QByteArray("DEPTH[m]"), QByteArray("MD[league]")}) {
      const auto version = add(QString::fromLatin1(axis).replace(QLatin1Char('/'), QLatin1Char('_')), axis + ",GR[API]\n1000,20\n1001,30\n");
      QVERIFY(!version.isEmpty());
      const auto rejected = fixture.work.predictionLog(version);
      QVERIFY(!rejected.ok);
      QVERIFY(!rejected.error.isEmpty());
      QVERIFY(rejected.curves.isEmpty());
    }
  }
  void scaleSettingsAreRestoredInANewProcess() {
    auto &preferences = FaciesDisplaySettings::instance();
    if (qEnvironmentVariableIsSet("PALEO_SCALE_SETTINGS_CHILD")) {
      QCOMPARE(preferences.thresholds().faciesToSubfacies, 375000.);
      QCOMPARE(preferences.thresholds().subfaciesToMicrofacies, 75000.);
      return;
    }
    const auto previous = preferences.thresholds(); QString error;
    const auto restore = qScopeGuard([&] { QString ignored; preferences.save(previous, &ignored); });
    QVERIFY2(preferences.save({375000, 75000}, &error), qPrintable(error));
    QTemporaryDir dir;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("PALEO_SCALE_SETTINGS_CHILD", "1");
    QProcess process; process.setProcessEnvironment(environment);
    process.start(QCoreApplication::applicationFilePath(), {"scaleSettingsAreRestoredInANewProcess",
        "-o", dir.filePath("startup.txt") + ",txt"});
    QVERIFY(process.waitForFinished(30000));
    QFile log(dir.filePath("startup.txt")); QVERIFY(log.open(QIODevice::ReadOnly));
    const auto results = log.readAll();
    QVERIFY2(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0, results.constData());
  }
  void thresholdSettingsRefreshAutoLayersAndKeepFixedModes() {
    Fixture f; QVERIFY(f.init()); QString error;
    const auto previous = f.work.displayThresholds();
    const auto restore = qScopeGuard([&] { QString ignored;
      f.work.setDisplayThresholds(previous.faciesToSubfacies, previous.subfaciesToMicrofacies, &ignored); });
    const auto draft = hierarchyDraft(f, &error); QVERIFY2(!draft.isEmpty(), qPrintable(error));
    f.work.updateDisplayScale(100000);
    QVERIFY(f.work.setDisplayMode(draft, "auto", &error));
    QVERIFY(f.work.setDisplayThresholds(250000, 50000, &error));
    QCOMPARE(f.work.resolvedLevel(draft), QString("sub_facies"));
    QCOMPARE(f.work.displayLegend(draft).size(), 3); // this fixture has three subfacies paths
    QCOMPARE(f.work.displayLegend(draft).first().toMap().value("level").toString(), QString("sub_facies"));
    QVERIFY(f.work.setDisplayThresholds(75000, 25000, &error));
    QCOMPARE(f.work.resolvedLevel(draft), QString("facies"));
    QCOMPARE(f.work.displayLegend(draft).size(), 3);
    QCOMPARE(f.work.displayLegend(draft).first().toMap().value("level").toString(), QString("facies"));
    QSettings settings(QStringLiteral("paleo"), QStringLiteral("paleo"));
    QCOMPARE(settings.value("mapping/faciesToSubfaciesScale").toDouble(), 75000.);
    QCOMPARE(settings.value("mapping/subfaciesToMicrofaciesScale").toDouble(), 25000.);
    QVERIFY(f.work.setDisplayMode(draft, "micro_facies", &error));
    QVERIFY(f.work.setDisplayThresholds(200000, 150000, &error));
    f.work.updateDisplayScale(10000000);
    QCOMPARE(f.work.resolvedLevel(draft), QString("micro_facies"));
    QCOMPARE(f.work.displayLegend(draft).size(), 5);
    QVERIFY(!f.work.setDisplayThresholds(1000, 1000, &error));
    QCOMPARE(f.work.displayThresholds().faciesToSubfacies, 200000.);
  }
  void scaleSettingsDialogValidatesRestoresAndCancels() {
    FaciesDisplayControl control;
    control.setState("facies", "test", "auto", "sub_facies", {250000, 50000});
    auto *settings = control.findChild<QToolButton *>("ribbonFaciesScaleSettings"); QVERIFY(settings);
    QSignalSpy requested(&control, &FaciesDisplayControl::thresholdsRequested);
    QTimer::singleShot(5000, &control, [&] {
      if (auto *dialog = control.findChild<QDialog *>("faciesScaleSettingsDialog")) dialog->reject();
    });
    QTimer::singleShot(0, &control, [&] {
      auto *dialog = control.findChild<QDialog *>("faciesScaleSettingsDialog"); QVERIFY(dialog);
      auto *facies = dialog->findChild<QSpinBox *>("faciesScaleThreshold"); QVERIFY(facies);
      auto *micro = dialog->findChild<QSpinBox *>("microfaciesScaleThreshold"); QVERIFY(micro);
      auto *buttons = dialog->findChild<QDialogButtonBox *>(); QVERIFY(buttons);
      facies->setValue(100000); micro->setValue(100000);
      QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());
      buttons->button(QDialogButtonBox::RestoreDefaults)->click();
      QCOMPARE(facies->value(), 250000); QCOMPARE(micro->value(), 50000);
      QVERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());
      const auto qa = qEnvironmentVariable("PALEO_FACIES_SCALE_QA_DIR");
      if (!qa.isEmpty()) { QDir().mkpath(qa); QVERIFY(dialog->grab().save(qa + "/scale-settings.png")); }
      facies->setValue(500000); micro->setValue(100000);
      buttons->button(QDialogButtonBox::Ok)->click();
    });
    settings->click();
    QCOMPARE(requested.size(), 1); QCOMPARE(requested.first()[0].toDouble(), 500000.);
    QCOMPARE(requested.first()[1].toDouble(), 100000.);
    QTimer::singleShot(0, &control, [&] {
      auto *dialog = control.findChild<QDialog *>("faciesScaleSettingsDialog"); QVERIFY(dialog); dialog->reject();
    });
    settings->click(); QCOMPARE(requested.size(), 1);
  }
  void ribbonFixedFaciesTracksVisibleTargetAndRestoresAutomaticMode() {
    QObject context;
    Fixture f; QVERIFY(f.init()); f.project.setParent(&context);
    QgisCanvasController controller(&context);
    controller.setLayerResolver([&f](const QString &id) { return f.layers.layer(id); });
    SelectionContext selection; selection.setActiveHorizon("D61");
    PredictionWorkflow prediction(&f.processing, &f.layers);
    CompositionWorkflow composition(&f.processing, &f.layers);
    ValidationWorkflow validation(&f.layers, &f.store);
    PaleoMainWindow window(&controller, &f.project, &f.layers, nullptr, &selection);
    window.attachWorkflows(&prediction, &f.constraints, &composition, &validation,
        nullptr, nullptr, &f.processing, &f.store);
    window.attachWorkbench(&f.work); window.showPage("compose"); selection.setActiveHorizon("D61");
    const auto controls = window.findChildren<FaciesDisplayControl *>(); QCOMPARE(controls.size(), 3);
    QString error; const auto draft = hierarchyDraft(f, &error); QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft, &error)); QVERIFY(layer);
    auto *node = f.project.project()->layerTreeRoot()->findLayer(layer->id()); QVERIFY(node);
    node->setItemVisibilityCheckedParentRecursive(true); controller.canvas()->setCurrentLayer(layer);
    auto *mode = controls.first()->findChild<QComboBox *>("ribbonFaciesDisplayMode"); QVERIFY(mode);
    QTRY_VERIFY(mode->isEnabled());
    MappingWorkbenchPage *page = nullptr;
    for (auto *candidate : window.findChildren<MappingWorkbenchPage *>())
      if (candidate->mode() == "compose") page = candidate;
    QVERIFY(page); page->selectLayer(draft);
    auto *sideMode = page->findChild<QComboBox *>("faciesDisplayLevel"); QVERIFY(sideMode);
    // Real constraint snapshots freeze the same taxonomy as facies products.
    // Selecting one must leave the ribbon bound to the visible facies map.
    QVERIFY2(f.constraints.addConstraint("D61", "LINESTRING(50 0,50 100)", "line", -1, &error), qPrintable(error));
    const auto snapshot = f.work.snapshotConstraints("D61", {}, &error);
    QVERIFY2(!snapshot.isEmpty(), qPrintable(error));
    QVERIFY(!f.work.versionForLayer(snapshot).extra.value("facies").toList().isEmpty());
    auto *constraint = f.layers.instantiate(snapshot, &error); QVERIFY(constraint);
    auto *constraintNode = f.project.project()->layerTreeRoot()->findLayer(constraint->id()); QVERIFY(constraintNode);
    constraintNode->setItemVisibilityCheckedParentRecursive(true);
    node->setItemVisibilityCheckedParentRecursive(true); // productReady focuses the new snapshot
    controller.canvas()->setCurrentLayer(constraint);
    QTRY_VERIFY(controller.canvas()->layers().contains(constraint));
    QTRY_VERIFY(controller.canvas()->layers().contains(layer)); QTRY_VERIFY(mode->isEnabled());
    controller.canvas()->zoomScale(100000);
    // productReady selects the new snapshot. The parameter panel follows the
    // facies map while the canvas current layer stays the constraint.
    page->selectLayer(draft);
    QTRY_VERIFY(sideMode->isEnabled());
    auto *decorations = window.findChild<PaleoDecorationManager *>(); QVERIFY(decorations);
    const auto leaf = hierarchySchema().first().toMap();
    const auto feature = layer->getFeature(1);
    QVERIFY(feature.isValid());
    for (const auto &level : FaciesHierarchy::levels()) {
      mode->setCurrentIndex(mode->findData(level));
      QVERIFY(QMetaObject::invokeMethod(mode, "activated", Qt::DirectConnection, Q_ARG(int, mode->currentIndex())));
      QCOMPARE(f.work.displayMode(draft), level);
      QCOMPARE(layer->customProperty("paleo/faciesResolvedLevel").toString(), level);
      auto *merged = dynamic_cast<QgsMergedFeatureRenderer *>(layer->renderer()); QVERIFY(merged);
      auto *categories = dynamic_cast<const QgsCategorizedSymbolRenderer *>(merged->embeddedRenderer()); QVERIFY(categories);
      auto expressionContext = layer->createExpressionContext(); expressionContext.setFeature(feature);
      QCOMPARE(QgsExpression(categories->classAttribute()).evaluate(&expressionContext).toString(), FaciesHierarchy::key(leaf, level));
      auto *labeling = dynamic_cast<QgsVectorLayerSimpleLabeling *>(layer->labeling()); QVERIFY(labeling);
      const auto expected = FaciesHierarchy::legend(hierarchySchema(), level, {1, 2, 3, 4, 5});
      QCOMPARE(QgsExpression(labeling->settings().fieldName).evaluate(&expressionContext).toString(), expected.first().toMap().value("name").toString());
      QTRY_VERIFY2(decorations->legendTitle().contains(FaciesHierarchy::title(level)),
                   qPrintable(decorations->legendTitle()));
      QStringList expectedKeys, shownKeys;
      for (const auto &entry : expected)
        expectedKeys << entry.toMap().value("key").toString();
      for (const auto &entry : decorations->legendFacies()) {
        const auto row = entry.toMap();
        if (row.value("heading").toBool()) continue;
        shownKeys << row.value("key").toString();
        QCOMPARE(row.value("level").toString(), level);
      }
      expectedKeys.sort();
      shownKeys.sort();
      QCOMPARE(shownKeys, expectedKeys);
      QVERIFY(!constraint->customProperty("paleo/faciesDisplayMode").isValid());
      QTRY_COMPARE(sideMode->currentData().toString(), level);
      controller.canvas()->zoomScale(1000); QCOMPARE(f.work.resolvedLevel(draft), level);
      QCOMPARE(layer->customProperty("paleo/faciesResolvedLevel").toString(), level);
      controller.canvas()->zoomScale(10000000); QCOMPARE(f.work.resolvedLevel(draft), level);
      QCOMPARE(layer->customProperty("paleo/faciesResolvedLevel").toString(), level);
      for (auto *control : controls)
        QTRY_COMPARE(control->findChild<QComboBox *>("ribbonFaciesDisplayMode")->currentData().toString(), level);
      const auto qa = qEnvironmentVariable("PALEO_FACIES_SCALE_QA_DIR");
      if (level == "micro_facies" && !qa.isEmpty()) {
        window.resize(1600, 1000); window.show();
        controller.canvas()->setExtent(layer->extent()); controller.canvas()->refresh();
        QTest::qWait(200); QTRY_VERIFY(!controller.canvas()->isDrawing());
        QVERIFY(window.grab().save(qa + "/fixed-microfacies-with-constraint.png"));
      }
    }
    mode->setCurrentIndex(0);
    QVERIFY(QMetaObject::invokeMethod(mode, "activated", Qt::DirectConnection, Q_ARG(int, 0)));
    controller.canvas()->zoomScale(100000); QTRY_COMPARE(f.work.resolvedLevel(draft), QString("sub_facies"));
    const auto qa = qEnvironmentVariable("PALEO_FACIES_SCALE_QA_DIR");
    if (!qa.isEmpty()) { window.resize(1600, 1000); window.show(); QTest::qWait(80);
      QVERIFY(window.categoryForPage("compose")->grab().save(qa + "/ribbon-facies.png")); }
    layer->setScaleBasedVisibility(true); layer->setMinimumScale(200000); layer->setMaximumScale(50000);
    controller.canvas()->zoomScale(1000000); QTRY_VERIFY(!mode->isEnabled());
    layer->setScaleBasedVisibility(false);
    node->setItemVisibilityChecked(false); QTRY_VERIFY(!mode->isEnabled());
    QVERIFY(!f.work.setDisplayMode(snapshot, "micro_facies", &error));
    QVERIFY(!constraint->customProperty("paleo/faciesDisplayMode").isValid());
    page->selectLayer(snapshot); QTRY_VERIFY(!sideMode->isEnabled());
    selection.setActiveHorizon("D62"); node->setItemVisibilityCheckedParentRecursive(true);
    controller.canvas()->setCurrentLayer(layer); QTRY_VERIFY(!mode->isEnabled());
  }
  void predictionProductsKeepMicrofaciesAtEveryScale() {
    Fixture f; QVERIFY(f.init()); QString error;
    const auto schema = FaciesCatalog::defaults();
    DerivedAssetRegistrar registrar(&f.catalog, f.dir.path());
    for (const QString kind : {QStringLiteral("wells_prediction"), QStringLiteral("seismic_prediction")}) {
      const bool wells = kind == "wells_prediction";
      const auto staged = registrar.stage(kind, kind, wells ? "points.geojson" : "grid.tif", &error);
      QVERIFY(staged.isValid());
      if (wells)
        QVERIFY(MappingArtifactWriter::points(staged.absolutePath,
            {QVariantMap{{"x", 10}, {"y", 10}, {"facies_code", 1}},
             QVariantMap{{"x", 20}, {"y", 20}, {"facies_code", 2}}}, &error));
      else
        QVERIFY(MappingArtifactWriter::raster(staged.absolutePath, {1, 2}, 2, 1, QRectF(0, 0, 30, 30), &error));
      const QString id = "product." + staged.versionId;
      // Legacy projects recorded automatic display although the codes were microfacies.
      QVERIFY(registrar.commit(staged, {}, "fixture", {{"kind", kind}, {"facies", schema},
          {"horizon", "D61"}, {"layer_id", id}, {"mapping_product", true},
          {"prediction_level", "micro_facies"}, {"display_mode", "auto"}, {"mock", !wells}}, &error));
      LayerDeclaration declaration;
      declaration.layerId = id; declaration.horizon = "D61";
      declaration.type = wells ? "vector" : "raster";
      declaration.source = staged.absolutePath;
      QVERIFY(f.layers.declare(declaration, &error));
      auto *layer = f.layers.instantiate(id, &error);
      QVERIFY2(layer, qPrintable(error));
      layer->setCustomProperty("paleo/faciesDisplayMode", "auto");
      f.work.styleLayer(id);
      for (double scale : {1000., 100000., 10000000.}) {
        f.work.updateDisplayScale(scale);
        QCOMPARE(f.work.displayMode(id), QString("micro_facies"));
        QCOMPARE(f.work.resolvedLevel(id), QString("micro_facies"));
        QCOMPARE(layer->customProperty("paleo/faciesResolvedLevel").toString(), QString("micro_facies"));
        for (const auto &entry : f.work.displayLegend(id))
          QCOMPARE(entry.toMap().value("level").toString(), QString("micro_facies"));
      }
      QgsMapCanvas main;
      MappingReferenceCanvas reference(&main, f.project.project());
      reference.setReference(layer->clone(), {{"kind", kind}, {"facies", schema}, {"display_mode", "auto"}});
      for (double scale : {1000., 100000., 10000000.}) {
        reference.canvas()->zoomScale(scale);
        QCOMPARE(reference.canvas()->layers().first()->customProperty("paleo/faciesResolvedLevel").toString(), QString("micro_facies"));
      }
    }
  }
  void changedScaleSettingsRefreshExistingReferenceViews() {
    Fixture f; QVERIFY(f.init()); QString error;
    const auto previous = f.work.displayThresholds();
    const auto restore = qScopeGuard([&] { QString ignored;
      f.work.setDisplayThresholds(previous.faciesToSubfacies, previous.subfaciesToMicrofacies, &ignored); });
    QVERIFY(f.work.setDisplayThresholds(250000, 50000, &error));
    QgsMapCanvas main; MappingReferenceCanvas reference(&main, f.project.project());
    auto *layer = new QgsVectorLayer("Polygon?field=facies_code:integer&crs=EPSG:3857", "facies", "memory");
    const auto schema = hierarchySchema(); QgsFeatureList features;
    for (const auto &row : schema) {
      const int code = row.toMap().value("code").toInt();
      QgsFeature feature(layer->fields()); feature.setAttribute("facies_code", code);
      feature.setGeometry(QgsGeometry::fromRect(QgsRectangle(code * 100, 0, (code + 1) * 100, 100)));
      features << feature;
    }
    QVERIFY(layer->dataProvider()->addFeatures(features)); layer->updateExtents();
    reference.setReference(layer, {{"facies", schema}, {"display_mode", "auto"}});
    reference.canvas()->zoomScale(100000);
    QCOMPARE(layer->customProperty("paleo/faciesResolvedLevel").toString(), QString("sub_facies"));
    QVERIFY(f.work.setDisplayThresholds(75000, 25000, &error));
    QCOMPARE(layer->customProperty("paleo/faciesResolvedLevel").toString(), QString("facies"));
  }
  void canvasLegendFollowsVisibleFaciesInsteadOfTheCatalogue() {
    QObject context;
    Fixture f; QVERIFY(f.init());
    f.project.setParent(&context);
    QgisCanvasController controller(&context);
    controller.setLayerResolver([&f](const QString &id) { return f.layers.layer(id); });
    SelectionContext selection; selection.setActiveHorizon("D61");
    PaleoMainWindow window(&controller, &f.project, &f.layers, nullptr, &selection);
    window.attachWorkbench(&f.work);
    window.showPage("compose");
    selection.setActiveHorizon("D61");
    auto *decorations = window.findChild<PaleoDecorationManager *>();
    QVERIFY(decorations);
    QTRY_VERIFY(decorations->legendFacies().isEmpty());
    QString error;
    const auto draft = hierarchyDraft(f, &error); QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft, &error)); QVERIFY(layer);
    auto *node = f.project.project()->layerTreeRoot()->findLayer(layer->id()); QVERIFY(node);
    node->setItemVisibilityCheckedParentRecursive(true);
    controller.canvas()->setCurrentLayer(layer);
    QTRY_VERIFY(controller.canvas()->layers().contains(layer));
    QTRY_VERIFY(!decorations->legendFacies().isEmpty());
    node->setItemVisibilityChecked(false);
    QTRY_VERIFY(!controller.canvas()->layers().contains(layer));
    QTRY_VERIFY(decorations->legendFacies().isEmpty());
    node->setItemVisibilityCheckedParentRecursive(true);
    QTRY_VERIFY(!decorations->legendFacies().isEmpty());
    selection.setActiveHorizon("D62");
    QTRY_VERIFY(decorations->legendFacies().isEmpty());
    selection.setActiveHorizon("D61");
    QTRY_VERIFY(!decorations->legendFacies().isEmpty());
    QgsFeatureIds ids; auto iterator = layer->getFeatures(); QgsFeature feature;
    while (iterator.nextFeature(feature)) ids.insert(feature.id()); iterator.close();
    QVERIFY(layer->dataProvider()->deleteFeatures(ids)); layer->reload();
    QTRY_VERIFY(decorations->legendFacies().isEmpty());
  }
  void impactCommandsAreGatedWithReasons() {
    Fixture f;
    QVERIFY(f.init());
    MappingWorkbenchPage page("compose", &f.work);
    auto *impact = page.commandButton("impactAnalysis");
    auto *recompute = page.commandButton("recompute");
    auto *reverse = page.commandButton("reverseQuery");
    QVERIFY(impact && recompute && reverse);
    // 无层位/无选中/无影响报告：三按钮禁用且带 reason（full-audit F6——
    // 此前恒 enabled，空层位反查伪报「无匹配结果」、无选中影响分析误报
    // 「只有单一版本」）。
    QVERIFY(!impact->isEnabled());
    QVERIFY2(!impact->toolTip().isEmpty(),
             "disabled impactAnalysis needs a reason tooltip");
    QVERIFY(!recompute->isEnabled());
    QVERIFY2(!recompute->toolTip().isEmpty(),
             "disabled recompute needs a reason tooltip");
    QVERIFY(!reverse->isEnabled());
    QVERIFY2(!reverse->toolTip().isEmpty(),
             "disabled reverseQuery needs a reason tooltip");
    // 层位就绪：层位范围反查（默认 pending_review）解禁；影响分析仍缺选中。
    page.setHorizon("D61");
    QVERIFY(reverse->isEnabled());
    QVERIFY(!impact->isEnabled());
    QVERIFY(!recompute->isEnabled());
    // 影响分析成功回喂 → 受控重算解禁；回喂失败 → 重新禁用。
    page.setImpactAvailable(true);
    QVERIFY(recompute->isEnabled());
    page.setImpactAvailable(false);
    QVERIFY(!recompute->isEnabled());
  }

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
  void hierarchyPanelShowsAndPreservesTheTree() {
    Fixture f;
    QVERIFY(f.init());
    MappingWorkbenchPage page("compose", &f.work);
    page.setHorizon("D61");
    auto *tree = page.findChild<QTreeWidget *>("faciesHierarchyTree");
    QVERIFY(tree);
    QCOMPARE(tree->topLevelItemCount(), 4);
    QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("三角洲相"));
    QCOMPARE(tree->topLevelItem(0)->childCount(), 3);
    int leaves = 0;
    for (int root = 0; root < tree->topLevelItemCount(); ++root)
    for (int i = 0; i < tree->topLevelItem(root)->childCount(); ++i) {
      auto *branch = tree->topLevelItem(root)->child(i);
      for (int j = 0; j < branch->childCount(); ++j) {
        QVERIFY(!branch->child(j)->text(1).isEmpty());
        ++leaves;
      }
    }
    QCOMPARE(leaves, 27);
    QSignalSpy commands(&page, &MappingWorkbenchPage::commandRequested);
    auto *save = page.commandButton("schema");
    QVERIFY(save);
    save->click();
    QCOMPARE(commands.size(), 1);
    const auto schema = commands.last()[1].toMap().value("facies").toList();
    QCOMPARE(schema.size(), 27);
    QCOMPARE(schema.first().toMap().value("aliases").toStringList(),
             FaciesCatalog::defaults().first().toMap().value("aliases").toStringList());
    const auto screenshot = qEnvironmentVariable("PALEO_FACIES_HIERARCHY_SCREENSHOT");
    if (!screenshot.isEmpty()) {
      tree->setMinimumHeight(280);
      page.resize(620, 900); page.show(); QTest::qWait(100);
      QVERIFY(tree->grab().save(screenshot));
    }
    auto *table = page.findChild<QTableWidget *>("faciesSchema");
    QVERIFY(table);
    table->removeRow(0);
    int remaining = 0;
    for (int root = 0; root < tree->topLevelItemCount(); ++root)
    for (int i = 0; i < tree->topLevelItem(root)->childCount(); ++i)
      remaining += tree->topLevelItem(root)->child(i)->childCount();
    QCOMPARE(remaining, 26);
  }
  void optionalTemplateSelectionKeepsTheProjectCodeAndOtherRows() {
    Fixture f;
    QVERIFY(f.init());
    MappingWorkbenchPage page("compose", &f.work);
    page.setHorizon("D61");
    auto *library = page.findChild<QComboBox *>("faciesTextureLibrary");
    auto *table = page.findChild<QTableWidget *>("faciesSchema");
    auto *apply = page.findChild<QPushButton *>("applyFaciesTemplate");
    QVERIFY(library); QVERIFY(table); QVERIFY(apply);
    QCOMPARE(library->count(), 91);
    QCOMPARE(table->rowCount(), 27);
    int index = -1;
    for (int i = 0; i < library->count(); ++i)
      if (library->itemData(i).toMap().value("name").toString() == QStringLiteral("河道充填")) index = i;
    QVERIFY(index >= 0);
    table->item(0, 0)->setText("101");
    table->setCurrentCell(0, 1);
    library->setCurrentIndex(index);
    apply->click();
    QSignalSpy commands(&page, &MappingWorkbenchPage::commandRequested);
    page.commandButton("schema")->click();
    QCOMPARE(commands.size(), 1);
    const auto schema = commands.first()[1].toMap().value("facies").toList();
    QCOMPARE(schema.size(), 27);
    QCOMPARE(schema.first().toMap().value("code").toInt(), 101);
    QCOMPARE(schema.first().toMap().value("facies").toString(), QStringLiteral("河流相"));
    QCOMPARE(schema.first().toMap().value("subfacies").toString(), QStringLiteral("河床亚相"));
    QCOMPARE(schema.first().toMap().value("microfacies").toString(), QStringLiteral("河道充填"));
    for (int i = 1; i < schema.size(); ++i)
      QCOMPARE(schema[i].toMap().value("microfacies"), FaciesCatalog::defaults()[i].toMap().value("microfacies"));
    QString error;
    QVERIFY2(f.work.saveFacies("D61", schema, &error), qPrintable(error));
    QCOMPARE(f.work.facies("D61").first().toMap().value("code").toInt(), 101);
  }
  void incompleteClassificationsCannotBeSavedOrPredicted() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    QVERIFY(!f.work.saveFacies("D61", {QVariantMap{{"code", 1}, {"name", "三角洲相"}, {"color", "#97B4CE"}}}, &error));
    QVERIFY(error.contains(QStringLiteral("归属")));
    QVERIFY(f.work.products("D61").isEmpty());
  }
  void unknownMicrofaciesCannotBecomeAnEditingVersion() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto draft = hierarchyDraft(f, &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft, &error));
    QVERIFY(layer);
    const auto previous = f.work.versionForLayer(draft).id;
    auto it = layer->getFeatures();
    QgsFeature feature;
    QVERIFY(it.nextFeature(feature));
    it.close();
    QgsChangedAttributesMap changes;
    changes[feature.id()].insert(layer->fields().indexOf("facies_code"), 77);
    QVERIFY(layer->dataProvider()->changeAttributeValues(changes));
    QVERIFY(!f.work.saveEditingVersion(draft, &error));
    QVERIFY2(error.contains(QStringLiteral("微相编码")), qPrintable(error));
    QCOMPARE(f.work.versionForLayer(draft).id, previous);
  }
  void hierarchyScaleFallbackAndScopedNames() {
    QCOMPARE(FaciesHierarchy::resolveLevel("auto", 250001), QString("facies"));
    QCOMPARE(FaciesHierarchy::resolveLevel("auto", 250000), QString("sub_facies"));
    QCOMPARE(FaciesHierarchy::resolveLevel("auto", 249999),
             QString("sub_facies"));
    QCOMPARE(FaciesHierarchy::resolveLevel("auto", 50001),
             QString("sub_facies"));
    QCOMPARE(FaciesHierarchy::resolveLevel("auto", 50000),
             QString("micro_facies"));
    QCOMPARE(FaciesHierarchy::resolveLevel("auto", 49999),
             QString("micro_facies"));
    QCOMPARE(FaciesHierarchy::resolveLevel("facies", 1), QString("facies"));
    QCOMPARE(FaciesHierarchy::path({{"microfacies", "仅微相"}}),
             QStringList({"其他", "未归类", "仅微相"}));
    QCOMPARE(FaciesHierarchy::path({{"facies", "相"}, {"microfacies", "微相"}}),
             QStringList({"相", "未细分", "微相"}));
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
  void legacySchemaIsUpgradedOnRead() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    // 直接注入平级旧分类（绕过 saveFacies 校验，模拟合并前的旧工程目录）。
    {
      DerivedAssetRegistrar r(&f.catalog, f.dir.path());
      auto st = r.stage("facies_schema", QStringLiteral("facies-schema-D61"),
                        QStringLiteral("facies.json"), &error);
      QVERIFY2(st.isValid(), qPrintable(error));
      const QVariantList legacy{
          QVariantMap{{"code", 1}, {"name", "河流相"}, {"facies", "河流相"}, {"color", "#E8D5B0"}},
          QVariantMap{{"code", 2}, {"name", "三角洲相"}, {"facies", "三角洲相"}, {"color", "#E8D5B0"}},
          QVariantMap{{"code", 3}, {"name", "湖泊相"}, {"facies", "湖泊相"}, {"color", "#D5E8D0"}}};
      QFile file(st.absolutePath);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write(QJsonDocument::fromVariant(legacy).toJson());
      file.close();
      QVERIFY2(r.commit(st, {}, "fixture",
                        {{"horizon", "D61"}, {"facies", legacy}}, &error),
               qPrintable(error));
    }
    // 读出即迁移：三级恒有值，项目里不出现未定义相。
    const auto schema = f.work.facies("D61");
    QCOMPARE(schema.size(), 3);
    for (const auto &v : schema) {
      const auto m = v.toMap();
      QVERIFY2(!m.value("facies").toString().isEmpty(), "facies");
      QVERIFY2(!m.value("subfacies").toString().isEmpty(), "subfacies");
      QVERIFY2(!m.value("microfacies").toString().isEmpty(), "microfacies");
    }
    QCOMPARE(schema[1].toMap().value("facies").toString(), QStringLiteral("三角洲相"));
    QCOMPARE(schema[1].toMap().value("subfacies").toString(), QStringLiteral("未细分"));
    QCOMPARE(schema[2].toMap().value("facies").toString(), QStringLiteral("湖泊相"));
    // 升级后的分类通过正规校验并可回写——预测/编图不再被旧分类拒绝。
    QVariantList normalized;
    QVERIFY2(FaciesTaxonomy::normalize(schema, &normalized, &error),
             qPrintable(error));
    QVERIFY2(f.work.saveFacies("D61", schema, &error), qPrintable(error));
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
    f.work.updateDisplayScale(500000);
    QCOMPARE(f.work.resolvedLevel(draft), QString("facies"));
    QCOMPARE(f.work.displayLegend(draft).size(), 3);
  }
  void featureDisplayLockMixesLevelsWithoutChangingClassification() {
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
    // Same leaf code, different feature display levels: code alone cannot select a symbol.
    QVERIFY(f.work.assignHierarchy(draft, {b.id()}, 1, "micro_facies", &error));
    b = v->getFeature(b.id());
    const auto geometry = a.geometry().asWkb();
    QSignalSpy changed(&f.work, &MappingWorkbench::displayChanged);
    QVERIFY(f.work.setDisplayMode(draft, "auto", &error));
    QVERIFY2(f.work.setFeatureDisplayMode(draft, {a.id()}, "facies", &error), qPrintable(error));
    QVERIFY(!changed.isEmpty());
    const auto schema = f.work.versionForLayer(draft).extra.value("facies").toList();
    const auto leaf = FaciesCatalog::find(schema, 1);
    auto evaluated = [v](const QString &expression, qint64 fid) {
      auto context = v->createExpressionContext();
      context.setFeature(v->getFeature(fid));
      QgsExpression expr(expression);
      return expr.evaluate(&context).toString();
    };
    for (const auto scale : {500000.0, 100000.0, 25000.0, 500000.0, 25000.0}) {
      f.work.updateDisplayScale(scale);
      auto *merged = dynamic_cast<QgsMergedFeatureRenderer *>(v->renderer());
      QVERIFY(merged);
      const auto *categories = dynamic_cast<const QgsCategorizedSymbolRenderer *>(merged->embeddedRenderer());
      QVERIFY(categories);
      QCOMPARE(evaluated(categories->classAttribute(), a.id()), FaciesHierarchy::key(leaf, "facies"));
      QCOMPARE(evaluated(categories->classAttribute(), b.id()), FaciesHierarchy::key(leaf, f.work.resolvedLevel(draft)));
      QCOMPARE(evaluated(v->customProperty("paleo/faciesDisplayExpression").toString(), a.id()),
               FaciesHierarchy::path(leaf).first());
      const auto entries = f.work.displayLegend(draft);
      QCOMPARE(entries.size(), scale > 250000 ? 3 : scale > 50000 ? 4 : 5);
      QSet<QString> treeKeys;
      for (const auto &value : FaciesHierarchy::legendTree(entries)) {
        const auto key = value.toMap().value("key").toString();
        QVERIFY2(!treeKeys.contains(key), qPrintable(key));
        treeKeys.insert(key);
      }
      QgsLayerTreeLayer node(v);
      const auto nodes = v->legend()->createLayerTreeModelLegendNodes(&node);
      QCOMPARE(nodes.size(), treeKeys.size());
      QSet<QString> nativeKeys;
      for (const auto *legendNode : nodes) {
        const auto key = legendNode->data(int(QgsLayerTreeModelLegendNode::CustomRole::RuleKey)).toString();
        QVERIFY(!nativeKeys.contains(key));
        nativeKeys.insert(key);
      }
      QCOMPARE(nativeKeys, treeKeys);
      qDeleteAll(nodes);
      QCOMPARE(v->getFeature(a.id()).geometry().asWkb(), geometry);
      QCOMPARE(v->getFeature(a.id()).attribute("facies_code").toInt(), 1);
    }
    QCOMPARE(f.work.featureDisplayMode(draft, a.id()), QString("facies"));
    QCOMPARE(f.work.featureDisplayMode(draft, b.id()), QString("auto"));
    const auto before = v->undoStack()->count();
    QVERIFY(!f.work.setFeatureDisplayMode(draft, {a.id(), 999999}, "sub_facies", &error));
    QVERIFY(!f.work.setFeatureDisplayMode(draft, {a.id()}, "invalid", &error));
    QCOMPARE(v->undoStack()->count(), before);
    QVERIFY(f.work.setFeatureDisplayMode(draft, {a.id()}, "auto", &error));
    QCOMPARE(f.work.displayLegend(draft).size(), 4);
    v->undoStack()->undo();
    QCoreApplication::processEvents();
    QCOMPARE(f.work.featureDisplayMode(draft, a.id()), QString("facies"));
    QCOMPARE(f.work.displayLegend(draft).size(), 5);
    v->undoStack()->redo();
    QCoreApplication::processEvents();
    QCOMPARE(f.work.featureDisplayMode(draft, a.id()), QString("auto"));
    QVERIFY(v->rollBack());
    QCOMPARE(v->fields().indexOf("facies_display_level"), -1);
    QCOMPARE(f.work.displayLegend(draft).size(), 5);
  }
  void classPinsFixLevelPerFaciesSubtree() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto draft = hierarchyDraft(f, &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *v = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft));
    QVERIFY(v);
    const auto schema = f.work.versionForLayer(draft).extra.value("facies").toList();
    QHash<int, qint64> byCode;
    auto it = v->getFeatures();
    QgsFeature feature;
    while (it.nextFeature(feature)) byCode.insert(feature.attribute("facies_code").toInt(), feature.id());
    QCOMPARE(byCode.size(), 5);
    const auto keyOf = [v](qint64 fid) {
      const auto *merged = dynamic_cast<QgsMergedFeatureRenderer *>(v->renderer());
      const auto *categories = merged ? dynamic_cast<const QgsCategorizedSymbolRenderer *>(merged->embeddedRenderer()) : nullptr;
      if (!categories) return QString("<no renderer>");
      auto context = v->createExpressionContext();
      context.setFeature(v->getFeature(fid));
      QgsExpression expr(categories->classAttribute());
      return expr.evaluate(&context).toString();
    };
    const auto expected = [&schema](int code, const QString &level) {
      return FaciesHierarchy::key(FaciesCatalog::find(schema, code), level);
    };
    QVERIFY(f.work.setDisplayMode(draft, "auto", &error));
    f.work.updateDisplayScale(500000); // layer at 相
    const auto plainLegend = f.work.displayLegend(draft);
    QVERIFY(f.work.pinnedLevels(draft).isEmpty());
    const auto river = expected(1, "facies");
    QVERIFY2(f.work.setPinnedLevel(draft, river, "micro_facies", &error), qPrintable(error));
    QVERIFY2(f.work.setPinnedLevel(draft, expected(1, "micro_facies"), "sub_facies", &error), qPrintable(error));
    QCOMPARE(f.work.pinnedLevels(draft).size(), 2);
    for (const auto scale : {500000.0, 100000.0, 25000.0, 500000.0}) {
      f.work.updateDisplayScale(scale);
      const auto layerLevel = f.work.resolvedLevel(draft);
      QCOMPARE(keyOf(byCode[1]), expected(1, "sub_facies")); // nearest pin (leaf) wins
      QCOMPARE(keyOf(byCode[2]), expected(2, "micro_facies")); // pinned parent subtree
      QCOMPARE(keyOf(byCode[3]), expected(3, layerLevel)); // unpinned follows layer
      QCOMPARE(keyOf(byCode[5]), expected(5, layerLevel));
      QSet<QString> treeKeys;
      for (const auto &value : FaciesHierarchy::legendTree(f.work.displayLegend(draft))) {
        const auto key = value.toMap().value("key").toString();
        QVERIFY2(!treeKeys.contains(key), qPrintable(key));
        treeKeys.insert(key);
      }
      QVERIFY(treeKeys.contains(expected(2, "micro_facies")));
    }
    QVERIFY(f.work.setDisplayMode(draft, "micro_facies", &error));
    QCOMPARE(keyOf(byCode[2]), expected(2, "micro_facies"));
    QCOMPARE(keyOf(byCode[1]), expected(1, "sub_facies")); // pin beats fixed layer mode too
    // Explicit per-polygon locks still win over class pins.
    QVERIFY(f.work.setFeatureDisplayMode(draft, {byCode[2]}, "facies", &error));
    QCOMPARE(keyOf(byCode[2]), expected(2, "facies"));
    QCOMPARE(keyOf(byCode[1]), expected(1, "sub_facies"));
    QVERIFY(v->rollBack());
    // Changing an existing pin's level, invalid requests, and removal.
    QVERIFY(f.work.setPinnedLevel(draft, river, "facies", &error));
    QCOMPARE(keyOf(byCode[2]), expected(2, "facies"));
    QVERIFY(!f.work.setPinnedLevel(draft, river, "invalid", &error));
    QVERIFY(!f.work.setPinnedLevel(draft, "[\"不存在\"]", "facies", &error));
    QVERIFY(!f.work.setPinnedLevel(draft, "garbage", "facies", &error));
    QCOMPARE(f.work.pinnedLevels(draft).value(river).toString(), QString("facies"));
    QVERIFY(f.work.setPinnedLevel(draft, river, "auto", &error));
    QVERIFY(f.work.setPinnedLevel(draft, expected(1, "micro_facies"), "auto", &error));
    QVERIFY(f.work.pinnedLevels(draft).isEmpty());
    QCOMPARE(v->customProperty("paleo/faciesPinnedLevels").toString(), QString("{}"));
    f.work.updateDisplayScale(500000);
    QVERIFY(f.work.setDisplayMode(draft, "auto", &error));
    QCOMPARE(f.work.displayLegend(draft), plainLegend);
  }
  void classPinsPersistWithProjectAndOldProjectsLoadUnchanged() {
    Fixture f;
    QVERIFY(f.init());
    const auto input = f.seismic();
    QVERIFY(!input.isEmpty());
    QString error;
    QVERIFY2(f.work.predict("D61", "seismic", {input}, &error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(!f.work.busy(), 5000);
    const auto polygon = f.work.polygonize(f.latest("seismic_prediction"), &error);
    QVERIFY2(!polygon.isEmpty(), qPrintable(error));
    auto *v = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(polygon, &error));
    QVERIFY2(v, qPrintable(error));
    const auto candidates = FaciesHierarchy::pinCandidates(
        f.work.versionForLayer(polygon).extra.value("facies").toList());
    QVERIFY(!candidates.isEmpty());
    const auto restoredFirst = [](QgsVectorLayer *layer, QgsFeature &feature) {
      auto it = layer->getFeatures();
      return it.nextFeature(feature);
    };
    // Old project: no pin property at all -> loads with empty pins and unchanged legend.
    QVERIFY(f.project.writeProject());
    QVERIFY(f.project.openProject(f.project.projectPath()));
    auto *old = qobject_cast<QgsVectorLayer *>(f.layers.layer(polygon));
    QVERIFY(old);
    QVERIFY(!old->customProperty("paleo/faciesPinnedLevels").isValid());
    QVERIFY(f.work.pinnedLevels(polygon).isEmpty());
    // Pin the 相 ancestor of a class that is actually present in the polygon layer.
    QgsFeature first;
    QVERIFY(restoredFirst(old, first));
    const auto schema = f.work.versionForLayer(polygon).extra.value("facies").toList();
    const auto key = FaciesHierarchy::key(FaciesCatalog::find(schema, first.attribute("facies_code")), "facies");
    QVERIFY(!key.isEmpty());
    QVERIFY2(f.work.setPinnedLevel(polygon, key, "facies", &error), qPrintable(error));
    QVERIFY(f.project.writeProject());
    QVERIFY(f.project.openProject(f.project.projectPath()));
    auto *restored = qobject_cast<QgsVectorLayer *>(f.layers.layer(polygon));
    QVERIFY(restored);
    QTRY_COMPARE_WITH_TIMEOUT(f.work.pinnedLevels(polygon).value(key).toString(), QString("facies"), 5000);
    // Restyle after reopen honours the pin: every pinned-subtree legend row is 相.
    f.work.updateDisplayScale(25000);
    QVERIFY(f.work.setDisplayMode(polygon, "micro_facies", &error));
    bool sawPinned = false;
    for (const auto &entry : f.work.displayLegend(polygon)) {
      const auto row = entry.toMap();
      if (row.value("key").toString() == key) {
        sawPinned = true;
        QCOMPARE(row.value("level").toString(), QString("facies"));
      }
    }
    QVERIFY(sawPinned);
  }
  void classPinsCarryIntoSavedVersionsAndEditingCopies() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto draft = hierarchyDraft(f, &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *v = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft));
    QVERIFY(v);
    const auto schema = f.work.versionForLayer(draft).extra.value("facies").toList();
    const auto river = FaciesHierarchy::key(FaciesCatalog::find(schema, 1), "facies");
    QVERIFY2(f.work.setPinnedLevel(draft, river, "micro_facies", &error), qPrintable(error));
    QVERIFY2(f.work.saveEditingVersion(draft, &error), qPrintable(error));
    // The saved version record carries the pins (catalog extra, no GPKG field).
    const auto version = f.work.versionForLayer(draft);
    QCOMPARE(FaciesHierarchy::parsePins(version.extra.value("pinned_levels").toString()).value(river).toString(),
             QString("micro_facies"));
    QgsVectorLayer saved(DataCatalog::resolvedVersionPath(f.dir.path(), version) + "|layername=features", "saved", "ogr");
    QVERIFY(saved.isValid());
    QCOMPARE(saved.fields().indexOf("pinned_levels"), -1);
    // A new editing copy starts with the same pins and renders them.
    const auto copy = f.work.copyForEditing(draft, {}, &error);
    QVERIFY2(!copy.isEmpty(), qPrintable(error));
    auto *copied = f.layers.instantiate(copy);
    QVERIFY(copied);
    f.work.styleLayer(copy);
    QCOMPARE(f.work.pinnedLevels(copy).value(river).toString(), QString("micro_facies"));
    f.work.updateDisplayScale(10000000);
    bool pinnedRow = false;
    for (const auto &entry : f.work.displayLegend(copy))
      pinnedRow |= entry.toMap().value("key").toString() == FaciesHierarchy::key(FaciesCatalog::find(schema, 1), "micro_facies");
    QVERIFY(pinnedRow);
    // Explicitly clearing pins on the copy is not undone by the version record.
    QVERIFY(f.work.setPinnedLevel(copy, river, "auto", &error));
    f.work.styleLayer(copy);
    QVERIFY(f.work.pinnedLevels(copy).isEmpty());
    // Versions without the field (old projects) keep having no pins.
    QVERIFY(f.work.pinnedLevels("input.grid").isEmpty());
  }
  void classPinLiteralsKeepFieldTypes() {
    const QMap<QString, QString> pinned{{"3", "facies"}, {"O'1", "facies"}, {"7", "micro_facies"}};
    const auto literals = FaciesHierarchyRenderer::pinnedCodeLiterals(
        pinned, {QVariant(3), QVariant(3), QVariant(QString("O'1")), QVariant(7), QVariant()}, "facies");
    QCOMPARE(literals, (QStringList{"3", "'O''1'"}));
    // Rendering matches text and integer facies_code fields alike.
    const auto schema = hierarchySchema();
    for (const QString type : {QStringLiteral("integer"), QStringLiteral("string")}) {
      QgsVectorLayer layer(QStringLiteral("Polygon?field=facies_code:%1&crs=EPSG:3857").arg(type), "facies", "memory");
      QgsFeatureList features;
      for (int code = 1; code <= 3; ++code) {
        QgsFeature feature(layer.fields());
        feature.setAttribute("facies_code", type == "string" ? QVariant(QString::number(code)) : QVariant(code));
        feature.setGeometry(QgsGeometry::fromRect(QgsRectangle(code * 100, 0, (code + 1) * 100, 100)));
        features << feature;
      }
      QVERIFY(layer.dataProvider()->addFeatures(features));
      const auto pinKey = FaciesHierarchy::key(FaciesCatalog::find(schema, 1), "micro_facies");
      layer.setCustomProperty("paleo/faciesPinnedLevels",
                              FaciesHierarchy::encodePins({{pinKey, "micro_facies"}}));
      FaciesHierarchyRenderer::apply(&layer, schema, "facies");
      const auto *merged = dynamic_cast<QgsMergedFeatureRenderer *>(layer.renderer());
      const auto *categories = merged ? dynamic_cast<const QgsCategorizedSymbolRenderer *>(merged->embeddedRenderer()) : nullptr;
      QVERIFY(categories);
      auto it = layer.getFeatures();
      QgsFeature feature;
      while (it.nextFeature(feature)) {
        auto context = layer.createExpressionContext();
        context.setFeature(feature);
        QgsExpression expr(categories->classAttribute());
        const int code = feature.attribute("facies_code").toInt();
        QCOMPARE(expr.evaluate(&context).toString(),
                 FaciesHierarchy::key(FaciesCatalog::find(schema, code), code == 1 ? "micro_facies" : "facies"));
      }
    }
  }
  void referenceCanvasHonoursClassPins() {
    Fixture f; QVERIFY(f.init());
    QgsMapCanvas main; MappingReferenceCanvas reference(&main, f.project.project());
    auto *layer = new QgsVectorLayer("Polygon?field=facies_code:integer&crs=EPSG:3857", "facies", "memory");
    const auto schema = hierarchySchema(); QgsFeatureList features;
    for (const auto &row : schema) {
      const int code = row.toMap().value("code").toInt();
      QgsFeature feature(layer->fields()); feature.setAttribute("facies_code", code);
      feature.setGeometry(QgsGeometry::fromRect(QgsRectangle(code * 100, 0, (code + 1) * 100, 100)));
      features << feature;
    }
    QVERIFY(layer->dataProvider()->addFeatures(features)); layer->updateExtents();
    const auto pinKey = FaciesHierarchy::key(FaciesCatalog::find(schema, 1), "micro_facies");
    reference.setReference(layer, {{"facies", schema}, {"display_mode", "facies"},
                                   {"pinned_levels", FaciesHierarchy::encodePins({{pinKey, "micro_facies"}})}});
    QCOMPARE(FaciesHierarchy::parsePins(layer->customProperty("paleo/faciesPinnedLevels").toString()).size(), 1);
    QSet<QString> levels; bool pinnedRow = false;
    for (const auto &entry : layer->customProperty("paleo/faciesDisplayLegend").toList()) {
      levels.insert(entry.toMap().value("level").toString());
      pinnedRow |= entry.toMap().value("key").toString() == pinKey;
    }
    QVERIFY(pinnedRow);
    QVERIFY(levels.contains("facies"));
  }
  void faciesPinListShowsOnlyPinsWithRemoveAndLevel() {
    FaciesPinnedLevelList list;
    const QVariantList candidates{
        QVariantMap{{"key", "f:A"}, {"level", "facies"}, {"path", "河流"}},
        QVariantMap{{"key", "s:A/B"}, {"level", "sub_facies"}, {"path", "河流 / 河道"}}};
    auto *tree = list.findChild<QTreeWidget *>("faciesPinList");
    auto *empty = list.findChild<QLabel *>("faciesPinEmpty");
    QVERIFY(tree && empty);
    list.setState(candidates, {}, true, QString());
    QCOMPARE(tree->topLevelItemCount(), 0); // 只列已固定项
    QVERIFY(!empty->isHidden() && tree->isHidden());
    list.setState(candidates, {{"s:A/B", "facies"}, {"gone", "sub_facies"}}, true, QString());
    QVERIFY(empty->isHidden() && !tree->isHidden());
    QCOMPARE(tree->topLevelItemCount(), 2);
    QTreeWidgetItem *channel = nullptr, *stale = nullptr;
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
      (tree->topLevelItem(i)->data(0, Qt::UserRole) == "gone" ? stale : channel) = tree->topLevelItem(i);
    QVERIFY(channel && stale);
    QCOMPARE(channel->text(0), QString("河流 / 河道"));
    QVERIFY(stale->text(0).endsWith(QString("（不在当前分类中）")));
    auto *box = qobject_cast<QComboBox *>(tree->itemWidget(channel, 1));
    QVERIFY(box);
    QCOMPARE(box->count(), 3); // 相 / 亚相 / 微相，无「自动」
    QCOMPARE(box->currentData().toString(), QString("facies"));
    QSignalSpy pin(&list, &FaciesPinnedLevelList::pinRequested);
    emit box->activated(box->currentIndex()); // 未改选不发意图
    QCoreApplication::processEvents();
    QCOMPARE(pin.size(), 0);
    box->setCurrentIndex(box->findData("micro_facies")); // 调整层级
    emit box->activated(box->currentIndex());
    QTRY_COMPARE(pin.size(), 1);
    QCOMPARE(pin.last()[0].toString(), QString("s:A/B"));
    QCOMPARE(pin.last()[1].toString(), QString("micro_facies"));
    auto *remove = qobject_cast<QToolButton *>(tree->itemWidget(stale, 2)); // 删除（含陈旧固定）
    QVERIFY(remove && remove->objectName() == QString("faciesPinRowRemove"));
    remove->click();
    QTRY_COMPARE(pin.size(), 2);
    QCOMPARE(pin.last()[0].toString(), QString("gone"));
    QCOMPARE(pin.last()[1].toString(), QString("auto"));
    list.setState(candidates, {{"s:A/B", "facies"}}, false, QString());
    QVERIFY(!tree->isEnabled());
  }
  void canvasPinMenuPinsChangesAndUnpins() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto draft = hierarchyDraft(f, &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    const auto schema = f.work.versionForLayer(draft).extra.value("facies").toList();
    const int code = schema.first().toMap().value("code").toInt();
    const auto row = FaciesCatalog::find(schema, code);
    const auto apply = [&](const QString &key, const QString &mode) {
      QVERIFY2(f.work.setPinnedLevel(draft, key, mode, &error), qPrintable(error));
    };
    const auto open = [&](QMenu &menu) {
      return FaciesPinMenu::add(&menu, schema, code, f.work.pinnedLevels(draft), apply);
    };
    QMenu none;
    QVERIFY(!FaciesPinMenu::add(&none, schema, -12345, {}, apply)); // 不在分类中的代码不加菜单
    QVERIFY(none.actions().isEmpty());
    // 固定：未固定时只有三个层级动作，无「取消固定」。
    QMenu first;
    auto *sub = open(first);
    QVERIFY(sub && sub->objectName() == QString("pinFeatureFaciesLevel"));
    QVERIFY(!sub->findChild<QAction *>("unpinFacies"));
    for (const auto &level : FaciesHierarchy::levels())
      QVERIFY(!sub->findChild<QAction *>("pinFacies_" + level)->isChecked());
    sub->findChild<QAction *>("pinFacies_sub_facies")->trigger();
    const auto subKey = FaciesHierarchy::key(row, "sub_facies");
    QCOMPARE(f.work.pinnedLevels(draft), (QVariantMap{{subKey, "sub_facies"}}));
    // 改层级：再次右键，当前固定已勾选；选相即改该固定的层级（立即生效，不另加固定）。
    QMenu second;
    sub = open(second);
    QVERIFY(sub->findChild<QAction *>("pinFacies_sub_facies")->isChecked());
    QVERIFY(sub->findChild<QAction *>("unpinFacies"));
    sub->findChild<QAction *>("pinFacies_facies")->trigger();
    QCOMPARE(f.work.pinnedLevels(draft), (QVariantMap{{subKey, "facies"}}));
    QCOMPARE(FaciesHierarchy::pinnedLevel(f.work.pinnedLevels(draft), row), QString("facies"));
    // 取消固定：移除作用于此相面的最近一级固定。
    QMenu third;
    sub = open(third);
    QVERIFY(sub->findChild<QAction *>("pinFacies_facies")->isChecked());
    QVERIFY(!sub->findChild<QAction *>("pinFacies_sub_facies")->isChecked());
    sub->findChild<QAction *>("unpinFacies")->trigger();
    QVERIFY(f.work.pinnedLevels(draft).isEmpty());
    // 祖先节点上的固定同样作用于此相面，可从此处取消。
    const auto faciesKey = FaciesHierarchy::key(row, "facies");
    QVERIFY(f.work.setPinnedLevel(draft, faciesKey, "micro_facies", &error));
    QMenu fourth;
    sub = open(fourth);
    QVERIFY(sub->findChild<QAction *>("pinFacies_micro_facies")->isChecked());
    sub->findChild<QAction *>("unpinFacies")->trigger();
    QVERIFY(f.work.pinnedLevels(draft).isEmpty());
    // 页面列表随之列出 / 移除，且分组在折叠的外观组之外、默认展开。
    QVERIFY(f.work.setPinnedLevel(draft, subKey, "micro_facies", &error));
    MappingWorkbenchPage page("compose", &f.work);
    page.setHorizon("D61");
    page.selectLayer(draft);
    auto *group = page.findChild<QgsCollapsibleGroupBox *>("workbenchFaciesPins");
    QVERIFY(group && !group->isCollapsed());
    auto *list = page.findChild<QTreeWidget *>("faciesPinList");
    QVERIFY(list && group->isAncestorOf(list));
    QCOMPARE(list->topLevelItemCount(), 1);
    QCOMPARE(list->topLevelItem(0)->data(0, Qt::UserRole).toString(), subKey);
    QSignalSpy intent(&page, &MappingWorkbenchPage::commandRequested);
    qobject_cast<QToolButton *>(list->itemWidget(list->topLevelItem(0), 2))->click();
    QTRY_COMPARE(intent.size(), 1);
    QCOMPARE(intent.last()[0].toString(), QString("pinFaciesLevel"));
    QCOMPARE(intent.last()[1].toMap().value("pin_key").toString(), subKey);
    QCOMPARE(intent.last()[1].toMap().value("display_mode").toString(), QString("auto"));
    QVERIFY(f.work.setPinnedLevel(draft, subKey, "auto", &error));
    page.refresh();
    QCOMPARE(list->topLevelItemCount(), 0);
  }
  void ribbonDisplayControlIndicatesPinnedClasses() {
    FaciesDisplayControl control;
    auto *mode = control.findChild<QComboBox *>("ribbonFaciesDisplayMode"); QVERIFY(mode);
    auto *state = control.findChild<QLabel *>("ribbonFaciesDisplayState"); QVERIFY(state);
    control.setState("facies", "图件", "auto", "sub_facies", {250000, 50000});
    const auto plainText = state->text(); const auto plainTip = mode->toolTip();
    QCOMPARE(state->property("paleo.pinnedCount").toInt(), 0);
    control.setState("facies", "图件", "auto", "sub_facies", {250000, 50000}, 2);
    QCOMPARE(state->property("paleo.pinnedCount").toInt(), 2);
    QVERIFY(state->text() != plainText && state->text().contains("2"));
    QVERIFY(mode->toolTip().startsWith(plainTip));
    QCOMPARE(mode->count(), 4); QCOMPARE(mode->currentData().toString(), QString("auto"));
    QVERIFY(mode->isEnabled());
    // Selecting a mode still requests exactly that mode.
    QSignalSpy requested(&control, &FaciesDisplayControl::displayModeRequested);
    mode->setCurrentIndex(1); emit mode->activated(1);
    QCOMPARE(requested.size(), 1); QCOMPARE(requested.first().at(1).toString(), QString("facies"));
    control.setState(QString(), QString(), "auto", "sub_facies", {250000, 50000}, 3);
    QCOMPARE(state->property("paleo.pinnedCount").toInt(), 0);
  }
  void featureDisplayLocksSurviveSavedVersionsAndCopies() {
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
    QVERIFY(f.work.setFeatureDisplayMode(draft, {a.id(), b.id()}, "sub_facies", &error));
    QVERIFY2(v->commitChanges(), qPrintable(v->commitErrors().join(";")));
    QVERIFY2(f.work.saveEditingVersion(draft, &error), qPrintable(error));
    const auto version = f.work.versionForLayer(draft);
    QgsVectorLayer saved(DataCatalog::resolvedVersionPath(f.dir.path(), version) + "|layername=features", "saved", "ogr");
    QVERIFY(saved.isValid());
    auto stored = saved.getFeatures();
    QgsFeature feature;
    int locked = 0;
    while (stored.nextFeature(feature))
      locked += feature.attribute("facies_display_level").toString() == "sub_facies";
    QCOMPARE(locked, 2);
    const auto copy = f.work.copyForEditing(draft, {}, &error);
    QVERIFY2(!copy.isEmpty(), qPrintable(error));
    QVERIFY(f.layers.instantiate(copy));
    f.work.updateDisplayScale(25000);
    int sub = 0;
    for (const auto &entry : f.work.displayLegend(copy))
      sub += entry.toMap().value("level") == "sub_facies";
    QCOMPARE(sub, 1);
    QVERIFY(!f.work.setFeatureDisplayMode("product." + version.id, {a.id()}, "facies", &error));
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
  void partitionOperationsPreserveEvidenceHierarchyUndoAndVersions() {
    Fixture f; QVERIFY(f.init()); QString error;
    const auto draft = hierarchyDraft(f, &error); QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft)); QVERIFY(layer);
    const auto originalVersion = f.work.versionForLayer(draft);
    const auto originalHash = digest(DataCatalog::resolvedVersionPath(f.dir.path(), originalVersion));
    // #540 子项 2：冻结覆盖范围走 sidecar 文件，extra 只留路径 + 可读摘要。
    const auto footprintPath = originalVersion.extra.value("partition_footprint_path").toString();
    QVERIFY2(!footprintPath.isEmpty(), qPrintable(footprintPath));
    QVERIFY(QFile::exists(f.dir.filePath(footprintPath)));
    QVERIFY(originalVersion.extra.value("partition_footprint_wkb").toString().isEmpty());
    QVERIFY(!originalVersion.extra.value("partition_footprint").toMap().value("sha256").toString().isEmpty());
    auto it = layer->getFeatures(); QgsFeature first, second;
    QVERIFY(it.nextFeature(first)); QVERIFY(it.nextFeature(second)); it.close();
    QVERIFY(layer->startEditing()); layer->beginEditCommand("source boundary semantics");
    for (const auto &field : {QgsField("boundary_kind", QMetaType::Type::QString),
         QgsField("transition_width", QMetaType::Type::Double), QgsField("confidence", QMetaType::Type::Double),
         QgsField("comment", QMetaType::Type::QString)})
      QVERIFY(layer->addAttribute(field));
    for (const auto &source : {first, second}) {
      QVERIFY(layer->changeAttributeValue(source.id(), layer->fields().indexOf("boundary_kind"), "fault_cut"));
      QVERIFY(layer->changeAttributeValue(source.id(), layer->fields().indexOf("transition_width"), 0.));
      QVERIFY(layer->changeAttributeValue(source.id(), layer->fields().indexOf("confidence"), .9));
      QVERIFY(layer->changeAttributeValue(source.id(), layer->fields().indexOf("comment"), QString("原面备注 %1").arg(source.id())));
    }
    layer->endEditCommand();
    for (const auto &source : {first, second})
      QVERIFY2(f.work.addEvidence(draft, {source.id()}, {{"text", QString("井震解释 %1").arg(source.id())},
          {"source_layer", "input.grid"}, {"actor_id", "local:geologist"}, {"reason", "记录编辑前依据"}}, &error), qPrintable(error));
    layer->selectByIds({first.id()});
    const int beforeCommands = layer->undoStack()->count();
    QVERIFY2(FaciesPartitionEditor::split(layer, QgsGeometry::fromWkt("LINESTRING(-10 50,110 50)"), &error), qPrintable(error));
    QCOMPARE(layer->featureCount(), 6); QCOMPARE(layer->undoStack()->count(), beforeCommands + 1);
    for (const auto &child : layer->selectedFeatures()) {
      QCOMPARE(child.attribute("facies_code").toInt(), 1);
      const auto state = f.work.featureEvidence(draft, {child.id()});
      QCOMPARE(state.value("records").toList().size(), 1);
      QCOMPARE(state.value("interpretation").toMap().value("status").toString(), QString("NeedsReview"));
    }
    layer->undoStack()->undo(); QCOMPARE(layer->featureCount(), 5);
    QCOMPARE(MappingFeatureIdentity::uid(layer->getFeature(first.id())), MappingFeatureIdentity::uid(first));
    QVERIFY2(FaciesPartitionEditor::paint(layer, QgsGeometry::fromRect(QgsRectangle(50,10,150,90)), 3, &error), qPrintable(error));
    QCOMPARE(layer->selectedFeatureCount(), 1);
    const auto painted = layer->selectedFeatures().first();
    QCOMPARE(painted.attribute("facies_code").toInt(), 3);
    QCOMPARE(painted.attribute("facies_name").toString(), QString("三角洲"));
    QVERIFY(painted.attribute("boundary_kind").isNull()); QVERIFY(painted.attribute("confidence").isNull());
    QVERIFY(painted.attribute("comment").isNull());
    const auto evidence = f.work.featureEvidence(draft, {painted.id()});
    QCOMPARE(evidence.value("records").toList().size(), 2);
    QCOMPARE(evidence.value("interpretation").toMap().value("parent_interpretations").toList().size(), 2);
    for (const auto &parent : evidence.value("interpretation").toMap().value("parent_interpretations").toList()) {
      QCOMPARE(parent.toMap().value("source_feature_attributes").toMap().value("boundary_kind").toString(), QString("fault_cut"));
      QCOMPARE(parent.toMap().value("source_feature_attributes").toMap().value("confidence").toDouble(), .9);
      QVERIFY(parent.toMap().value("source_feature_attributes").toMap().value("comment").toString().startsWith("原面备注"));
    }
    const auto interpretation = painted.attribute("paleo_interpretation");
    const auto identity = MappingFeatureIdentity::uid(painted);
    layer->undoStack()->undo(); QCOMPARE(layer->featureCount(), 5);
    layer->undoStack()->redo();
    QCOMPARE(MappingFeatureIdentity::find(layer, identity).attribute("paleo_interpretation"), interpretation);
    QVERIFY2(layer->commitChanges(), qPrintable(layer->commitErrors().join(";")));
    QVERIFY2(f.work.saveEditingVersion(draft, &error), qPrintable(error));
    const auto saved = f.work.versionForLayer(draft);
    QVERIFY(saved.id != originalVersion.id);
    // 保存时沿用同一 sidecar（覆盖范围被 validate 钉住），extra 不再出现 WKB。
    QCOMPARE(saved.extra.value("partition_footprint_path"), originalVersion.extra.value("partition_footprint_path"));
    QCOMPARE(saved.extra.value("partition_footprint"), originalVersion.extra.value("partition_footprint"));
    QVERIFY(saved.extra.value("partition_footprint_wkb").toString().isEmpty());
    QCOMPARE(digest(DataCatalog::resolvedVersionPath(f.dir.path(), originalVersion)), originalHash);
    QgsVectorLayer reopened(DataCatalog::resolvedVersionPath(f.dir.path(), saved) + "|layername=features", "reopened", "ogr");
    QVERIFY(reopened.isValid());
    const auto restored = MappingFeatureIdentity::find(&reopened, identity); QVERIFY(restored.isValid());
    QCOMPARE(restored.attribute("paleo_interpretation"), interpretation);
    QVERIFY2(FaciesPartitionEditor::install(&reopened, saved.extra.value("facies").toList(),
                 FaciesPartitionState::loadFootprint(f.dir.path(), saved.extra), &error), qPrintable(error));
    QVERIFY2(FaciesPartitionEditor::validate(&reopened, &error), qPrintable(error));
    QVERIFY(layer->startEditing()); QgsFeatureIds mergeIds;
    it = layer->getFeatures(); QgsFeature feature;
    while (it.nextFeature(feature)) if (feature.geometry().boundingBox().xMaximum() <= 200) mergeIds.insert(feature.id());
    layer->selectByIds(mergeIds);
    QVERIFY2(FaciesPartitionEditor::merge(layer, 3, &error), qPrintable(error));
    QCOMPARE(layer->featureCount(), 4);
    const auto merged = layer->selectedFeatures().first();
    const auto mergedEvidence = f.work.featureEvidence(draft, {merged.id()});
    QVERIFY(mergedEvidence.value("records").toList().size() >= 2);
    QVERIFY2(layer->commitChanges(), qPrintable(layer->commitErrors().join(";")));
    QVERIFY2(f.work.saveEditingVersion(draft, &error), qPrintable(error));
    QCOMPARE(digest(DataCatalog::resolvedVersionPath(f.dir.path(), originalVersion)), originalHash);
    QVERIFY(f.work.setDisplayMode(draft, "facies", &error));
    QVERIFY(!f.work.displayLegend(draft).isEmpty());
    QVERIFY(f.work.setDisplayMode(draft, "micro_facies", &error));
  }
  void partitionToolbarGatesUnsafeToolsAndFollowsSelection() {
    Fixture f; QVERIFY(f.init()); QString error;
    const auto draft = hierarchyDraft(f, &error); QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft)); QVERIFY(layer);
    QgsMapCanvas canvas; PaleoEditingToolbar toolbar(&canvas);
    toolbar.setProject(f.project.project()); toolbar.setLayers({layer}); toolbar.setCurrentLayer(layer);
    QVERIFY(!toolbar.actionMove()->isEnabled()); QVERIFY(!toolbar.actionReshape()->isEnabled());
    QVERIFY(!toolbar.actionDeleteFeatures()->isEnabled()); QVERIFY(!toolbar.actionCrossLayerTopo()->isEnabled());
    QVERIFY(!toolbar.actionPaintFacies()->isEnabled()); QVERIFY(!toolbar.actionSplitFacies()->isEnabled());
    auto *code = toolbar.findChild<QComboBox *>("partitionFaciesCode"); QVERIFY(code);
    QCOMPARE(code->count(), 6); QVERIFY(code->itemText(1).contains("→"));
    code->setCurrentIndex(code->findData(3)); QVERIFY(toolbar.actionPaintFacies()->isEnabled());
    toolbar.actionAddPolygon()->trigger(); QVERIFY(qobject_cast<FaciesPartitionTool *>(canvas.mapTool()));
    auto it = layer->getFeatures(); QgsFeature a, b; QVERIFY(it.nextFeature(a)); QVERIFY(it.nextFeature(b)); it.close();
    layer->selectByIds({a.id()}); QVERIFY(toolbar.actionSplitFacies()->isEnabled());
    QVERIFY(!toolbar.actionMergeFacies()->isEnabled());
    toolbar.actionSplitFacies()->trigger(); QVERIFY(qobject_cast<FaciesPartitionTool *>(canvas.mapTool()));
    layer->selectByIds({a.id(), b.id()}); QVERIFY(toolbar.actionMergeFacies()->isEnabled());
    toolbar.actionMergeFacies()->trigger(); QCOMPARE(layer->featureCount(), 4);
    QVERIFY(!toolbar.actionSplitFacies()->isChecked());
    QVERIFY(!qobject_cast<FaciesPartitionTool *>(canvas.mapTool()));
    toolbar.actionUndo()->trigger(); QCOMPARE(layer->featureCount(), 5);
    toolbar.actionRedo()->trigger(); QCOMPARE(layer->featureCount(), 4);
    QVERIFY(toolbar.cancelEditing()); QCOMPARE(layer->featureCount(), 5);
  }
  // #541 子项 2：零要素相面（从零手绘的起点）建不出编辑副本——此前
  // copyForEditing 被 footprint() 的「无法建立相图覆盖范围」堵死。
  void mappingFlowOffersBlankBasisAndDelivery() {
    Fixture f; QVERIFY(f.init()); QString error;
    MappingWorkbenchPage page("compose", &f.work); page.setHorizon("D61");
    page.resize(500, 1000); page.show(); QCoreApplication::processEvents();
    auto *category = page.findChild<QComboBox *>("baseMapCategory"); QVERIFY(category);
    QCOMPARE(category->currentData().toString(), QString("new"));
    const auto starts = page.findChildren<QPushButton *>();
    QPushButton *start = nullptr;
    for (auto *button : starts) if (button->text() == QStringLiteral("新建并开始编制")) start = button;
    QVERIFY(start); QVERIFY(start->isEnabled());
    QSignalSpy commands(&page, &MappingWorkbenchPage::commandRequested);
    start->click(); QCOMPARE(commands.last().first().toString(), QString("newMap"));
    auto *advanced = page.findChild<QGroupBox *>("mappingComposeAdvanced"); QVERIFY(advanced);
    auto *toggle = page.findChild<QToolButton *>("mappingComposeToggle"); QVERIFY(toggle);
    QVERIFY(!toggle->isChecked());
    auto *text = page.findChild<QPlainTextEdit *>("editBasisText"); QVERIFY(text);
    auto *reason = page.findChild<QLineEdit *>("editBasisReason"); QVERIFY(reason);
    text->setPlainText(QStringLiteral("砂厚高值区与井相支持河道展布"));
    reason->setText(QStringLiteral("调整河道边界"));
    auto *apply = page.commandButton("setEditEvidence"); QVERIFY(apply); apply->click();
    const auto p = commands.last().at(1).toMap();
    QCOMPARE(commands.last().first().toString(), QString("setEditEvidence"));
    QVERIFY2(f.work.setEditEvidence("D61", p, &error), qPrintable(error));
    QVERIFY(page.findChild<QLabel *>("editBasisState")->text().contains(QStringLiteral("已启用")));
    QVERIFY(page.commandButton("checkTopology")); QVERIFY(page.commandButton("save"));
    QVERIFY(page.commandButton("exportVector")); QVERIFY(page.commandButton("exportMap"));
    QVERIFY(!page.commandButton("exportVector")->isEnabled());
    const auto qa = qEnvironmentVariable("PALEO_MAPPING_QA_DIR");
    if (!qa.isEmpty()) {
      PaleoTheme::applyLightTheme(); QCoreApplication::processEvents();
      QDir().mkpath(qa); QVERIFY(page.grab().save(QDir(qa).filePath("mapping-flow.png")));
      if (auto *scroll = page.findChild<QScrollArea *>()) {
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        QCoreApplication::processEvents(); QVERIFY(page.grab().save(QDir(qa).filePath("mapping-delivery.png")));
      }
    }
    if (!qa.isEmpty()) {
      PaleoTheme::applyDarkTheme();
      if (auto *scroll = page.findChild<QScrollArea *>()) scroll->verticalScrollBar()->setValue(0);
      QCoreApplication::processEvents(); QVERIFY(page.grab().save(QDir(qa).filePath("mapping-flow-dark.png")));
      PaleoTheme::applyLightTheme();
    }
    page.setHorizon("D62"); QVERIFY(f.work.editEvidence("D61").isEmpty());
  }
  void blankMapRequiresEvidenceAndKeepsUndoAndCoverage() {
    Fixture f; QVERIFY(f.init()); QString error;
    const auto draft = f.work.createBlankMap("D61", &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    const auto initial = f.work.versionForLayer(draft);
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft, &error));
    QVERIFY2(layer, qPrintable(error)); QCOMPARE(layer->featureCount(), 0);
    QVERIFY(layer->fields().indexOf("microfacies") >= 0);
    QVERIFY(layer->startEditing());
    const auto outline = QgsGeometry::fromWkt("POLYGON((0 0,100 0,100 100,0 100,0 0))");
    QVERIFY(!FaciesPartitionEditor::paint(layer, outline, 1, &error));
    QVERIFY(error.contains(QStringLiteral("依据"))); QCOMPARE(layer->featureCount(), 0);
    QVERIFY(!f.work.setEditEvidence("D61", {{"text", "observation"}}, &error));
    const QVariantMap context{{"text", "well and sand thickness observation"}, {"reason", "draw initial outline"}};
    QVERIFY2(f.work.setEditEvidence("D61", context, &error), qPrintable(error));
    QVERIFY2(FaciesPartitionEditor::paint(layer, outline, 1, &error), qPrintable(error));
    QCOMPARE(layer->featureCount(), 1);
    auto selected = layer->selectedFeatureIds().values(); QCOMPARE(selected.size(), 1);
    const auto evidence = f.work.featureEvidence(draft, selected);
    QCOMPARE(evidence.value("records").toList().size(), 1);
    QCOMPARE(evidence.value("records").toList().first().toMap().value("role").toString(), QString("SUPPORTED_BY"));
    QVERIFY(f.work.hasUnpublishedEdits(draft));
    layer->undoStack()->undo(); QCOMPARE(layer->featureCount(), 0);
    QVERIFY2(FaciesPartitionEditor::validate(layer, &error), qPrintable(error));
    layer->undoStack()->redo(); QCOMPARE(layer->featureCount(), 1);
    QVERIFY2(f.work.checkEditingTopology(draft, &error), qPrintable(error));
    QVERIFY2(layer->commitChanges(), qPrintable(layer->commitErrors().join(";")));
    QVERIFY2(f.work.saveEditingVersion(draft, &error), qPrintable(error));
    QVERIFY(!f.work.hasUnpublishedEdits(draft));
    QVERIFY(FaciesPartitionState::hasFrozenFootprint(layer));
    const auto version = f.work.versionForLayer(draft);
    QVERIFY(!version.extra.value("partition_footprint_path").toString().isEmpty());
    QVERIFY(version.extra.value("requires_edit_evidence").toBool());
    { DataExportDialog dialog(&f.catalog, {version.assetId}, nullptr, nullptr, {initial.id});
      QCOMPARE(dialog.request().versionIds, QStringList{initial.id}); }
    QVERIFY(layer->startEditing());
    const auto stripe = QgsGeometry::fromWkt("POLYGON((40 -20,60 -20,60 120,40 120,40 -20))");
    QVERIFY2(FaciesPartitionEditor::paint(layer, stripe, 2, &error), qPrintable(error));
    QVERIFY2(f.work.checkEditingTopology(draft, &error), qPrintable(error));
    auto pieces = layer->getFeatures(); QgsFeature piece; double area = 0;
    while (pieces.nextFeature(piece)) {
      area += piece.geometry().area();
      QVERIFY(FaciesTaxonomy::complete(FaciesCatalog::find(f.work.facies("D61"), piece.attribute("facies_code"))));
      QVERIFY(!piece.attribute("facies_evidence").toString().isEmpty());
    }
    QCOMPARE(area, outline.area());
    QVERIFY(layer->rollBack());
  }
  void editingContextPinsReferenceVersionsAndReclassificationIsAtomic() {
    Fixture f; QVERIFY(f.init()); QString error;
    const auto base = hierarchyDraft(f, &error); QVERIFY2(!base.isEmpty(), qPrintable(error));
    const auto reference = f.work.versionForLayer(base);
    const auto draft = f.work.copyForEditing(base, {}, &error, true);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft)); QVERIFY(layer);
    QVERIFY(layer->startEditing()); QgsFeature face; auto iterator = layer->getFeatures();
    QVERIFY(iterator.nextFeature(face)); iterator.close();
    const int original = face.attribute("facies_code").toInt(), next = original == 1 ? 2 : 1;
    QVERIFY(!f.work.assignHierarchy(draft, {face.id()}, next, "micro_facies", &error));
    QCOMPARE(layer->getFeature(face.id()).attribute("facies_code").toInt(), original);
    QVERIFY(!f.work.setEditEvidence("D61", {{"text", "test"}, {"reason", "test"}, {"source_versions", QStringList{"missing"}}}, &error));
    const QVariantMap context{{"text", "observed channel boundary"}, {"reason", "revise microfacies"},
        {"source_versions", QStringList{reference.id, reference.id}}};
    QVERIFY2(f.work.setEditEvidence("D61", context, &error), qPrintable(error));
    QCOMPARE(f.work.editEvidence("D61").value("references").toList().size(), 1);
    const auto sourcePath = DataCatalog::resolvedVersionPath(f.dir.path(), reference);
    QVERIFY(QFile::rename(sourcePath, sourcePath + ".hidden"));
    const bool missingSourceAccepted = f.work.assignHierarchy(draft, {face.id()}, next, "micro_facies", &error);
    QVERIFY(QFile::rename(sourcePath + ".hidden", sourcePath));
    QVERIFY(!missingSourceAccepted);
    QCOMPARE(layer->getFeature(face.id()).attribute("facies_code").toInt(), original);
    QVERIFY2(f.work.assignHierarchy(draft, {face.id()}, next, "micro_facies", &error), qPrintable(error));
    QVariantList records; QVariantMap interpretation;
    auto changed = layer->getFeature(face.id());
    QVERIFY(paleo::evidence::decodeRecords(changed.attribute("facies_evidence").toString(), &records, &error));
    QCOMPARE(records.size(), 2);
    QCOMPARE(records.first().toMap().value("source_version").toString(), reference.id);
    QCOMPARE(records.first().toMap().value("role").toString(), QString("REFERENCES"));
    QCOMPARE(records.last().toMap().value("role").toString(), QString("SUPPORTED_BY"));
    QVERIFY(changed.geometry().isGeosEqual(face.geometry()));
    layer->undoStack()->undo();
    QCOMPARE(layer->getFeature(face.id()).attribute("facies_code").toInt(), original);
    QVERIFY(paleo::evidence::decodeRecords(layer->getFeature(face.id()).attribute("facies_evidence").toString(), &records, &error));
    QVERIFY(records.isEmpty());
    layer->undoStack()->redo();
    QVERIFY2(f.work.assignHierarchy(draft, {face.id()}, original, "micro_facies", &error), qPrintable(error));
    QVERIFY2(layer->commitChanges(), qPrintable(layer->commitErrors().join(";")));
    QVERIFY2(f.work.saveEditingVersion(draft, &error), qPrintable(error));
    QVERIFY(f.work.versionForLayer(draft).parentVersionIds.contains(reference.id));
    f.work.bindCatalog(&f.catalog, f.dir.path());
    QVERIFY(f.work.editEvidence("D61").isEmpty());
  }
  void manualMappingModeRequiresEvidenceForExistingDrafts() {
    Fixture f; QVERIFY(f.init()); QString error;
    const auto draft = hierarchyDraft(f, &error); QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft)); QVERIFY(layer);
    QgsFeature face; auto features = layer->getFeatures(); QVERIFY(features.nextFeature(face)); features.close();
    const int original = face.attribute("facies_code").toInt(), target = original == 1 ? 2 : 1;
    f.work.setManualEvidenceRequired(true);
    QVERIFY(!f.work.assignHierarchy(draft, {face.id()}, target, "micro_facies", &error));
    QCOMPARE(layer->getFeature(face.id()).attribute("facies_code").toInt(), original);
    QVERIFY2(f.work.setEditEvidence("D61", {{"text", "manual observation"}, {"reason", "revise existing map"}}, &error), qPrintable(error));
    QVERIFY2(f.work.assignHierarchy(draft, {face.id()}, target, "micro_facies", &error), qPrintable(error));
    QVERIFY(layer->rollBack());
    f.work.setManualEvidenceRequired(false);
    QVERIFY(f.work.editEvidence("D61").isEmpty());
    QVERIFY2(f.work.assignHierarchy(draft, {face.id()}, target, "micro_facies", &error), qPrintable(error));
    QVERIFY(layer->rollBack());
  }
  void zeroFeaturePartitionStillOpensAnEditingCopy() {
    Fixture f; QVERIFY(f.init()); QString error;
    QVERIFY2(f.work.saveFacies("D61", hierarchySchema(), &error), qPrintable(error));
    QgsVectorLayer empty("Polygon?field=facies_code:integer", "empty", "memory");
    empty.setCrs(QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt()));
    QCOMPARE(empty.featureCount(), 0);
    const auto path = f.dir.filePath("empty-grid.gpkg");
    QVERIFY2(MappingArtifactWriter::vectorSnapshot(&empty, path, &error), qPrintable(error));
    LayerDeclaration d;
    d.layerId = "input.empty"; d.horizon = "D61"; d.type = "vector";
    d.group = "05_Composition"; d.title = "empty partition";
    d.source = path + "|layername=features";
    QVERIFY(f.layers.declare(d, &error));
    const auto draft = f.work.copyForEditing(d.layerId, {}, &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    // 放行语义：footprint 留空——既不写 sidecar，也不写旧 WKB 字段。
    const auto version = f.work.versionForLayer(draft);
    QVERIFY(version.extra.value("partition_footprint_path").toString().isEmpty());
    QVERIFY(version.extra.value("partition_footprint_wkb").toString().isEmpty());
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft, &error));
    QVERIFY(layer);
    // install() 放行且不冻结覆盖范围（validate 的 footprint 比对随之跳过）。
    QVERIFY2(FaciesPartitionEditor::validate(layer, &error), qPrintable(error));
    QVERIFY(!FaciesPartitionState::hasFrozenFootprint(layer));
    QVERIFY(FaciesPartitionEditor::isPartition(layer));
  }
  void partitionCommitRejectionCanBeRepairedWithUndo() {
    Fixture f; QVERIFY(f.init()); QString error;
    const auto draft = hierarchyDraft(f, &error); QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft)); QVERIFY(layer);
    QVERIFY(layer->startEditing()); auto it = layer->getFeatures(); QgsFeature face; QVERIFY(it.nextFeature(face)); it.close();
    layer->beginEditCommand("invalid outside edit"); auto geometry = face.geometry(); geometry.translate(100000,0);
    QVERIFY(layer->changeGeometry(face.id(), geometry)); layer->endEditCommand();
    QVERIFY(!layer->commitChanges()); QVERIFY(layer->isEditable());
    QVERIFY(!layer->commitChanges()); // repeated save must retain the recovery path
    layer->undoStack()->undo(); QCoreApplication::processEvents();
    QVERIFY2(FaciesPartitionEditor::validate(layer, &error), qPrintable(error));
    QVERIFY2(layer->commitChanges(), qPrintable(layer->commitErrors().join(";")));
    // An independent beforeCommit veto must still win on a healthy partition.
    QVERIFY(layer->startEditing());
    const auto veto = connect(layer, &QgsVectorLayer::beforeCommitChanges, layer, [layer] { layer->setAllowCommit(false); });
    QVERIFY(!layer->commitChanges()); disconnect(veto); QVERIFY(layer->rollBack());
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
  // X10 验收链 C：地图选中 → 证据定位 → 图谱同目标 → 反查命中导航到命中
  // 图斑 → 回退恢复原主目标；全程核对工程/层位/版本/图斑/证据身份。
  void reverseHitNavigationAndBackRestorePrimaryTarget() {
    Fixture f; QVERIFY(f.init()); QString error;
    const auto draft = hierarchyDraft(f, &error); QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft)); QVERIFY(layer);
    QgsFeature a, b; auto features = layer->getFeatures();
    QVERIFY(features.nextFeature(a)); QVERIFY(features.nextFeature(b)); features.close();
    const auto uidA = MappingFeatureIdentity::uid(a), uidB = MappingFeatureIdentity::uid(b);
    QVERIFY(!uidA.isEmpty()); QVERIFY(uidA != uidB);
    // 草稿与保存版本同为 edited_facies——只取已保存（product.*）行。
    const auto latestSavedEdited = [&f]() {
      QString found; int ver = 0;
      for (const auto &row : f.work.products(QStringLiteral("D61"))) {
        const auto r = row.toMap();
        if (r.value("kind") == QLatin1String("edited_facies") &&
            r.value("id").toString().startsWith(QLatin1String("product.")) &&
            r.value("version").toInt() > ver) {
          found = r.value("id").toString(); ver = r.value("version").toInt();
        }
      }
      return found;
    };
    // V1：保存版本作为可钉图元的证据来源。
    QVERIFY2(f.work.saveEditingVersion(draft, &error), qPrintable(error));
    const auto v1Layer = latestSavedEdited(); QVERIFY(!v1Layer.isEmpty());
    const auto v1 = f.work.versionForLayer(v1Layer).id; QVERIFY(!v1.isEmpty());
    // A 与 B 各钉定一条指向 V1 内图元的来源记录（同一 source_version）。
    QVERIFY2(f.work.addEvidence(draft, {a.id()},
        {{"text", "链C证据A"}, {"role", "REFERENCES"}, {"type", "single_factor"},
         {"source_layer", v1Layer}, {"source_feature_uid", uidA},
         {"reason", "链C钉源"}, {"actor_id", "local:tester"}}, &error), qPrintable(error));
    QVERIFY2(f.work.addEvidence(draft, {b.id()},
        {{"text", "链C证据B"}, {"role", "REFERENCES"}, {"type", "single_factor"},
         {"source_layer", v1Layer}, {"source_feature_uid", uidB},
         {"reason", "链C钉源"}, {"actor_id", "local:tester"}}, &error), qPrintable(error));
    QVERIFY2(layer->commitChanges(), qPrintable(layer->commitErrors().join(';')));
    QVERIFY2(f.work.saveEditingVersion(draft, &error), qPrintable(error));
    const auto v2Layer = latestSavedEdited(); QVERIFY(v2Layer != v1Layer);
    const auto v2 = f.work.versionForLayer(v2Layer).id; QVERIFY(!v2.isEmpty()); QVERIFY(v2 != v1);
    SelectionContext selection; selection.setActiveHorizon("D61");
    QgisCanvasController canvas; canvas.setLayerResolver([&f](const QString &id) { return f.layers.layer(id); });
    EvidenceInteraction interaction(&f.work, &selection, &f.layers, &f.project, &canvas);
    const auto projectId = f.project.mapConfiguration().projectId;
    // 1) 地图选中 A → 主目标/快照 = A
    layer->selectByIds({a.id()});
    QCOMPARE(selection.target().featureUid, uidA);
    QCOMPARE(selection.target().layerId, draft);
    QCOMPARE(selection.target().projectId, projectId);
    QCOMPARE(interaction.snapshot().value("feature_uid").toString(), uidA);
    QVERIFY(!interaction.canGoBack());
    // 2) 定位 A 的证据来源：来源高亮，主目标仍是 A 且 evidenceId=E
    QVariantMap evidenceRowA;
    for (const auto &entry : f.work.evidence(draft, {a.id()})) {
      const auto row = entry.toMap();
      if (row.value("text").toString() == QStringLiteral("链C证据A"))
        evidenceRowA = row;
    }
    QVERIFY(!evidenceRowA.isEmpty());
    const auto evidenceIdA = evidenceRowA.value("id").toString();
    QVERIFY(!evidenceIdA.isEmpty());
    QCOMPARE(evidenceRowA.value("source_version").toString(), v1);
    interaction.navigate(evidenceRowA);
    QCOMPARE(selection.target().featureUid, uidA);
    QCOMPARE(selection.target().evidenceId, evidenceIdA);
    QVERIFY(!interaction.canGoBack()); // 来源高亮不构成新研究目标
    // 3) 图谱：同一快照覆盖层含证据节点 E 与来源版本节点 V1
    const auto graphIndex = paleo::datagraph::buildIndex(f.catalog, {}, {}, {});
    QVERIFY(graphIndex);
    paleo::datagraph::Query gq;
    gq.mode = paleo::datagraph::Mode::Relations;
    gq.focus = paleo::datagraph::versionKey(f.work.versionForLayer(draft).id);
    gq.depth = 4; gq.horizon = QStringLiteral("D61");
    gq.featureEvidence = interaction.snapshot();
    const auto graph = paleo::datagraph::query(*graphIndex, gq, {}, {});
    bool hasEvidence = false, hasSource = false;
    for (const auto &node : graph.nodes) {
      hasEvidence |= node.evidenceId == evidenceIdA;
      hasSource |= node.versionId == v1;
    }
    QVERIFY2(hasEvidence, "覆盖层缺少证据节点");
    QVERIFY2(hasSource, "覆盖层缺少来源版本节点");
    QVERIFY(paleo::datagraph::edgesMissingProvenance(graph).isEmpty());
    // 4) 反查 references_source(V1) → A 与 B 的命中行（V2 版本）
    const auto rows = f.work.evidenceReverseQuery(
        QStringLiteral("references_source"), v1, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVariantMap rowB;
    bool foundA = false;
    for (const auto &value : rows) {
      const auto row = value.toMap();
      if (row.value("version_id").toString() != v2) continue;
      if (row.value("feature_uid").toString() == uidA) foundA = true;
      if (row.value("feature_uid").toString() == uidB) rowB = row;
    }
    QVERIFY(foundA); QVERIFY(!rowB.isEmpty());
    // 5) navigateFeature 命中 B：主目标换成 B 所在的确切版本，A 入栈
    interaction.navigateFeature(rowB);
    QCOMPARE(selection.target().featureUid, uidB);
    QCOMPARE(selection.target().layerId, f.work.layerForVersion(v2));
    QCOMPARE(selection.target().versionId, v2);
    QCOMPARE(selection.target().assetId, f.work.versionForLayer(v2Layer).assetId);
    QCOMPARE(selection.target().horizon, QStringLiteral("D61"));
    QCOMPARE(selection.target().projectId, projectId);
    QVERIFY(interaction.canGoBack());
    QCOMPARE(interaction.snapshot().value("feature_uid").toString(), uidB);
    // 6) back() → 恢复 A（含版本/图层身份），栈空
    interaction.back();
    QCOMPARE(selection.target().featureUid, uidA);
    QCOMPARE(selection.target().layerId, draft);
    QCOMPARE(selection.target().versionId, f.work.versionForLayer(draft).id);
    QCOMPARE(selection.target().projectId, projectId);
    QCOMPARE(interaction.snapshot().value("feature_uid").toString(), uidA);
    QVERIFY(!interaction.canGoBack());
    // 7) 其他层位的命中：明确失败且不移动目标
    QSignalSpy failures(&interaction, &EvidenceInteraction::navigationFailed);
    auto other = rowB; other.insert(QStringLiteral("horizon"), QStringLiteral("D62"));
    interaction.navigateFeature(other);
    QCOMPARE(failures.count(), 1);
    QVERIFY(failures[0][0].toString().contains(QStringLiteral("层位")));
    QCOMPARE(selection.target().featureUid, uidA);
    QCOMPARE(selection.target().layerId, draft);
    // 8) reset 清空回退栈
    interaction.navigateFeature(rowB);
    QVERIFY(interaction.canGoBack());
    interaction.reset();
    QVERIFY(!interaction.canGoBack());
    QVERIFY(!layer->isEditable());
  }
  void repeatedNativeMergesKeepOneParentTreeAndReopenableHistory() {
    Fixture f; QVERIFY(f.init()); QString error;
    const auto draft = hierarchyDraft(f, &error); QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft)); QVERIFY(layer);
    QgsFeature survivor, candidate; auto features = layer->getFeatures();
    while (features.nextFeature(candidate)) if (!survivor.isValid() || candidate.geometry().boundingBox().xMaximum() > survivor.geometry().boundingBox().xMaximum()) survivor = candidate;
    features.close(); QVERIFY(survivor.isValid());
    QVERIFY(layer->startEditing()); const auto fid = survivor.id();
    // #503 快速道减样：PALEO_TEST_FAST=1 时合并轮数 20→5——逐轮不变量
    //（父树单线/无双记/解释继承）与收尾 saveEditingVersion+重开对拍不变，
    // 只减轮次；默认全量 20（CI 语义不变）。轮内断言界 (n+1)*20000 随轮缩放。
    const int mergeRounds = qEnvironmentVariableIntValue("PALEO_TEST_FAST") > 0 ? 5 : 20;
    for (int n = 0; n < mergeRounds; ++n) {
      const auto current = layer->getFeature(fid); const auto box = current.geometry().boundingBox();
      QgsFeature adjacent(layer->fields()); adjacent.setAttribute("facies_code", current.attribute("facies_code"));
      // Stress the lineage inside the frozen map footprint: carve off the
      // neighbor instead of silently extending the study area on every cycle.
      const double splitX = (box.xMinimum() + box.xMaximum()) / 2;
      adjacent.setGeometry(QgsGeometry::fromRect(QgsRectangle(splitX, box.yMinimum(), box.xMaximum(), box.yMaximum())));
      auto remainder = QgsGeometry::fromRect(QgsRectangle(box.xMinimum(), box.yMinimum(), splitX, box.yMaximum()));
      layer->beginEditCommand("new neighbouring interpretation");
      QVERIFY(layer->changeGeometry(fid, remainder)); QVERIFY(layer->addFeature(adjacent)); layer->endEditCommand();
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
      // Stress the lineage inside the frozen map footprint: carve off the
      // neighbor instead of silently extending the study area on every cycle.
      const double splitX = (box.xMinimum() + box.xMaximum()) / 2;
      adjacent.setGeometry(QgsGeometry::fromRect(QgsRectangle(splitX, box.yMinimum(), box.xMaximum(), box.yMaximum())));
      auto remainder = QgsGeometry::fromRect(QgsRectangle(box.xMinimum(), box.yMinimum(), splitX, box.yMaximum()));
      layer->beginEditCommand("new neighbouring interpretation");
      QVERIFY(layer->changeGeometry(fid, remainder)); QVERIFY(layer->addFeature(adjacent)); layer->endEditCommand();
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
    const auto versions = f.catalog.versions().size();
    const auto again = f.work.snapshotConstraints("D61", {}, &error);
    QVERIFY2(!again.isEmpty(), qPrintable(error));
    QCOMPARE(again, layerId);
    QCOMPARE(f.catalog.versions().size(), versions);
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
  void referenceLegendFollowsPinnedLevelAndClearsOnSourceChange() {
    QgsMapCanvas main;
    main.resize(600, 400);
    main.setDestinationCrs(QgsCoordinateReferenceSystem("EPSG:3857"));
    MappingReferencePanel panel(&main, nullptr);
    panel.setSources({QVariantMap{{"id", "micro"}, {"name", "微相版本"}, {"type", "vector"}},
                      QVariantMap{{"id", "other"}, {"name", "其他证据"}, {"type", "vector"}}}, "D61");
    panel.selectSource("micro");
    const auto schema = FaciesCatalog::defaults();
    const auto load = [&](const QString &level) {
      auto *layer = new QgsVectorLayer("Polygon?field=facies_code:integer&crs=EPSG:3857", "facies", "memory");
      QgsFeatureList features;
      for (const auto &value : schema) {
        QgsFeature feature(layer->fields());
        const int code = value.toMap().value("code").toInt();
        feature.setAttribute("facies_code", code);
        feature.setGeometry(QgsGeometry::fromRect(QgsRectangle(code * 100, 0, (code + 1) * 100, 100)));
        features << feature;
      }
      QVERIFY(layer->dataProvider()->addFeatures(features));
      layer->updateExtents();
      panel.setReference(layer, {{"facies", schema}, {"display_mode", level}, {"mock", true}});
    };
    load("micro_facies");
    QVERIFY(panel.hasReference());
    panel.selectSource("other");
    QVERIFY(!panel.hasReference());
    main.stopRendering();
  }
  void referenceViewSelectsSourcesAndKeepsPinnedVersion() {
    QgsMapCanvas main;
    main.resize(600, 400);
    main.setDestinationCrs(QgsCoordinateReferenceSystem("EPSG:3857"));
    main.setExtent(QgsRectangle(0, 0, 600, 400));
    MappingReferencePanel panel(&main, nullptr);
    panel.resize(420, 400);
    panel.show();
    panel.setViewActive(true);
    const QVariantList rows{
        QVariantMap{{"id", "seismic"}, {"name", "D61 地震相 v1"}, {"horizon", "D61"}, {"type", "raster"}, {"kind", "seismic_prediction"}},
        QVariantMap{{"id", "wells"}, {"name", "D61 测井相 v1"}, {"horizon", "D61"}, {"type", "vector"}, {"kind", "wells_prediction"}},
        QVariantMap{{"id", "factor"}, {"name", "D61 砂厚 v2"}, {"horizon", "D61"}, {"type", "raster"}, {"kind", "single_factor"}},
        QVariantMap{{"id", "reference"}, {"name", "参考相图"}, {"type", "vector"}},
    };
    panel.setSources(rows, "D61");
    auto *sources = panel.findChild<QComboBox *>("referenceSource");
    QVERIFY(sources);
    QCOMPARE(panel.findChildren<QComboBox *>().size(), 1);
    QCOMPARE(sources->count(), 5);
    QVERIFY(sources->itemText(1).contains(QStringLiteral("地震相")));
    QVERIFY(sources->itemText(2).contains(QStringLiteral("测井相")));
    QVERIFY(sources->itemText(3).contains(QStringLiteral("单因素图")));
    QSignalSpy request(&panel, &MappingReferencePanel::referenceRequested);
    panel.selectSource("wells");
    QCOMPARE(panel.sourceId(), QString("wells"));
    QCOMPARE(request.size(), 1);
    auto *layer = new QgsVectorLayer("Point?crs=EPSG:3857", "well", "memory");
    QVERIFY(layer->isValid());
    QPointer<QgsMapLayer> owned = layer;
    panel.setReference(layer, {});
    QVERIFY(panel.hasReference());
    panel.setSources(rows, "D62");
    QCOMPARE(panel.sourceId(), QString("wells"));
    QVERIFY(panel.findChild<QComboBox *>("referenceSource")->toolTip().contains(QStringLiteral("跨层位")));
    QVERIFY(owned);
    // 目录刷新保留选中的版本；移除来源则释放私有图层并显示空态。
    panel.setSources(rows, "D62");
    QCOMPARE(request.size(), 1);
    panel.setSources({}, "D62");
    QVERIFY(!panel.hasReference());
    QVERIFY(owned.isNull());
    QVERIFY(panel.findChild<QLabel *>("referenceEmptyState")->isVisibleTo(&panel));
    QVERIFY(!sources->isEnabled());
  }

  void evidencePanelAddsViewsAndPreservesOtherSelections() {
    QgsMapCanvas main;
    main.setDestinationCrs(QgsCoordinateReferenceSystem("EPSG:3857"));
    main.setExtent(QgsRectangle(0, 0, 600, 400));
    MappingEvidencePanel panel(&main, nullptr);
    panel.resize(800, 360);
    panel.show();
    panel.setViewActive(true);
    const QVariantList rows{
        QVariantMap{{"id", "wells"}, {"name", "D61 测井相 v1"}, {"horizon", "D61"}, {"type", "vector"}, {"kind", "wells_prediction"}},
        QVariantMap{{"id", "seismic"}, {"name", "D61 地震相 v1"}, {"horizon", "D61"}, {"type", "vector"}, {"kind", "seismic_prediction"}}};
    panel.setSources(rows, "D61");
    connect(&panel, &MappingEvidencePanel::referenceRequested, &panel,
            [](MappingReferencePanel *view, const QString &) {
      view->setReference(new QgsVectorLayer("Point?crs=EPSG:3857", "evidence", "memory"), {});
    });
    QCOMPARE(panel.views().size(), 2);
    auto *first = panel.views()[0];
    auto *second = panel.views()[1];
    first->selectSource("wells");
    second->selectSource("seismic");
    QVERIFY(first->hasReference() && second->hasReference());
    QPointer<QgsMapLayer> firstLayer = first->canvas()->layers().first();
    QPointer<QgsMapLayer> secondLayer = second->canvas()->layers().first();
    panel.findChild<QPushButton *>("referenceAddView")->click();
    QCOMPARE(panel.views().size(), 3);
    QCoreApplication::processEvents();
    auto *scroll = panel.findChild<QScrollArea *>("referenceViewsScroll");
    QVERIFY(scroll);
    QCOMPARE(scroll->horizontalScrollBarPolicy(), Qt::ScrollBarAsNeeded);
    const int cardWidth = panel.views().first()->width();
    QVERIFY(cardWidth > 0 && cardWidth <= 420);
    scroll->widget()->layout()->activate();
    const int rowY = panel.views().first()->pos().y();
    int previousX = -1;
    QString placed;
    for (auto *view : panel.views()) {
      QCOMPARE(view->width(), cardWidth);
      QCOMPARE(view->pos().y(), rowY);
      const int x = view->pos().x();
      placed += QString::number(x) + QLatin1Char(' ');
      QVERIFY2(x > previousX, qPrintable(placed));
      previousX = x;
    }
    QVERIFY(scroll->widget()->minimumWidth() > scroll->viewport()->width());
    QCOMPARE(first->sourceId(), QString("wells"));
    QCOMPARE(second->sourceId(), QString("seismic"));
    QCOMPARE(first->canvas()->layers().first(), firstLayer.data());
    auto *third = panel.views()[2];
    QVERIFY(third->sourceId().isEmpty());
    third->selectSource("wells");
    QVERIFY(third->hasReference());
    QCOMPARE(panel.showSource("seismic"), second);
    QCOMPARE(panel.views().size(), 3); // 重复打开已有来源只定位内部视图。
    second->findChild<QToolButton *>("referenceRemoveView")->click();
    QCOMPARE(panel.views().size(), 2);
    QVERIFY(secondLayer.isNull());
    QCOMPARE(first->sourceId(), QString("wells"));
    QCOMPARE(first->canvas()->layers().first(), firstLayer.data());
    third->findChild<QToolButton *>("referenceRemoveView")->click();
    QCOMPARE(panel.views().size(), 1);
    QVERIFY(first->findChild<QToolButton *>("referenceRemoveView")->isEnabled());
    QCOMPARE(first->findChild<QToolButton *>("referenceRemoveView")->text(), QStringLiteral("−"));
    // D-12（full-audit-20261020）：关最后视图有空态引导；再添加可恢复；
    // 最后视图的移除提示如实说明后果（#547 允许关全部，不改可点性）。
    QVERIFY(first->findChild<QToolButton *>("referenceRemoveView")->toolTip()
                .contains(QStringLiteral("最后一个")));
    auto *emptyHint = panel.findChild<QLabel *>("referenceEmptyHint");
    auto *countLabel = panel.findChild<QLabel *>("referenceViewCount");
    QVERIFY(emptyHint && countLabel);
    QVERIFY(!emptyHint->isVisibleTo(&panel));
    first->findChild<QToolButton *>("referenceRemoveView")->click();
    QCOMPARE(panel.views().size(), 0);
    QVERIFY(emptyHint->isVisibleTo(&panel));
    QCOMPARE(countLabel->text(), QStringLiteral("0 个视图"));
    panel.findChild<QPushButton *>("referenceAddView")->click();
    QCOMPARE(panel.views().size(), 1);
    QVERIFY(!emptyHint->isVisibleTo(&panel));
    QCOMPARE(panel.views().first()->sourceId(), QString());
    panel.setSources({}, QString());
    QVERIFY(firstLayer.isNull());
    QVERIFY(!first->hasReference());
    main.stopRendering();
  }

  void evidenceViewsStayInOneRowWhenResizedAndScrollWithWheel() {
    QgsMapCanvas main;
    main.setDestinationCrs(QgsCoordinateReferenceSystem("EPSG:3857"));
    main.setExtent(QgsRectangle(0, 0, 600, 400));
    MappingEvidencePanel panel(&main, nullptr);
    panel.resize(800, 360); panel.show();
    for (int i = 0; i < 3; ++i) panel.addView();
    QCOMPARE(panel.views().size(), 5);
    auto *scroll = panel.findChild<QScrollArea *>("referenceViewsScroll"); QVERIFY(scroll);
    auto *add = panel.findChild<QPushButton *>("referenceAddView"); QVERIFY(add);
    QCOMPARE(scroll->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
    QTRY_VERIFY(scroll->horizontalScrollBar()->maximum() > 0);
    QVERIFY(add->mapTo(&panel, QPoint()).y() + add->height() <= scroll->mapTo(&panel, QPoint()).y());
    auto *first = panel.views().first();
    first->setReference(new QgsVectorLayer("Point?crs=EPSG:3857", "evidence", "memory"), {});
    const auto scale = first->canvas()->scale();
    QCoreApplication::processEvents();
    auto *bar = scroll->horizontalScrollBar(); bar->setValue(0);
    const QPoint pos = first->canvas()->viewport()->rect().center();
    QWheelEvent right(pos, first->canvas()->viewport()->mapToGlobal(pos), QPoint(-60, 0),
        QPoint(-120, 0), Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
    QApplication::sendEvent(first->canvas()->viewport(), &right);
    QCOMPARE(bar->value(), 60); QCOMPARE(first->canvas()->scale(), scale);
    const QPoint background = scroll->viewport()->rect().center();
    QWheelEvent left(background, scroll->viewport()->mapToGlobal(background), QPoint(60, 0),
        QPoint(120, 0), Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
    QApplication::sendEvent(scroll->viewport(), &left); QCOMPARE(bar->value(), 0);
    QWheelEvent vertical(background, scroll->viewport()->mapToGlobal(background), {},
        QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(scroll->viewport(), &vertical); QVERIFY(bar->value() > 0);
    for (int width : {800, 340, 1100}) {
      panel.resize(width, 360); QCoreApplication::processEvents();
      const int row = first->y(); int rightEdge = -1;
      for (auto *view : panel.views()) {
        QCOMPARE(view->y(), row); QCOMPARE(view->width(), first->width());
        QVERIFY(view->x() > rightEdge); rightEdge = view->geometry().right();
      }
      QVERIFY(scroll->widget()->minimumWidth() > scroll->viewport()->width());
      QCOMPARE(scroll->verticalScrollBar()->maximum(), 0);
      if (const auto qa = qEnvironmentVariable("PALEO_MAPPING_REFERENCE_QA_DIR"); !qa.isEmpty()) {
        QDir().mkpath(qa); QVERIFY(panel.grab().save(qa + QString("/evidence-single-row-%1.png").arg(width)));
      }
    }
    panel.views()[2]->findChild<QToolButton *>("referenceRemoveView")->click();
    QCOMPARE(panel.views().size(), 4); QCoreApplication::processEvents();
    for (auto *view : panel.views()) QCOMPARE(view->y(), first->y());
    main.stopRendering();
  }

  void multipleEvidenceViewsKeepDrawingToolAndCanSuspendLinks() {
    QgsProject project;
    QgsMapCanvas main;
    main.resize(600, 400);
    main.setDestinationCrs(QgsCoordinateReferenceSystem("EPSG:3857"));
    main.setExtent(QgsRectangle(0, 0, 600, 400));
    auto *draft = new QgsVectorLayer("Polygon?crs=EPSG:3857&field=facies_code:integer", "draft", "memory");
    project.addMapLayer(draft);
    PaleoEditingToolbar edit(&main);
    edit.setProject(&project);
    edit.setLayers({draft});
    edit.setCurrentLayer(draft);
    edit.actionAddPolygon()->trigger();
    QVERIFY(edit.isEditing());
    const auto *drawingTool = main.mapTool();
    QVERIFY(drawingTool);
    MappingReferencePanel first(&main, &project), second(&main, &project);
    const QVariantList rows{QVariantMap{{"id", "reference"}, {"name", "参考相图"}, {"type", "vector"}}};
    for (auto *panel : {&first, &second}) {
      panel->resize(400, 360);
      panel->setSources(rows, "D61");
      panel->selectSource("reference");
      panel->show();
      panel->setViewActive(true);
      auto *layer = new QgsVectorLayer("Polygon?crs=EPSG:3857", "reference", "memory");
      QVERIFY(layer->isValid());
      panel->setReference(layer, {});
      QCOMPARE(main.mapTool(), drawingTool);
      QCOMPARE(main.currentLayer(), draft);
    }
    QCOMPARE(project.mapLayers().size(), 1); // 参考层不登记到主工程图层树。
    // 从不同的初始尺寸拆分到相同尺寸后，联动范围也应一致，不能只同步中心。
    first.resize(720, 480);
    second.resize(340, 340);
    QCoreApplication::processEvents();
    first.resize(500, 360);
    second.resize(500, 360);
    QTRY_VERIFY(qFuzzyCompare(first.canvas()->extent().width(), second.canvas()->extent().width()));
    QTRY_VERIFY(qFuzzyCompare(first.canvas()->extent().height(), second.canvas()->extent().height()));
    main.setExtent(QgsRectangle(100, 200, 700, 600));
    QCOMPARE(first.canvas()->extent().center(), main.extent().center());
    QCOMPARE(second.canvas()->extent().center(), main.extent().center());
    emit main.xyCoordinates(QgsPointXY(300, 400));
    const auto hasCursor = [](QgsMapCanvas *canvas) {
      for (auto *item : canvas->scene()->items())
        if (dynamic_cast<QgsRubberBand *>(item)) return true;
      return false;
    };
    QVERIFY(hasCursor(first.canvas()));
    QVERIFY(hasCursor(second.canvas()));
    first.setViewActive(false);
    QVERIFY(!hasCursor(first.canvas()));
    const auto stopped = first.canvas()->extent();
    main.setExtent(QgsRectangle(200, 300, 800, 700));
    QCOMPARE(first.canvas()->extent(), stopped);
    QCOMPARE(second.canvas()->extent().center(), main.extent().center());
    first.setViewActive(true);
    QCOMPARE(first.canvas()->extent().center(), main.extent().center());
    const auto before = main.extent();
    first.resize(180, 140);
    QCoreApplication::processEvents();
    QCOMPARE(main.extent(), before);
    auto *linked = first.findChild<QToolButton *>("referenceLinked");
    QVERIFY(linked && linked->isChecked());
    linked->setChecked(false);
    QCOMPARE(main.extent(), before);
    first.clearReference(QStringLiteral("参考文件缺失"));
    QVERIFY(first.canvas()->layers().isEmpty());
    QCOMPARE(first.findChild<QLabel *>("referenceEmptyState")->text(), QStringLiteral("参考文件缺失"));
    QCOMPARE(main.mapTool(), drawingTool);
    QCOMPARE(main.currentLayer(), draft);
    QVERIFY(edit.cancelEditing());
    main.stopRendering();
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
    auto *featureLevel = page.findChild<QComboBox *>("faciesFeatureDisplayLevel");
    auto *featureApply = page.commandButton("featureDisplayLevel");
    QVERIFY(featureLevel && featureApply);
    QVERIFY(!featureApply->isEnabled());
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
    page.setEvidenceSnapshot(f.work.featureEvidence(draft, {feature.id()}));
    QVERIFY(featureApply->isEnabled());
    featureLevel->setCurrentIndex(featureLevel->findData("facies"));
    featureApply->click();
    QCOMPARE(intent.last()[0].toString(), QString("featureDisplayLevel"));
    QCOMPARE(intent.last()[1].toMap().value("display_mode").toString(), QString("facies"));
    // 固定显示层级的相类：单列一组默认展开，只列已固定项；行内改层级只发意图。
    auto *pinGroup = page.findChild<QgsCollapsibleGroupBox *>("workbenchFaciesPins");
    QVERIFY(pinGroup && !pinGroup->isCollapsed());
    auto *pinList = page.findChild<QTreeWidget *>("faciesPinList");
    QVERIFY(pinList && pinGroup->isAncestorOf(pinList));
    QCOMPARE(pinList->topLevelItemCount(), 0);
    const auto pinKey = FaciesHierarchy::pinCandidates(
        f.work.versionForLayer(draft).extra.value("facies").toList()).first().toMap().value("key").toString();
    QVERIFY(f.work.setPinnedLevel(draft, pinKey, "facies", &error));
    page.refresh();
    QCOMPARE(pinList->topLevelItemCount(), 1);
    auto *rowLevel = qobject_cast<QComboBox *>(pinList->itemWidget(pinList->topLevelItem(0), 1));
    QVERIFY(rowLevel && rowLevel->isEnabled());
    const auto before = intent.size();
    rowLevel->setCurrentIndex(rowLevel->findData("micro_facies"));
    emit rowLevel->activated(rowLevel->currentIndex());
    // Row changes are queued (the handler rebuilds the list and deletes this combo).
    QTRY_COMPARE(intent.size(), before + 1);
    QCOMPARE(intent.last()[0].toString(), QString("pinFaciesLevel"));
    QCOMPARE(intent.last()[1].toMap().value("pin_key").toString(), pinKey);
    QCOMPARE(intent.last()[1].toMap().value("display_mode").toString(), QString("micro_facies"));
    QVERIFY(f.work.setPinnedLevel(draft, pinKey, "auto", &error));
    page.refresh();
    QCOMPARE(pinList->topLevelItemCount(), 0);
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
    QCOMPARE(schema.size(), 27);
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
      QVERIFY(!f.attribute("facies_label").toString().isEmpty());
      if (f.attribute("facies_code").toInt() == 1) {
        QCOMPARE(f.attribute("facies_name").toString(), QString("三角洲相"));
        QCOMPARE(f.attribute("subfacies").toString(), QString("三角洲前缘"));
        QCOMPARE(f.attribute("microfacies").toString(),
                 QString("水下分流河道"));
      } else {
        QVERIFY(f.attribute("facies_name").toString().isEmpty());
        QVERIFY(f.attribute("subfacies").toString().isEmpty());
        QVERIFY(f.attribute("microfacies").toString().isEmpty());
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
      // 迁移后微相挂回真实相：相级聚合为三角洲相 + 未预测，微相级仍是逐微相。
      const bool micro = QLatin1String(level) == QLatin1String("micro_facies");
      QCOMPARE(legend.size(), micro ? 4 : 2);
      if (!micro)
        continue;
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
    QCOMPARE(f.work.versionForLayer(original).extra.value("display_mode").toString(), QString("micro_facies"));
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
    {
      // Well prediction points can pin a facies class to a coarser level too.
      const auto schema = f.work.versionForLayer(draft).extra.value("facies").toList();
      QVariant firstCode;
      for (const auto &code : FaciesHierarchyRenderer::codes(layer))
        if (!code.isNull() && FaciesCatalog::find(schema, code).value("code").isValid()) { firstCode = code; break; }
      QVERIFY(firstCode.isValid());
      {
        const auto key = FaciesHierarchy::key(FaciesCatalog::find(schema, firstCode), "facies");
        f.work.styleLayer(draft);
        const auto baseline = f.work.displayLegend(draft);
        QVERIFY2(f.work.setPinnedLevel(draft, key, "facies", &error), qPrintable(error));
        bool pinnedRow = false;
        for (const auto &entry : f.work.displayLegend(draft))
          pinnedRow |= entry.toMap().value("key").toString() == key &&
                       entry.toMap().value("level").toString() == "facies";
        QVERIFY(pinnedRow);
        QVERIFY(f.work.setPinnedLevel(draft, key, "auto", &error));
        QCOMPARE(f.work.displayLegend(draft), baseline);
      }
    }
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
  // 预测专项 review M1：进行中的准备检查被 bindCatalog / cancelPrediction 中止时，
  // 必须发出 wellReadinessReady（带中止原因），否则面板卡在「正在检查…」。
  void wellReadinessAbortedByCatalogRebindRecoversPanel() {
    WellApiServer server;
    server.holdModels = true; // 卡住 GET /models，使检查停在途中
    WellApiEnvironment environment(server);
    Fixture f;
    QVERIFY(f.init());
    QString error;
    QVERIFY(!f.well("A", 10, 20).isEmpty());
    QVERIFY(f.tops("A", QStringLiteral("#WellTops File From SMI\n#WellName Name MD X Y Z TVD Time(ms)\nA D61 1000 0 0 -1000 1000 -99999\nA D62 1060 0 0 -1060 1060 -99999\n")));
    QVERIFY(WellAttributeStore::mergeIntervals(&f.catalog, f.dir.path(), &f.layers,
      {QVariantMap{{"well_id", "A"}, {"top_md", 1000.0}, {"base_md", 1120.0},
        {"lithology", QStringLiteral("维护砂岩")}, {"facies", QStringLiteral("原解释")}}}, &error));
    installRemotePrediction(&f.work, RemotePredictConfig{}, nullptr, {}, &f.work);
    MappingWorkbenchPage page("predict", &f.work);
    page.setHorizon("D61");
    auto *kind = page.findChild<QComboBox *>("predictionKind");
    kind->setCurrentIndex(kind->findData(QStringLiteral("wells")));
    auto *inputs = page.findChild<QListWidget *>("workbenchInputs");
    QCOMPARE(inputs->count(), 1);
    inputs->item(0)->setCheckState(Qt::Checked);
    auto *check = page.findChild<QPushButton *>("wellReadinessCheck");
    auto *summary = page.findChild<QLabel *>("wellReadinessSummary");
    QVERIFY(check && summary);
    QTRY_VERIFY(check->isEnabled());
    QSignalSpy readiness(&f.work, &MappingWorkbench::wellReadinessReady);
    check->click();
    QTRY_COMPARE(server.modelRequests, 1);
    QTRY_VERIFY(summary->text().contains(QStringLiteral("正在检查")));
    QVERIFY(!check->isEnabled());
    QCOMPARE(readiness.size(), 0);
    f.work.bindCatalog(&f.catalog, f.dir.path()); // → cancelPrediction；修复前不发信号
    QTRY_COMPARE_WITH_TIMEOUT(readiness.size(), 1, 5000);
    const auto abortMsg = readiness.first()[2].toString();
    QVERIFY2(!abortMsg.isEmpty(), "中止必须带原因，面板才能离开「正在检查」");
    QVERIFY2(abortMsg.contains(QStringLiteral("中止")), qPrintable(abortMsg));
    QTRY_VERIFY(check->isEnabled());
    QVERIFY2(summary->text().contains(QStringLiteral("未完成")) ||
             summary->text().contains(QStringLiteral("中止")),
             qPrintable(summary->text()));
    // 中止原因跨事件循环仍在（重绑触发的输入重建不得把它清掉）……
    QTest::qWait(50);
    QVERIFY2(summary->text().contains(QStringLiteral("未完成")), qPrintable(summary->text()));
    // ……但勾选真正变化后作废。
    inputs = page.findChild<QListWidget *>("workbenchInputs");
    QCOMPARE(inputs->count(), 1);
    inputs->item(0)->setCheckState(Qt::Unchecked);
    QTRY_VERIFY(summary->text().isEmpty());
    inputs->item(0)->setCheckState(Qt::Checked);
    QTRY_VERIFY(check->isEnabled());
    // 放开服务后再跑通一次，证明 m_readinessId 已清干净。
    server.holdModels = false;
    check->click();
    QTRY_COMPARE_WITH_TIMEOUT(readiness.size(), 2, 10000);
    QVERIFY2(readiness.last()[2].toString().isEmpty(), qPrintable(readiness.last()[2].toString()));
    QCOMPARE(readiness.last()[1].toList().size(), 1);
    QVERIFY(readiness.last()[1].toList().first().toMap().value("ready").toBool());
  }

  // 预测专项 P0：批量逐井准备检查——与正式运行同一条规则链，干跑只查模型
  // 清单（不 POST /predict、不写成果），逐井给出可预测/不可预测原因。
  void wellReadinessDryRunReportsPerWellReasonsWithoutPredicting() {
    WellApiServer server;
    WellApiEnvironment environment(server);
    Fixture f;
    QVERIFY(f.init());
    QString error;
    for (const auto &name : {"A", "B", "C"}) {
      QVERIFY(!f.well(name, 10, 20).isEmpty());
      // B 只有 D62 分层：缺当前层位 D61 → 不可预测。
      const QString d61 = QStringLiteral("%1 D61 1000 0 0 -1000 1000 -99999\n").arg(name);
      QVERIFY(f.tops(name, QStringLiteral("#WellTops File From SMI\n#WellName Name MD X Y Z TVD Time(ms)\n%1%2 D62 1060 0 0 -1060 1060 -99999\n")
                               .arg(QString(name) == "B" ? QString() : d61, name)));
      QVERIFY(WellAttributeStore::mergeIntervals(&f.catalog, f.dir.path(), &f.layers,
        {QVariantMap{{"well_id", name}, {"top_md", 1000.0}, {"base_md", 1120.0},
          {"lithology", QStringLiteral("维护砂岩")}, {"facies", QStringLiteral("原解释")}}}, &error));
    }
    installRemotePrediction(&f.work, RemotePredictConfig{}, nullptr, {}, &f.work);
    MappingWorkbenchPage page("predict", &f.work);
    page.setHorizon("D61");
    auto *kind = page.findChild<QComboBox *>("predictionKind");
    kind->setCurrentIndex(kind->findData(QStringLiteral("wells")));
    QSignalSpy readiness(&f.work, &MappingWorkbench::wellReadinessReady);
    QVERIFY2(f.work.checkWellReadiness("D61", {"A", "B", "C"}, &error), qPrintable(error));
    QVERIFY(!f.work.busy()); // 干跑不占用正式运行
    QTRY_COMPARE_WITH_TIMEOUT(readiness.size(), 1, 10000);
    QCOMPARE(readiness.first()[0].toString(), QString("D61"));
    QVERIFY2(readiness.first()[2].toString().isEmpty(), qPrintable(readiness.first()[2].toString()));
    const auto wells = readiness.first()[1].toList();
    QCOMPARE(wells.size(), 3);
    QHash<QString, QVariantMap> byId;
    for (const auto &v : wells) byId.insert(v.toMap().value("id").toString(), v.toMap());
    QVERIFY(byId["A"].value("ready").toBool());
    QVERIFY(byId["C"].value("ready").toBool());
    QVERIFY(!byId["B"].value("ready").toBool());
    QVERIFY2(byId["B"].value("reason").toString().contains("D61"), qPrintable(byId["B"].value("reason").toString()));
    QCOMPARE(server.modelRequests, 1);
    QCOMPARE(server.bodies.size(), 0);                  // 没有提交任何预测
    QVERIFY(f.latest("wells_prediction").isEmpty());    // 没有写成果
    // 与正式运行口径一致：随后正式运行 A+B+C，只有 A、C 提交。
    QVERIFY2(f.work.predict("D61", "wells", {"A", "B", "C"}, &error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(!f.work.busy(), 10000);
    QCOMPARE(server.bodies.size(), 2);

    // 界面：勾选全部井 → 「检查可预测井」→ 汇总与逐井原因。
    auto *inputs = page.findChild<QListWidget *>("workbenchInputs");
    QCOMPARE(inputs->count(), 3);
    for (int i = 0; i < inputs->count(); ++i) inputs->item(i)->setCheckState(Qt::Checked);
    auto *check = page.findChild<QPushButton *>("wellReadinessCheck");
    QVERIFY(check);
    QTRY_VERIFY(check->isEnabled()); // 勾选变化后下一轮事件循环刷新
    check->click();
    auto *summary = page.findChild<QLabel *>("wellReadinessSummary");
    QTRY_VERIFY_WITH_TIMEOUT(summary->text().contains(QStringLiteral("口不可预测")), 10000);
    QCOMPARE(summary->text(), QStringLiteral("3 口井 / 2 口可预测 / 1 口不可预测"));
    auto *toggle = page.findChild<QPushButton *>("wellReadinessToggle");
    QVERIFY(!toggle->isHidden());
    toggle->click();
    auto *reasons = page.findChild<QListWidget *>("wellReadinessReasons");
    QVERIFY(!reasons->isHidden());
    QCOMPARE(reasons->count(), 3);
  }

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
    for (const auto &link : f.catalog.linksForEntity("A"))
      if (link.role == "well_log")
        QVERIFY(f.catalog.updateVersionExtra(f.catalog.currentVersion(link.assetId).id,
          "synthetic_input", true, &error));
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
    QVERIFY(version.extra.value("synthetic_input").toBool());
    const auto evidence = version.extra.value("evidence").toMap();
    QVERIFY(evidence.value("provenance").toMap().value("synthetic_input").toBool());
    bool syntheticParent = false;
    for (const auto &input : evidence.value("inputs").toList())
      syntheticParent |= input.toMap().value("synthetic_input").toBool();
    QVERIFY(syntheticParent);
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
  // 测井岩性与地层相表在预测后物化，供属性表维护；不进入图层树。
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
    auto *intervals = qobject_cast<QgsVectorLayer *>(
        f.layers.layer(WellAttributeStore::intervalLayerId()));
    QVERIFY(intervals);
    QgsProject *project = formations->project();
    QVERIFY(project && project->layerTreeRoot());
    QVERIFY(!project->layerTreeRoot()->findLayer(formations->id()));
    QVERIFY(!project->layerTreeRoot()->findLayer(intervals->id()));
    QVERIFY(formations->flags().testFlag(QgsMapLayer::Private));
    QVERIFY(intervals->flags().testFlag(QgsMapLayer::Private));
    QCoreApplication::processEvents();
    QCOMPARE(project->mapLayer(formations->id()), static_cast<QgsMapLayer *>(formations));
    QCOMPARE(project->mapLayer(intervals->id()), static_cast<QgsMapLayer *>(intervals));
    QCOMPARE(f.layers.layer(WellAttributeStore::formationLayerId()),
             static_cast<QgsMapLayer *>(formations));
    QCOMPARE(f.layers.layer(WellAttributeStore::intervalLayerId()),
             static_cast<QgsMapLayer *>(intervals));
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
      first.insert("texture", QString());
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
    // 井段属性层：小空心点，不标注（每井多行同位堆叠），且不在图层树里。
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
  void seismicMockRequiresSavedWellPredictions() {
    Fixture f; QVERIFY(f.init());
    const auto input = f.seismic(); QVERIFY(!input.isEmpty());
    QString error;
    QVERIFY(!f.work.predict("D61", "seismic_mock", {input}, &error));
    QVERIFY(error.contains(QStringLiteral("测井相预测")));
    QVERIFY(!f.work.busy());
    QVERIFY(f.latest("seismic_prediction").isEmpty());
  }
  void seismicCalibrationTransformsCoordinatesAndMatchesFrozenClassPaths() {
    QTemporaryDir dir;
    QgsVectorLayer layer("Point?crs=EPSG:4326&field=id:string&field=facies_code:integer", "wells", "memory");
    QgsFeature feature(layer.fields()); feature.setAttribute("id", "W1"); feature.setAttribute("facies_code", 42);
    feature.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(1, 1)));
    QgsFeatureList points{feature}; QVERIFY(layer.dataProvider()->addFeatures(points));
    const auto file = dir.filePath("wells.gpkg"); QString error;
    QVERIFY2(MappingArtifactWriter::vectorSnapshot(&layer, file, &error), qPrintable(error));
    auto frozen = FaciesCatalog::defaults()[1].toMap(); frozen.insert("code", 42);
    const QVariantMap source{{"path", file}, {"sha256", DataCatalog::sha256FileHex(file)},
        {"version_id", "well-v1"}, {"facies", QVariantList{frozen}}};
    QStringList versions;
    const auto wells = readSeismicCalibrationWells({source}, FaciesCatalog::defaults(),
        QgsCoordinateReferenceSystem("EPSG:3857").toWkt(), QgsCoordinateTransformContext(), &versions, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error)); QCOMPARE(wells.size(), 1);
    QCOMPARE(wells.first().code, 2); // same geological path despite different stored codes
    QVERIFY(qAbs(wells.first().x - 111319.49) < 1);
    QVERIFY(qAbs(wells.first().y - 111325.14) < 1);
    QCOMPARE(versions, QStringList{"well-v1"});
    auto altered = source; altered.insert("sha256", "changed");
    QVERIFY(readSeismicCalibrationWells({altered}, FaciesCatalog::defaults(),
        QgsCoordinateReferenceSystem("EPSG:3857").toWkt(), QgsCoordinateTransformContext(), &versions, &error).isEmpty());
    QVERIFY(error.contains(QStringLiteral("已改变")));
  }
  void seismicMockUsesLatestWellMajoritiesAndRecordsTheirVersions() {
    Fixture f; QVERIFY(f.init()); QString error;
    QVERIFY2(f.work.saveFacies("D61", FaciesCatalog::defaults().mid(0, 7), &error), qPrintable(error)); // explicit seven-class numerical fixture

    DerivedAssetRegistrar registrar(&f.catalog, f.dir.path());
    const auto volume = registrar.stage("seismic", "volume", "volume.sgy", &error);
    QVERIFY(volume.isValid());
    QVERIFY(SeismicHorizonFixture::writeFile(volume.absolutePath, SeismicHorizonFixture::buildVolume()));
    QVERIFY(registrar.commit(volume, {}, "fixture", {}, &error));
    const auto horizon = f.dir.filePath("D61.dat");
    QVERIFY(SeismicHorizonFixture::writeFile(horizon, SeismicHorizonFixture::buildHorizon(100)));
    const auto point = [](const QString &id, double x, const QVariant &code) {
      return QVariantMap{{"id", id}, {"x", x}, {"y", 65.0}, {"facies_code", code}};
    };
    const auto save = [&](const QString &name, const QVariantList &points) {
      const auto st = registrar.stage("wells_prediction", name, "wells.geojson", &error);
      if (!st.isValid() || !MappingArtifactWriter::points(st.absolutePath, points, &error) ||
          !registrar.commit(st, {}, "fixture", {{"kind", "wells_prediction"}, {"horizon", "D61"},
              {"mapping_product", true}, {"facies", FaciesCatalog::defaults()}}, &error)) return QString();
      return st.versionId;
    };
    const auto old = save("old", {point("a", 2, 2), point("b", 3, 2), point("c", 4, 2),
        point("d", 12, 7), point("e", 13, 7), point("skip", 14, 7)});
    QVERIFY2(!old.isEmpty(), qPrintable(error));
    const auto current = save("current", {point("a", 2, 7), point("b", 3, 7), point("c", 4, 2),
        point("d", 12, 2), point("e", 13, 2), point("skip", 14, QVariant())});
    QVERIFY2(!current.isEmpty(), qPrintable(error));
    const auto tail = save("later-partial-batch", {point("d", 12, 2)});
    QVERIFY2(!tail.isEmpty(), qPrintable(error));
    QSignalSpy errors(&f.work, &MappingWorkbench::errorOccurred);
    QVERIFY2(f.work.predict("D61", "seismic_mock", {volume.assetId}, &error, horizon), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(!f.work.busy(), 30000);
    QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.first().first().toString()));
    const auto id = f.latest("seismic_prediction"); QVERIFY(!id.isEmpty());
    const auto version = f.work.versionForLayer(id);
    QVERIFY(version.extra.value("mock").toBool());
    QCOMPARE(version.extra.value("display_mode").toString(), QString("micro_facies"));
    f.work.updateDisplayScale(1000000);
    QCOMPARE(f.work.resolvedLevel(id), QString("micro_facies"));
    QCOMPARE(version.extra.value("window_half_ms").toDouble(), 12.0);
    QVERIFY(version.parentVersionIds.contains(current)); QVERIFY(version.parentVersionIds.contains(tail));
    QVERIFY(!version.parentVersionIds.contains(old));
    QCOMPARE(version.extra.value("wells").toList().size(), 5);
    const auto mapping = version.extra.value("cluster_facies_mapping").toList();
    QCOMPARE(mapping.size(), 2);
    QCOMPARE(mapping[0].toMap().value("vote_count").toInt(), 3);
    QCOMPARE(mapping[0].toMap().value("facies_code").toInt(), 7);
    QCOMPARE(mapping[1].toMap().value("vote_count").toInt(), 2);
    QCOMPARE(mapping[1].toMap().value("facies_code").toInt(), 2);
    auto *raster = qobject_cast<QgsRasterLayer *>(f.layers.instantiate(id, &error)); QVERIFY(raster);
    // B-01（2026-10-19 审计第三轮）：SMI 点栅格 extent 半格外扩后，相图
    // 栅格像素中心对齐到样本点本身（x=10·c）——旧配准像素中心在样本间
    // 半格处（x=10·c+5），正是被修复的半道间距错位。采样点随配准校正。
    for (int column = 0; column < 4; ++column) {
      bool ok = false;
      const double code = raster->dataProvider()->sample(QgsPointXY(column * 10, 65), 1, &ok);
      QVERIFY(ok); QCOMPARE(int(code), column % 2 == 0 ? 7 : 2);
    }
  }
  // #536：原生属性表改编码的写路径必须与 place() 读路径同一口径。旧七列错位
  // 草稿（#526 之前存进版本 extra 的元组：microfacies=色值、color=SVG、
  // texture 空）若被原样喂给 FaciesCatalog::attributes()，会把色值写进图层的
  // microfacies、texture 置空——而渲染器/图例走 place() 已修复，同一次编辑
  // 产出自相矛盾的属性与图面。
  void nativeAttributeTableRecodeRecoversLegacySchemaColumns() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto draft = hierarchyDraft(f, &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    f.work.styleLayer(draft);
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft));
    QVERIFY(layer);
    // 草稿版本 extra 建于 #526 之前：就地换回旧七列错位元组（不动版本血缘）。
    const QVariantList legacySchema{
      QVariantMap{{"code", 1}, {"name", QStringLiteral("河道充填")},
        {"facies", QStringLiteral("河流相")}, {"subfacies", QStringLiteral("河床亚相")},
        {"microfacies", QStringLiteral("#E8D5B0")},
        {"color", QStringLiteral("strata/micro_channel_fill.svg")},
        {"texture", QString()}}};
    const auto draftVersion = f.work.versionForLayer(draft);
    QVERIFY2(f.catalog.updateVersionExtra(draftVersion.id,
                                          QVariantMap{{"facies", legacySchema}},
                                          &error),
             qPrintable(error));
    QCOMPARE(f.work.versionForLayer(draft).extra.value("facies").toList(),
             legacySchema);
    QSignalSpy failures(&f.work, &MappingWorkbench::errorOccurred);
    QVERIFY(layer->startEditing());
    // 原生属性表改编码：显式编辑命令（属性表对话框同口径）→ businessEdit →
    // recordSpatialInterpretationEdit 的分类同步分支。取一个编码确实变化的
    // 要素（observer 对「改了又等于原值」的分类编辑不发 businessEdit）。
    const int field = layer->fields().indexOf("facies_code");
    QVERIFY(field >= 0);
    QgsFeature feature;
    auto all = layer->getFeatures();
    while (all.nextFeature(feature))
      if (feature.attribute(field).toInt() != 1)
        break;
    QVERIFY(feature.isValid());
    QVERIFY(feature.attribute(field).toInt() != 1);
    layer->beginEditCommand(QStringLiteral("改编码")); // void 返回，同本文件既有口径不包 QVERIFY
    QVERIFY(layer->changeAttributeValue(feature.id(), field, 1));
    layer->endEditCommand();
    QCOMPARE(failures.count(), 0); // 同步不得报错
    // 写路径取值与渲染侧 place() 一致（错位元组被恢复，不是被照抄）。
    const auto placed = FaciesTaxonomy::place(legacySchema.first().toMap());
    const auto edited = layer->getFeature(feature.id());
    QCOMPARE(edited.attribute("facies_code").toInt(), 1);
    QCOMPARE(edited.attribute("microfacies").toString(),
             placed.value("microfacies").toString());
    QCOMPARE(edited.attribute("facies_label").toString(),
             placed.value("name").toString());
    QCOMPARE(edited.attribute("facies_name").toString(),
             placed.value("facies").toString());
    QCOMPARE(edited.attribute("subfacies").toString(),
             placed.value("subfacies").toString());
    QCOMPARE(edited.attribute("texture").toString(),
             placed.value("texture").toString());
    // 反向锚定：色值不进 microfacies、SVG 不丢成纹理缺失。
    QCOMPARE(edited.attribute("microfacies").toString(),
             QStringLiteral("河道充填"));
    QVERIFY(!edited.attribute("texture").toString().isEmpty());
    QVERIFY(layer->rollBack());
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
    QCOMPARE(v.extra.value("prediction_level").toString(), QString("micro_facies"));
    for (const auto &value : v.extra.value("facies").toList()) {
      const auto leaf = value.toMap();
      QVERIFY(!leaf.value("facies").toString().isEmpty());
      QVERIFY(!leaf.value("subfacies").toString().isEmpty());
      QCOMPARE(leaf.value("microfacies").toString(), leaf.value("name").toString());
    }
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
  void polygonizeWritesValueRangesAndLabelsInterval() {
    // 连续单因素（砂地比型）分级转面：面带 value_min/value_max，标注模式 4
    // 显示分级区间，回答“这块是砂地比多少”（2026-10-17 用户反馈序号无意义）。
    Fixture f;
    QVERIFY(f.init());
    QString error;
    QVERIFY2(f.work.saveFacies("D61", FaciesCatalog::defaults().mid(0, 7), &error), qPrintable(error)); // explicit seven-class numerical fixture

    const auto source = f.dir.filePath("sand-ratio-samples.geojson");
    QVERIFY(MappingArtifactWriter::points(
        source,
        {QVariantMap{{"x", 0}, {"y", 0}, {"z", 0}},
         QVariantMap{{"x", 100}, {"y", 0}, {"z", 20}},
         QVariantMap{{"x", 0}, {"y", 100}, {"z", 40}},
         QVariantMap{{"x", 100}, {"y", 100}, {"z", 60}}},
        &error));
    LayerDeclaration samples;
    samples.layerId = "wells.D61";
    samples.horizon = "D61";
    samples.type = "vector";
    samples.source = source;
    samples.title = QStringLiteral("砂地比样点");
    QVERIFY(f.layers.declare(samples, &error));
    QVERIFY2(f.work.generateFactor(
                  "D61", "sandratio",
                  {{"pointsLayerId", samples.layerId}, {"field", "z"}, {"cellSize", 10}},
                  &error),
             qPrintable(error));
    const auto factor = f.latest("single_factor_raster");
    QVERIFY(!factor.isEmpty());
    // compose() 内部即完成转面：返回值就是相面 layerId（带值域字段）。
    const auto polygon = f.work.compose(
        "D61", {factor},
        {{"thresholds", QVariantList{20., 25., 30., 35., 40., 45.}},
         {"cellSize", 10}},
        &error);
    QVERIFY2(!polygon.isEmpty(), qPrintable(error));
    auto *v =
        qobject_cast<QgsVectorLayer *>(f.layers.instantiate(polygon, &error));
    QVERIFY2(v, qPrintable(error));
    QVERIFY(v->fields().indexOf("value_min") >= 0);
    QVERIFY(v->fields().indexOf("value_max") >= 0);
    double minCode = 1e9, maxCode = -1e9;
    QgsFeature feat;
    auto it = v->getFeatures();
    QHash<int, std::pair<QVariant, QVariant>> ranges;
    while (it.nextFeature(feat)) {
      const int code = feat.attribute("facies_code").toInt();
      ranges.insert(code, {feat.attribute("value_min"), feat.attribute("value_max")});
      minCode = std::min(minCode, double(code));
      maxCode = std::max(maxCode, double(code));
    }
    QVERIFY(!ranges.isEmpty());
    // 首类无下界、末类无上界，中间类区间端点即相邻阈值。
    // 数据库 NULL 读回是 valid-but-null 的 QVariant：口径用 isNull()。
    const auto emptyValue = [](const QVariant &v) {
      return !v.isValid() || v.isNull();
    };
    QVERIFY(ranges.contains(int(minCode)));
    QVERIFY(emptyValue(ranges[int(minCode)].first));
    QCOMPARE(ranges[int(minCode)].second.toDouble(), 20.0);
    QVERIFY(ranges.contains(int(maxCode)));
    QCOMPARE(ranges[int(maxCode)].first.toDouble(), 45.0);
    QVERIFY(emptyValue(ranges[int(maxCode)].second));
    for (auto r = ranges.begin(); r != ranges.end(); ++r) {
      if (!emptyValue(r.value().first) && !emptyValue(r.value().second))
        QVERIFY(r.value().first.toDouble() < r.value().second.toDouble());
    }
    // 标注模式 4：表达式含值域字段；labelMode 接受并持久化。
    MappingArtifactWriter::applyFaciesLabels(v, 4);
    auto *simple = dynamic_cast<QgsVectorLayerSimpleLabeling *>(v->labeling());
    QVERIFY(simple);
    QVERIFY(simple->settings().isExpression);
    QVERIFY(simple->settings().fieldName.contains("value_min"));
    QVERIFY(simple->settings().fieldName.contains("value_max"));
    QVERIFY(!simple->settings().fieldName.contains("$id"));
    QVERIFY(f.work.setLabelMode(polygon, 4, &error));
    QCOMPARE(f.work.labelMode(polygon), 4);
    // 无值域字段的层（如地震相面）：模式 4 回退相名，不引用缺失字段。
    const auto input = f.seismic();
    QVERIFY(!input.isEmpty());
    QVERIFY2(f.work.predict("D61", "seismic", {input}, &error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(!f.work.busy(), 5000);
    const auto seismicPolygon =
        f.work.polygonize(f.latest("seismic_prediction"), &error);
    QVERIFY2(!seismicPolygon.isEmpty(), qPrintable(error));
    auto *sv = qobject_cast<QgsVectorLayer *>(
        f.layers.instantiate(seismicPolygon, &error));
    QVERIFY2(sv, qPrintable(error));
    QVERIFY(sv->fields().indexOf("value_min") < 0);
    MappingArtifactWriter::applyFaciesLabels(sv, 4);
    const auto fallback =
        dynamic_cast<QgsVectorLayerSimpleLabeling *>(sv->labeling())
            ->settings().fieldName;
    QVERIFY(!fallback.contains("value_min"));
    QVERIFY(fallback.contains("facies"));
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
        QVariantMap{{"code", 7}, {"name", "水下分流河道"}, {"facies", "三角洲"},
                    {"subfacies", "三角洲前缘"}, {"microfacies", "水下分流河道"},
                    {"color", "#E6C875"}},
        QVariantMap{{"code", 9}, {"name", "湖相泥"}, {"facies", "湖泊相"}, {"subfacies", "半深湖"}, {"microfacies", "湖相泥"}, {"color", "#97B4CE"}}};
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
    QVERIFY2(f.work.saveFacies("D61", FaciesCatalog::defaults().mid(0, 7), &error), qPrintable(error)); // explicit seven-class numerical fixture

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
        "D61", {factorId}, {{"thresholds", QVariantList{10., 20., 30., 40., 50., 60.}}}, &error);
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
  // 回归：编图流程页的等值线走 m_taskSvc 手工任务（不经 runFactorWork，
  // 发布时 factorBusy()==false）——成果仍须默认激活：补 product.<v> 别名、
  // 发 productReady、进成果列表。此前 publishFactorAlias 按 factorBusy()
  // 门控把该路径挡在成果外。
  void generatedContourActivatesForNonWorkbenchPublish() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto source = f.dir.filePath("samples.geojson");
    QVERIFY(MappingArtifactWriter::points(
        source,
        {QVariantMap{{"x", 0}, {"y", 0}, {"z", 0}},
         QVariantMap{{"x", 100}, {"y", 0}, {"z", 20}},
         QVariantMap{{"x", 0}, {"y", 100}, {"z", 40}},
         QVariantMap{{"x", 100}, {"y", 100}, {"z", 60}}},
        &error));
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
    const QVariantMap params{
        {"pointsLayerId", d.layerId}, {"field", "z"}, {"cellSize", 10}};
    QVERIFY2(f.work.generateFactor("D61", "sandthick", params, &error),
             qPrintable(error));
    QSignalSpy ready(&f.work, &MappingWorkbench::productReady);
    // 直调 ConstraintWorkflow（同步发布，factorBusy()==false，与约束页手工
    // 任务路径的发布时序等价）。
    QVERIFY2(f.constraints.generateContours("D61", "factor.D61.sandthick", 10,
                                            &error),
             qPrintable(error));
    QCOMPARE(ready.count(), 1);
    const auto contours = f.latest("contour_lines");
    QVERIFY(!contours.isEmpty());
    QCOMPARE(ready.first().at(0).toString(), QString("D61"));
    QCOMPARE(ready.first().at(1).toString(), contours);
    QVERIFY2(f.layers.instantiate(contours, &error), qPrintable(error));
  }
  // productReady 对每个生成的等值线恰好发射一次：同步/异步都不双发
  //（发布点唯一——contoursGenerated→publishFactorAlias 信号链）。
  void generatedContourReadyEmittedOnce() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto source = f.dir.filePath("samples.geojson");
    QVERIFY(MappingArtifactWriter::points(
        source,
        {QVariantMap{{"x", 0}, {"y", 0}, {"z", 0}},
         QVariantMap{{"x", 100}, {"y", 0}, {"z", 20}},
         QVariantMap{{"x", 0}, {"y", 100}, {"z", 40}},
         QVariantMap{{"x", 100}, {"y", 100}, {"z", 60}}},
        &error));
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
    const QVariantMap params{
        {"pointsLayerId", d.layerId}, {"field", "z"}, {"cellSize", 10}};
    QVERIFY2(f.work.generateFactor("D61", "sandthick", params, &error),
             qPrintable(error));
    // 同步路径：一次发射。
    {
      QSignalSpy ready(&f.work, &MappingWorkbench::productReady);
      QVERIFY2(f.work.generateContours("D61", "factor.D61.sandthick", 10,
                                       &error),
               qPrintable(error));
      QCOMPARE(ready.count(), 1);
    }
    // 任务中心路径：同样一次发射（早退 + 信号收尾，不再叠加旧尾部自发布）。
    {
      PaleoTaskService tasks;
      f.constraints.setTaskService(&tasks);
      f.work.setTaskService(&tasks);
      QSignalSpy ready(&f.work, &MappingWorkbench::productReady);
      QVERIFY2(f.work.generateContours("D61", "factor.D61.sandthick", 10,
                                       &error),
               qPrintable(error));
      QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 1, 15000);
      QCOMPARE(ready.count(), 1);
      QVERIFY(!f.latest("contour_lines").isEmpty());
      f.constraints.setTaskService(nullptr);
      f.work.setTaskService(nullptr);
    }
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
    // 隐藏页不自动刷新；测井已是默认类型时 setCurrentIndex 不发变化信号。
    page.refresh();
    QCOMPARE(inputs->count(), 2);
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
  void removeUnregisteredProductPreservesDataAndOtherLayers() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto path = f.dir.filePath("legacy-prediction.geojson");
    QVERIFY(MappingArtifactWriter::points(path, {QVariantMap{{"x", 0}, {"y", 0}, {"z", 10}}}, &error));
    LayerDeclaration orphan;
    orphan.layerId = "product.legacy-prediction";
    orphan.title = "D61 测井相预测 · v1";
    orphan.horizon = "D61"; orphan.type = "vector"; orphan.source = path;
    QVERIFY(f.layers.declare(orphan, &error));
    auto reference = orphan; reference.layerId = "wells.D61";
    QVERIFY(f.layers.declare(reference, &error));
    QPointer<QgsMapLayer> removed = f.layers.instantiate(orphan.layerId, &error);
    QPointer<QgsMapLayer> kept = f.layers.instantiate(reference.layerId, &error);
    QVERIFY(removed); QVERIFY(kept);
    QVERIFY(f.work.versionForLayer(orphan.layerId).id.isEmpty());
    MappingWorkbenchPage page("compose", &f.work);
    page.setHorizon("D61"); page.selectLayer(orphan.layerId);
    auto *results = page.findChild<QTreeWidget *>("workbenchResults");
    QVERIFY(results); QVERIFY(results->currentItem());
    QCOMPARE(results->currentItem()->text(1), QString("未登记"));
    QVERIFY(page.commandButton("delete")->isEnabled());
    const auto products = f.work.removalProducts(orphan.layerId, &error);
    QCOMPARE(products.size(), 1);
    QVERIFY(products.first().toMap().value("declaration_only").toBool());
    QCOMPARE(products.first().toMap().value("title").toString(), orphan.title);
    QVERIFY2(f.work.removeProduct(orphan.layerId, &error), qPrintable(error));
    QVERIFY(removed.isNull()); QVERIFY(kept);
    QVERIFY(QFile::exists(path));
    QVERIFY(f.work.declaration(orphan.layerId).layerId.isEmpty());
    QVERIFY(!f.work.declaration(reference.layerId).layerId.isEmpty());
    for (const auto &row : f.work.products("D61"))
      QVERIFY(row.toMap().value("id").toString() != orphan.layerId);
    QVERIFY(f.project.openProject(f.project.projectPath()));
    for (auto *layer : f.project.project()->mapLayers())
      QVERIFY(layer->customProperty("paleoLayerId").toString() != orphan.layerId);
    QVERIFY(f.work.declaration(orphan.layerId).layerId.isEmpty());
    QVERIFY(QFile::exists(path));
  }
  void removeUnregisteredDraftWithoutSource() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    LayerDeclaration orphan;
    orphan.layerId = "draft.missing-source";
    orphan.type = "vector"; orphan.source = f.dir.filePath("missing.gpkg");
    QVERIFY(f.layers.declare(orphan, &error));
    QVERIFY2(f.work.removeProduct(orphan.layerId, &error), qPrintable(error));
    QVERIFY(f.work.declaration(orphan.layerId).layerId.isEmpty());
    QVERIFY(!f.work.removeProduct(orphan.layerId, &error));
    QVERIFY(!error.isEmpty());
  }
  void unregisteredRemovalRespectsEditingAndReadOnlyGuards() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto path = f.dir.filePath("orphan-edit.geojson");
    QVERIFY(MappingArtifactWriter::points(path, {QVariantMap{{"x", 0}, {"y", 0}, {"z", 10}}}, &error));
    LayerDeclaration orphan;
    orphan.layerId = "draft.orphan-edit"; orphan.type = "vector"; orphan.source = path;
    QVERIFY(f.layers.declare(orphan, &error));
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(orphan.layerId, &error));
    QVERIFY(layer); QVERIFY(layer->startEditing());
    QVERIFY(!f.work.removeProduct(orphan.layerId, &error));
    QVERIFY2(error.contains("编辑"), qPrintable(error));
    QVERIFY(!f.work.declaration(orphan.layerId).layerId.isEmpty());
    QVERIFY(layer->isEditable()); QVERIFY(layer->rollBack());
    f.manifest.setReadOnly(true);
    QVERIFY(!f.work.removeProduct(orphan.layerId, &error));
    QVERIFY(!f.work.declaration(orphan.layerId).layerId.isEmpty());
    QVERIFY(f.layers.layer(orphan.layerId)); QVERIFY(QFile::exists(path));
    f.manifest.setReadOnly(false);
    f.catalog.setLockedReadOnly(true);
    QVERIFY(!f.work.removeProduct(orphan.layerId, &error));
    QVERIFY(!f.work.declaration(orphan.layerId).layerId.isEmpty());
    f.catalog.setLockedReadOnly(false);
    QVERIFY(!f.work.removeProduct("wells.D61", &error));
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
    // #499 句柄生命周期审计：工作副本（features.gpkg）不得被任何活层持有——
    // copyForEditing 只声明 draft 层（惰性实例化），编辑探测用的栈层在函数
    // 返回前已析构。Windows 上若有层/GDALDataset 持有目录内文件，目录删除
    // 必败；此断言把「删前无活层」纪律锁进测试（泄漏路径当场红）。
    QVERIFY(f.layers.layer(draft) == nullptr);
    const auto draftVersion = f.work.versionForLayer(draft);
    QVERIFY(!draftVersion.assetId.isEmpty());
    QVERIFY2(f.work.removeProduct(draft, &error), qPrintable(error));
    QVERIFY(f.work.declaration(draft).layerId.isEmpty());
    QVERIFY(f.catalog.assetById(draftVersion.assetId).id.isEmpty());
    // #499：removeProduct 的文件回收（受管文件 + 工作副本目录）在任务池后台
    // 执行（产品契约：记录同步删、文件异步收）。此处确定性等待已知后台任务
    // 收尾（同 tst_imagelod 先例）——不是「重试等待删除成功」：若存在句柄
    // 泄漏，等待完成后目录断言仍然红，泄漏不会被掩盖。
    QVERIFY2(QThreadPool::globalInstance()->waitForDone(30000),
             "background purge did not finish");
    QVERIFY(!QDir(workDir).exists());
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
  void backgroundConsistencyReportPublishesAcrossFilesystems() {
    // C-2（full-audit-20261021）：后台一致性报告的暂存目录必须建在目标
    // 目录（工程 artifacts/reports 内）——旧实现写默认临时目录再 rename
    // 进工程，跨文件系统（/tmp 独立 tmpfs 的常见 Linux 布局）恒 EXDEV，
    // 报告永远发布失败。TMPDIR 指到 /dev/shm 构造跨文件系统红态。
    const QByteArray priorTmp = qgetenv("TMPDIR");
    {
      QTemporaryDir shmProbe(QStringLiteral("/dev/shm/paleo-c2-probe-XXXXXX"));
      if (!shmProbe.isValid() || shmProbe.path() == QDir::tempPath())
        QSKIP("需要与默认临时目录不同的文件系统（/dev/shm）");
    }
    Fixture f;
    QVERIFY(f.init());
    QVERIFY(!f.well("A", 10, 20).isEmpty());
    QVERIFY(!f.well("B", 300, 200).isEmpty());
    QString error;
    QVERIFY(f.work.predict("D61", "wells", {"A", "B"}, &error));
    QTRY_VERIFY(!f.work.busy());
    const auto wells = f.latest("wells_prediction");
    QVERIFY(!wells.isEmpty());
    // 比对需要两侧：补地震相预测（与 faciesConsistencyReportsWellVsSeismic
    // 同夹具口径）。
    const auto input = f.seismic();
    QVERIFY(!input.isEmpty());
    QVERIFY2(f.work.predict("D61", "seismic", {input}, &error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(!f.work.busy(), 5000);

    PaleoTaskService tasks;
    f.work.setTaskService(&tasks);
    QSignalSpy ready(&f.work, &MappingWorkbench::consistencyReady);
    QSignalSpy failed(&f.work, &MappingWorkbench::errorOccurred);
    qputenv("TMPDIR", "/dev/shm");
    QVERIFY(f.work.startFaciesConsistency(wells, &error));
    QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 30000);
    if (!priorTmp.isEmpty())
      qputenv("TMPDIR", priorTmp.constData());
    else
      qunsetenv("TMPDIR");
    QVERIFY2(ready.size() == 1,
             qPrintable(failed.isEmpty() ? QStringLiteral("consistencyReady 未发出")
                                         : failed.first().first().toString()));
    const QVariantMap report = ready.first().first().toMap();
    const QString path = report.value("report_path").toString();
    QVERIFY2(!path.isEmpty(), "报告必须带发布路径");
    QVERIFY2(QFile::exists(path), qPrintable(path));
    QVERIFY(path.startsWith(f.dir.path())); // 落在工程内，非系统临时区
    // 暂存目录随发布清理，不留 .consistency-* 残留
    const QStringList leftovers = QDir(f.dir.filePath("artifacts/reports"))
                                      .entryList({QStringLiteral(".consistency-*")},
                                                 QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(leftovers.size(), 0);
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
