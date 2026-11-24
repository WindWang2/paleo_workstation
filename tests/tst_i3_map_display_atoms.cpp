// 层：测试壳
// I3/tst_i3_map_display_atoms — 地图显示原子的真实 runner 链：
//   工具表可发现/精确名称分派（I4 隔离）→ AiChatToolRunner 准入 →
//   CapabilityHost 效果门 → AgentMapDisplayAdapter（原生 QGIS）→ 回读。
// oracle 用原生树节点/renderer/opacity/QgsProject 脏状态与 manifest 实例
// 化事实，不测 fake provider 的 JSON 解码。guiEvidenceScenario 仅在
// PALEO_I3_GUI_EVIDENCE_DIR 设置时执行（有窗口 GUI 人工观察；offscreen
// 车道自动跳过，不冒充 GUI 证据）。
#include <QtTest>
#include "../src/ai/chat/chatmessage.h"
#include "../src/ai/chat/domaintools.h"
#include "../src/ai/chat/mapdisplaytools.h"
#include "../src/catalog/datacatalog.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/projectlock.h"
#include "../src/qgis/agentmapdisplayadapter.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/services/paleotaskservice.h"
#include "../src/workflow/aichattoolrunner.h"
#include "../src/workflow/capabilityhost.h"

#include <qgsapplication.h>
#include <qgsrenderer.h>
#include <qgsgeometry.h>
#include <qgslayertree.h>
#include <qgslayertreemapcanvasbridge.h>
#include <qgsmapcanvas.h>
#include <qgsmaplayerstylemanager.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsvectorfilewriter.h>
#include <qgsvectorlayer.h>

#include "../src/qgis/geopatterns.h"
#include "../src/qgis/qgisstyleservice.h"
#include "../src/ui/layers/layerpropertiesdialog.h" // GUI 证据：属性业务页
#include "../src/ui/layers/layertreepanel.h"        // GUI 证据：原生图层树

#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUuid>
#include <QVBoxLayout>
#include <functional>

using namespace paleo;
using namespace paleo::capability;
using namespace paleo::agent::mapdisplay;

namespace {
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

// 测试 GPKG：点/线/面三张表（内存层经 QgsVectorFileWriter 落 OGR 真源）。
bool writeMemoryToGpkg(const QString &memorySpec, const QString &path, const QString &layerName,
                       bool appendToExisting, const std::function<void(QgsVectorLayer *)> &fill) {
  QgsVectorLayer memory(memorySpec, layerName, QStringLiteral("memory"));
  if (!memory.isValid())
    return false;
  fill(&memory);
  QgsVectorFileWriter::SaveVectorOptions options;
  options.driverName = QStringLiteral("GPKG");
  options.layerName = layerName;
  options.actionOnExistingFile = appendToExisting
                                     ? QgsVectorFileWriter::CreateOrOverwriteLayer
                                     : QgsVectorFileWriter::CreateOrOverwriteFile;
  QString error;
  const auto result = QgsVectorFileWriter::writeAsVectorFormatV3(
      &memory, path, QgsProject::instance()->transformContext(), options, &error);
  if (result != QgsVectorFileWriter::NoError)
    qWarning("gpkg write failed: %s", qPrintable(error));
  return result == QgsVectorFileWriter::NoError;
}

bool makeTestGpkg(const QString &path) {
  if (!writeMemoryToGpkg(QStringLiteral("Point?crs=EPSG:32650&field=name:string(32)"), path,
                         QStringLiteral("pts"), false, [](QgsVectorLayer *layer) {
                           QgsFeature f(layer->fields());
                           f.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(1000, 2000)));
                           layer->addFeature(f);
                         }))
    return false;
  if (!writeMemoryToGpkg(QStringLiteral("LineString?crs=EPSG:32650&field=name:string(32)"), path,
                         QStringLiteral("lns"), true, [](QgsVectorLayer *layer) {
                           QgsFeature f(layer->fields());
                           f.setGeometry(QgsGeometry::fromPolylineXY(
                               {QgsPointXY(0, 0), QgsPointXY(3000, 1500)}));
                           layer->addFeature(f);
                         }))
    return false;
  return writeMemoryToGpkg(QStringLiteral("Polygon?crs=EPSG:32650&field=name:string(32)"), path,
                           QStringLiteral("pol"), true, [](QgsVectorLayer *layer) {
                             QgsFeature f(layer->fields());
                             f.setGeometry(QgsGeometry::fromPolygonXY(
                                 {{QgsPointXY(0, 0), QgsPointXY(2000, 0), QgsPointXY(2000, 2000),
                                   QgsPointXY(0, 0)}}));
                             layer->addFeature(f);
                           });
}

// 4×4 ASCII 栅格（AAIGrid，GDAL 原生可读；不依赖 GDAL 写 API）。
bool makeTestAsc(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    return false;
  QTextStream out(&file);
  out << "NCOLS 4\nNROWS 4\nXLLCORNER 0\nYLLCORNER 0\nCELLSIZE 1000\nNODATA_VALUE -9999\n";
  for (int row = 0; row < 4; ++row)
    out << "1 2 3 4\n";
  return true;
}

LayerDeclaration declFor(const QString &layerId, const QString &horizon, const QString &type,
                         const QString &source) {
  LayerDeclaration d;
  d.layerId = layerId;
  d.horizon = horizon;
  d.type = type;
  d.source = source;
  d.group = QStringLiteral("04_SingleFactor");
  d.title = layerId;
  return d;
}

// 每用例独立夹具：manifest/层服务/适配器/宿主/runner 全新，QgsProject 用
// 单例（与 tst_layerservice 同口径），cleanup 里清层清树。
struct DisplayFixture {
  QTemporaryDir root;
  QString gpkg;
  QString asc;
  LayerManifest manifest;
  QgisLayerService layers{nullptr, &manifest};
  AgentMapDisplayAdapter adapter{nullptr, &layers, nullptr};
  DataCatalog catalog;
  std::unique_ptr<ProjectDirLock> lock;
  PaleoTaskService tasks;
  CapabilityHost host{NativeAdapterFactory([](const HostBinding &) {
    CapabilityExecutionAdapter adapter;
    adapter.validateFrozen = [](const OperationLog::Record &) { return QString(); };
    adapter.restoreInputValidator = [](const OperationLog::Record &, QString *) {
      return OperationInputValidator{};
    };
    return adapter;
  }), nullptr};
  AiChatToolRunner runner;
  QString documentId = QStringLiteral("doc-i3");

  // ids: i3.pts / i3.lns / i3.pol / i3.ras 实例化；i3.other 仅声明。
  QString pts = QStringLiteral("i3.pts"), lns = QStringLiteral("i3.lns"),
          pol = QStringLiteral("i3.pol"), ras = QStringLiteral("i3.ras"),
          other = QStringLiteral("i3.other");

  explicit DisplayFixture(bool readOnlyHost = false) : manifest(root.path() + "/project.sqlite") {
    gpkg = root.filePath("layers.gpkg");
    asc = root.filePath("grid.asc");
    QVERIFY2(makeTestGpkg(gpkg), "test gpkg");
    QVERIFY2(makeTestAsc(asc), "test asc");
    QVERIFY2(manifest.open(), "manifest open");
    QVERIFY2(layers.declare(declFor(pts, "T1", "vector", gpkg + "|layername=pts")), "declare pts");
    QVERIFY2(layers.declare(declFor(lns, "T1", "vector", gpkg + "|layername=lns")), "declare lns");
    QVERIFY2(layers.declare(declFor(pol, "T1", "vector", gpkg + "|layername=pol")), "declare pol");
    QVERIFY2(layers.declare(declFor(ras, "T1", "raster", asc)), "declare ras");
    QVERIFY2(layers.declare(declFor(other, "T2", "vector", gpkg + "|layername=pts")),
             "declare other");
    QString error;
    for (const QString &id : {pts, lns, pol, ras})
      QVERIFY2(layers.instantiate(id, &error) != nullptr, qPrintable(id + ": " + error));

    lock = std::make_unique<ProjectDirLock>(root.path());
    QVERIFY2(lock->tryLock(), "project lock");
    QVERIFY2(catalog.open(root.path()), "catalog open");
    host.setTaskService(&tasks);
    host.setCallerScope(paleo::host::Caller::Agent, QStringLiteral("agent:i3"));
    host.setBindingProvider([this, readOnlyHost] {
      HostBinding binding;
      binding.context.open = true;
      binding.context.documentId = documentId;
      binding.context.epoch = 1;
      binding.context.persistentIdentity = true; // 只读工程仍具持久身份
      binding.context.readOnly = readOnlyHost;
      binding.context.ownsWrites = !readOnlyHost;
      // 生产同口径（capabilityhostwiring）：效果集固定含 Presentation，只读
      // 拒绝来自 readOnly/ownsWrites 闸而不是收窄效果集。
      binding.context.allowedEffects = unsigned(host::Read | host::Presentation);
      binding.projectDir = root.path();
      binding.catalog = &catalog;
      binding.writeClaim = readOnlyHost ? nullptr : lock->claim();
      return binding;
    });
    AiChatToolContext context;
    context.capabilityHost = &host;
    context.requireCapabilityHost = true;
    context.documentId = documentId;
    context.documentEpoch = 1;
    context.projectDir = root.path();
    runner.setContext(context);
    runner.setTaskService(&tasks);
    runner.setMapDisplay(&adapter, &adapter);
  }

  QgsVectorLayer *vectorLayer(const QString &id) const {
    return qobject_cast<QgsVectorLayer *>(layers.layer(id));
  }
  QgsRasterLayer *rasterLayer(const QString &id) const {
    return qobject_cast<QgsRasterLayer *>(layers.layer(id));
  }
  QgsLayerTreeLayer *treeNode(QgsMapLayer *layer) const {
    return QgsProject::instance()->layerTreeRoot()->findLayer(layer->id());
  }

  bool runTool(const QString &name, const QJsonObject &args, bool *ok, QJsonObject *payload) {
    QSignalSpy finished(&runner, &AiChatToolRunner::toolFinished);
    runner.run({QStringLiteral("call-") + uuid(), name,
                QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))});
    for (int i = 0; i < 500 && finished.isEmpty(); ++i) {
      QCoreApplication::processEvents();
      QThread::msleep(1);
    }
    if (finished.isEmpty())
      return false;
    const auto record = finished.takeFirst();
    *ok = record.at(1).toBool();
    *payload = QJsonDocument::fromJson(record.at(2).toString().toUtf8()).object();
    return true;
  }
  QJsonObject get(const QString &id) {
    bool ok = false;
    QJsonObject payload;
    runTool("paleo.map_layer_get", {{"layer_id", id}}, &ok, &payload);
    return payload;
  }
  QString tokenOf(const QString &id) { return get(id).value("state_token").toString(); }
};

QStringList layerIdsIn(const QJsonObject &listResult) {
  QStringList ids;
  for (const auto &value : listResult.value("layers").toArray())
    ids.append(value.toObject().value("layer_id").toString());
  return ids;
}
} // namespace

class TestI3MapDisplayAtoms : public QObject {
  Q_OBJECT

private slots:
  void initTestCase() { QVERIFY(QgsApplication::instance() != nullptr); }
  void cleanup() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QgsProject::instance()->removeAllMapLayers();
    QgsProject::instance()->clear();
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  }

  void registryAndDispatchIsolation();
  void listManagedLayersAndPagination();
  void listDoesNotInstantiateOrCreateCanvas();
  void cursorExpiresOnDisplayChange();
  void getLayerStateAndOptions();
  void setVisibilityFlow();
  void setVisibilityParentGroupHidden();
  void setOpacityVectorAndRaster();
  void setOpacityUserModifyDuringApply();
  void applyNamedPreset();
  void applySemanticSymbol();
  void readOnlyProjectRejectsMutationsBeforeSetters();
  void hostAndProviderGates();
  void editingLayerPolicy();
  void replayAndIdempotency();
  void runnerContractAndI4Isolation();
  void strictArgumentRejections();

private:
  void pump() {
    for (int i = 0; i < 20; ++i)
      QCoreApplication::processEvents();
  }
};

void TestI3MapDisplayAtoms::registryAndDispatchIsolation() {
  const QVector<AiToolSpec> tools = builtinAiToolSpecs();
  QStringList names;
  for (const AiToolSpec &spec : tools)
    names.append(spec.name);
  for (const char *expected : {"paleo.map_layers", "paleo.map_layer_get",
                               "paleo.map_layer_set_visibility", "paleo.map_layer_set_opacity",
                               "paleo.map_layer_apply_style"})
    QVERIFY2(names.contains(QLatin1String(expected)), expected);
  const AiToolSpec list = aiToolSpec(QStringLiteral("paleo.map_layers"));
  QVERIFY(!list.name.isEmpty());
  QCOMPARE(unsigned(list.effects), unsigned(paleo::host::Read));
  for (const char *mutation : {"paleo.map_layer_set_visibility", "paleo.map_layer_set_opacity",
                               "paleo.map_layer_apply_style"}) {
    const AiToolSpec spec = aiToolSpec(QString::fromLatin1(mutation));
    QCOMPARE(unsigned(spec.effects), unsigned(paleo::host::Read | paleo::host::Presentation));
  }
  QVERIFY(!list.resultSchemaJson.isEmpty());
  QJsonParseError parseError;
  QJsonDocument::fromJson(list.resultSchemaJson.toUtf8(), &parseError);
  QCOMPARE(parseError.error, QJsonParseError::NoError);
  // 精确名称集合：I4 名称与前缀变体不被吞。
  QVERIFY(isMapDisplayTool(QStringLiteral("paleo.map_layers")));
  QVERIFY(!isMapDisplayTool(QStringLiteral("paleo.map_locate")));
  QVERIFY(!isMapDisplayTool(QStringLiteral("paleo.map_layers_v2")));
  QVERIFY(!isMapDisplayTool(QStringLiteral("paleo.map_")));
  const auto locate = dispatchAiTool(QStringLiteral("paleo.map_locate"), {});
  QCOMPARE(locate.status, AiToolDispatchStatus::NotFound);
  for (const char *name : {"paleo.map_layers", "paleo.map_layer_get"}) {
    const auto dispatch = dispatchAiTool(QString::fromLatin1(name), {});
    if (QLatin1String(name) == QLatin1String("paleo.map_layer_get"))
      QCOMPARE(dispatch.status, AiToolDispatchStatus::Disabled); // 缺必填 layer_id
    else
      QCOMPARE(dispatch.status, AiToolDispatchStatus::Routed);
  }
}

void TestI3MapDisplayAtoms::listManagedLayersAndPagination() {
  DisplayFixture fx;
  bool ok = false;
  QJsonObject payload;
  QVERIFY(fx.runTool("paleo.map_layers", {{"page_size", 3}}, &ok, &payload));
  QVERIFY(ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("ok"));
  QVERIFY(payload.value("has_more").toBool());
  QVERIFY(!payload.value("next_cursor").toString().isEmpty());
  // 字典序：i3.lns, i3.other, i3.pol | i3.pts, i3.ras
  QCOMPARE(layerIdsIn(payload), (QStringList{fx.lns, fx.other, fx.pol}));
  QVERIFY2(!layerIdsIn(payload).contains(fx.other) == false, "unmaterialized declared listed");
  QVERIFY(fx.runTool("paleo.map_layers", {{"page_size", 3}, {"cursor", payload.value("next_cursor").toString()}},
                     &ok, &payload));
  QVERIFY(ok);
  QCOMPARE(layerIdsIn(payload), (QStringList{fx.pts, fx.ras}));
  QVERIFY(!payload.value("has_more").toBool());
  // 手工层不入目录。
  auto *manual = new QgsVectorLayer(fx.gpkg + "|layername=pts", QStringLiteral("manual"),
                                    QStringLiteral("ogr"));
  QgsProject::instance()->addMapLayer(manual);
  QVERIFY(fx.runTool("paleo.map_layers", {}, &ok, &payload));
  QVERIFY(ok);
  QVERIFY(!layerIdsIn(payload).contains(manual->id()));
  QVERIFY(!layerIdsIn(payload).contains(QStringLiteral("manual")));
  // 筛选：horizon 精确 + type 精确。
  QVERIFY(fx.runTool("paleo.map_layers", {{"horizon", "T2"}}, &ok, &payload));
  QVERIFY(ok);
  QCOMPARE(layerIdsIn(payload), QStringList{fx.other});
  QVERIFY(fx.runTool("paleo.map_layers", {{"type", "raster"}}, &ok, &payload));
  QVERIFY(ok);
  QCOMPARE(layerIdsIn(payload), QStringList{fx.ras});
}

void TestI3MapDisplayAtoms::listDoesNotInstantiateOrCreateCanvas() {
  DisplayFixture fx;
  QVERIFY(!fx.layers.isInstantiated(fx.other));
  bool ok = false;
  QJsonObject payload;
  QVERIFY(fx.runTool("paleo.map_layers", {}, &ok, &payload));
  QVERIFY(ok);
  QVERIFY(fx.runTool("paleo.map_layers", {{"horizon", "T2"}}, &ok, &payload));
  QVERIFY(ok);
  QVERIFY(fx.runTool("paleo.map_layer_get", {{"layer_id", fx.other}}, &ok, &payload));
  QVERIFY(ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("not_materialized"));
  QVERIFY(!payload.value("materialized").toBool());
  QVERIFY(!fx.layers.isInstantiated(fx.other)); // 查询零实例化
  QVERIFY(fx.adapter.unavailabilityReason().isEmpty());
  // 适配器未接画布控制器：canvas_present=false（未触发惰性创建——本夹具
  // 根本没有画布可创建）。
  QVERIFY(fx.runTool("paleo.map_layers", {}, &ok, &payload));
  QVERIFY(!payload.value("canvas_present").toBool());
}

void TestI3MapDisplayAtoms::cursorExpiresOnDisplayChange() {
  DisplayFixture fx;
  bool ok = false;
  QJsonObject payload;
  QVERIFY(fx.runTool("paleo.map_layers", {{"page_size", 2}}, &ok, &payload));
  QVERIFY(ok);
  const QString cursor = payload.value("next_cursor").toString();
  QVERIFY(!cursor.isEmpty());
  const QString token = fx.tokenOf(fx.pts);
  QVERIFY(fx.runTool("paleo.map_layer_set_visibility",
                     {{"layer_id", fx.pts}, {"visible", false}, {"state_token", token}}, &ok,
                     &payload));
  QVERIFY(ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("applied"));
  // 显示事实变化后旧游标明确作废（不静默回第一页）。
  QVERIFY(fx.runTool("paleo.map_layers", {{"page_size", 2}, {"cursor", cursor}}, &ok, &payload));
  QVERIFY(!ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("cursor_expired"));
  // 筛选条件变化同样作废。
  QVERIFY(fx.runTool("paleo.map_layers", {{"page_size", 2}}, &ok, &payload));
  QVERIFY(ok);
  const QString cursor2 = payload.value("next_cursor").toString();
  QVERIFY(fx.runTool("paleo.map_layers",
                     {{"page_size", 2}, {"cursor", cursor2}, {"horizon", "T1"}}, &ok, &payload));
  QVERIFY(!ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("cursor_expired"));
}

void TestI3MapDisplayAtoms::getLayerStateAndOptions() {
  DisplayFixture fx;
  const QJsonObject detail = fx.get(fx.pol);
  QCOMPARE(detail.value("code").toString(), QStringLiteral("ok"));
  QVERIFY(detail.value("materialized").toBool());
  QVERIFY(!detail.value("state_token").toString().isEmpty());
  QVERIFY(detail.value("checked").toBool()); // 新实例化层默认勾选
  QVERIFY(detail.value("effective_visible").toBool());
  QCOMPARE(detail.value("opacity").toDouble(), 1.0);
  QVERIFY(detail.value("presets").toArray().size() >= 1); // 默认样式存在
  // 几何适配：面层给 lithology+facies，线层给 line，点/栅格不给。
  QStringList families;
  for (const auto &value : detail.value("semantic_families").toArray())
    families.append(value.toObject().value("family").toString());
  QCOMPARE(families, (QStringList{QStringLiteral("lithology"), QStringLiteral("facies")}));
  families.clear();
  for (const auto &value : fx.get(fx.lns).value("semantic_families").toArray())
    families.append(value.toObject().value("family").toString());
  QCOMPARE(families, QStringList{QStringLiteral("line")});
  QCOMPARE(fx.get(fx.ras).value("semantic_families").toArray().size(), 0);
  QCOMPARE(fx.get(fx.pts).value("semantic_families").toArray().size(), 0);
  // 未知层与手工身份伪造。
  bool ok = false;
  QJsonObject payload;
  QVERIFY(fx.runTool("paleo.map_layer_get", {{"layer_id", "nope"}}, &ok, &payload));
  QVERIFY(!ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("layer_not_declared"));
}

void TestI3MapDisplayAtoms::setVisibilityFlow() {
  DisplayFixture fx;
  QgsProject::instance()->setDirty(false);
  const QString token = fx.tokenOf(fx.pts);
  bool ok = false;
  QJsonObject payload;
  QVERIFY(fx.runTool("paleo.map_layer_set_visibility",
                     {{"layer_id", fx.pts}, {"visible", false}, {"state_token", token}}, &ok,
                     &payload));
  QVERIFY(ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("applied"));
  QVERIFY(payload.value("mutated").toBool());
  QCOMPARE(payload.value("persistence").toString(), QStringLiteral("unsaved_project_display"));
  // 原生树节点事实。
  auto *node = fx.treeNode(fx.layers.layer(fx.pts));
  QVERIFY(node);
  QVERIFY(!node->itemVisibilityChecked());
  QVERIFY(!node->isVisible());
  QVERIFY(QgsProject::instance()->isDirty()); // 工程脏标记如实前进
  // 新 token 的同值再设 = no_change（零刷新零脏标记推进）。
  QgsProject::instance()->setDirty(false);
  const QString token2 = payload.value("state_token").toString();
  QVERIFY(!token2.isEmpty());
  QVERIFY(fx.runTool("paleo.map_layer_set_visibility",
                     {{"layer_id", fx.pts}, {"visible", false}, {"state_token", token2}}, &ok,
                     &payload));
  QVERIFY(ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("no_change"));
  QVERIFY(!payload.value("mutated").toBool());
  QVERIFY(!QgsProject::instance()->isDirty());
  // 陈旧标记（用户在 get 与 set 之间改了显示）→ 零副作用拒绝。
  node->setItemVisibilityChecked(true);
  QVERIFY(fx.runTool("paleo.map_layer_set_visibility",
                     {{"layer_id", fx.pts}, {"visible", false}, {"state_token", token2}}, &ok,
                     &payload));
  QVERIFY(!ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("state_token_stale"));
  QVERIFY(node->itemVisibilityChecked()); // 未被改动
}

void TestI3MapDisplayAtoms::setVisibilityParentGroupHidden() {
  DisplayFixture fx;
  auto *root = QgsProject::instance()->layerTreeRoot();
  auto *node = fx.treeNode(fx.layers.layer(fx.pol));
  auto *group = root->insertGroup(0, QStringLiteral("HiddenGroup"));
  // 本 QGIS 版本无 takeChildNode；同 layertreepanel 的挪节点口径：插入
  // 副本再摘原节点（removeChildNode 会删除原节点）。
  group->addChildNode(node->clone());
  root->removeChildNode(node);
  node = fx.treeNode(fx.layers.layer(fx.pol));
  group->setItemVisibilityChecked(false); // 父组隐藏

  const QJsonObject detail = fx.get(fx.pol);
  QVERIFY(detail.value("checked").toBool());
  QVERIFY(!detail.value("effective_visible").toBool());
  QVERIFY(detail.value("visibility_note").toString().startsWith(QStringLiteral("parent_group_hidden")));
  // 隐藏该层（checked true→false，applied）；再经新 token 打开：applied
  // 但 effective 仍 false——不代为打开父组。
  bool ok = false;
  QJsonObject payload;
  QVERIFY(fx.runTool("paleo.map_layer_set_visibility",
                     {{"layer_id", fx.pol}, {"visible", false},
                      {"state_token", detail.value("state_token").toString()}},
                     &ok, &payload));
  QVERIFY(ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("applied"));
  QVERIFY(!payload.value("checked").toBool());
  QVERIFY(!payload.value("effective_visible").toBool());
  QVERIFY(fx.runTool("paleo.map_layer_set_visibility",
                     {{"layer_id", fx.pol}, {"visible", true},
                      {"state_token", payload.value("state_token").toString()}},
                     &ok, &payload));
  QVERIFY(ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("applied"));
  QVERIFY(payload.value("checked").toBool());
  QVERIFY(!payload.value("effective_visible").toBool());
  QVERIFY(payload.value("visibility_note").toString().contains(QStringLiteral("HiddenGroup")));
  QVERIFY(!group->itemVisibilityChecked()); // 父组保持隐藏
  QCOMPARE(group->findLayers().size(), 1);  // 组未被展开/重排
}

void TestI3MapDisplayAtoms::setOpacityVectorAndRaster() {
  DisplayFixture fx;
  auto *vector = fx.vectorLayer(fx.pol);
  auto *raster = fx.rasterLayer(fx.ras);
  const int featureCount = vector->featureCount();
  const QgsRectangle extent = vector->extent();
  bool ok = false;
  QJsonObject payload;
  // 矢量 0.45：原生值精确等于请求。
  QVERIFY(fx.runTool("paleo.map_layer_set_opacity",
                     {{"layer_id", fx.pol}, {"opacity", 0.45},
                      {"state_token", fx.tokenOf(fx.pol)}}, &ok, &payload));
  QVERIFY(ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("applied"));
  QCOMPARE(vector->opacity(), 0.45);
  QCOMPARE(payload.value("opacity").toDouble(), 0.45);
  // 栅格 0.7（各层各自取标记——token 绑定单层显示状态）。
  QVERIFY(fx.runTool("paleo.map_layer_set_opacity",
                     {{"layer_id", fx.ras}, {"opacity", 0.7},
                      {"state_token", fx.tokenOf(fx.ras)}}, &ok, &payload));
  QVERIFY(ok);
  QCOMPARE(raster->opacity(), 0.7);
  // 源数据/范围/选择零变化。
  QCOMPARE(vector->featureCount(), featureCount);
  QCOMPARE(vector->extent(), extent);
  QVERIFY(vector->selectedFeatureCount() == 0);
  // no_change（token 链）：零脏标记推进（与 visibility 同口径钉住）。
  QgsProject::instance()->setDirty(false);
  QVERIFY(fx.runTool("paleo.map_layer_set_opacity",
                     {{"layer_id", fx.ras}, {"opacity", 0.7},
                      {"state_token", payload.value("state_token").toString()}}, &ok, &payload));
  QVERIFY(ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("no_change"));
  QVERIFY(!QgsProject::instance()->isDirty());
}

void TestI3MapDisplayAtoms::setOpacityUserModifyDuringApply() {
  DisplayFixture fx;
  auto *vector = fx.vectorLayer(fx.lns);
  QMetaObject::Connection concurrent;
  concurrent = QObject::connect(vector, &QgsMapLayer::opacityChanged, vector,
                                [&concurrent, vector](double) {
                                  QObject::disconnect(concurrent);
                                  vector->setOpacity(0.9); // 同步回调中的用户并发修改
                                });
  bool ok = false;
  QJsonObject payload;
  QVERIFY(fx.runTool("paleo.map_layer_set_opacity",
                     {{"layer_id", fx.lns}, {"opacity", 0.45},
                      {"state_token", fx.tokenOf(fx.lns)}}, &ok, &payload));
  QVERIFY(ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("applied"));
  QCOMPARE(vector->opacity(), 0.9);            // 用户终值未被回滚/覆盖
  QCOMPARE(payload.value("opacity").toDouble(), 0.9); // 回执如实回读
  QVERIFY(!payload.value("note").toString().isEmpty()); // 与请求不同的显式说明
}

void TestI3MapDisplayAtoms::applyNamedPreset() {
  DisplayFixture fx;
  auto *sm = fx.vectorLayer(fx.pol)->styleManager();
  QVERIFY(sm->addStyleFromLayer(QStringLiteral("bold")));
  QVERIFY(sm->addStyleFromLayer(QStringLiteral("flat")));
  const QJsonObject detail = fx.get(fx.pol);
  QStringList presets;
  for (const auto &value : detail.value("presets").toArray())
    presets.append(value.toString());
  QVERIFY(presets.contains(QStringLiteral("bold")));
  QVERIFY(presets.contains(QStringLiteral("flat")));
  bool ok = false;
  QJsonObject payload;
  QVERIFY(fx.runTool("paleo.map_layer_apply_style",
                     {{"layer_id", fx.pol}, {"named_preset", "bold"},
                      {"state_token", detail.value("state_token").toString()}}, &ok, &payload));
  QVERIFY(ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("applied"));
  QCOMPARE(payload.value("applied_kind").toString(), QStringLiteral("named_preset"));
  QCOMPARE(sm->currentStyle(), QStringLiteral("bold"));
  QCOMPARE(payload.value("current_style").toString(), QStringLiteral("bold"));
  // 再应用同预设 = no_change。
  QVERIFY(fx.runTool("paleo.map_layer_apply_style",
                     {{"layer_id", fx.pol}, {"named_preset", "bold"},
                      {"state_token", payload.value("state_token").toString()}}, &ok, &payload));
  QVERIFY(ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("no_change"));
  // 预设被删 → 明确拒绝（removeStyle 会切换 currentStyle，须用新标记）。
  sm->removeStyle(QStringLiteral("bold"));
  QVERIFY(fx.runTool("paleo.map_layer_apply_style",
                     {{"layer_id", fx.pol}, {"named_preset", "bold"},
                      {"state_token", fx.tokenOf(fx.pol)}}, &ok, &payload));
  QVERIFY(!ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("preset_not_found"));
  // 同名内容变化：删后重存同名 flat（addStyleFromLayer 拒绝重复名，先删
  // 再存 = 同名新内容），按名应用即取新内容（名字是选项键，不是内容快照）。
  sm->removeStyle(QStringLiteral("flat"));
  QVERIFY(sm->addStyleFromLayer(QStringLiteral("flat")));
  QVERIFY(fx.runTool("paleo.map_layer_apply_style",
                     {{"layer_id", fx.pol}, {"named_preset", "flat"},
                      {"state_token", fx.tokenOf(fx.pol)}}, &ok, &payload));
  QVERIFY(ok);
  QCOMPARE(sm->currentStyle(), QStringLiteral("flat"));
}

void TestI3MapDisplayAtoms::applySemanticSymbol() {
  DisplayFixture fx;
  // 词表真实 id（从 GeoPatterns 单一权威取，不复制清单）。
  QString lithId, lineId;
  for (const QVariant &entry : GeoPatterns::lithologyDefinitions())
    if (lithId.isEmpty())
      lithId = entry.toMap().value(QStringLiteral("id")).toString();
  for (const QVariant &entry : GeoPatterns::lineStyleDefinitions())
    if (lineId.isEmpty())
      lineId = entry.toMap().value(QStringLiteral("id")).toString();
  QVERIFY(!lithId.isEmpty());
  QVERIFY(!lineId.isEmpty());
  bool ok = false;
  QJsonObject payload;
  const auto apply = [&](const QString &layerId, const QString &family, const QString &id) {
    const QJsonObject detail = fx.get(layerId);
    return fx.runTool("paleo.map_layer_apply_style",
                      {{"layer_id", layerId},
                       {"semantic_symbol", QJsonObject{{"family", family}, {"id", id}}},
                       {"state_token", detail.value("state_token").toString()}},
                      &ok, &payload);
  };
  // 面层 + 岩性：真实成功路径。
  QVERIFY(apply(fx.pol, "lithology", lithId));
  QVERIFY(ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("applied"));
  QCOMPARE(payload.value("applied_kind").toString(), QStringLiteral("semantic_symbol"));
  const auto semantics = payload.value("symbol_semantics").toObject();
  QCOMPARE(semantics.value("family").toString(), QStringLiteral("lithology"));
  QCOMPARE(semantics.value("id").toString(), lithId);
  QCOMPARE(semantics.value("version").toInt(), QgisStyleService::symbolTableVersion());
  QCOMPARE(fx.vectorLayer(fx.pol)->renderer()->type(), QStringLiteral("singleSymbol"));
  // 再应用 = no_change。
  QVERIFY(fx.runTool("paleo.map_layer_apply_style",
                     {{"layer_id", fx.pol},
                      {"semantic_symbol", QJsonObject{{"family", "lithology"}, {"id", lithId}}},
                      {"state_token", payload.value("state_token").toString()}}, &ok, &payload));
  QVERIFY(ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("no_change"));
  // 线层 + 线型：真实成功路径。
  QVERIFY(apply(fx.lns, "line", lineId));
  QVERIFY(ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("applied"));
  QCOMPARE(fx.vectorLayer(fx.lns)->renderer()->type(), QStringLiteral("singleSymbol"));
  // 线型 id 用在面层：几何不匹配 → 零副作用。
  const QJsonObject before = fx.get(fx.pol);
  QVERIFY(apply(fx.pol, "line", lineId));
  QVERIFY(!ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("semantic_symbol_inapplicable"));
  const QJsonObject after = fx.get(fx.pol);
  QCOMPARE(after.value("symbol_semantics"), before.value("symbol_semantics"));
  // 栅格层：语义符号不支持。
  QVERIFY(apply(fx.ras, "lithology", lithId));
  QVERIFY(!ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("semantic_symbol_unsupported_geometry"));
  // 未知 id / 家族错配。
  QVERIFY(apply(fx.pol, "lithology", QStringLiteral("no-such-id")));
  QVERIFY(!ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("semantic_symbol_unknown"));
}

void TestI3MapDisplayAtoms::readOnlyProjectRejectsMutationsBeforeSetters() {
  DisplayFixture fx(true); // 宿主只读
  auto *node = fx.treeNode(fx.layers.layer(fx.pts));
  QVERIFY(node->itemVisibilityChecked());
  QgsProject::instance()->setDirty(false);
  bool ok = false;
  QJsonObject payload;
  // 查询可读。
  QVERIFY(fx.runTool("paleo.map_layers", {}, &ok, &payload));
  QVERIFY(ok);
  QVERIFY(fx.runTool("paleo.map_layer_get", {{"layer_id", fx.pts}}, &ok, &payload));
  QVERIFY(ok);
  // 修改在 QGIS setters 前被拒（原生状态零变化）。
  QVERIFY(fx.runTool("paleo.map_layer_set_visibility",
                     {{"layer_id", fx.pts}, {"visible", false},
                      {"state_token", fx.tokenOf(fx.pts)}}, &ok, &payload));
  QVERIFY(!ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("project_read_only"));
  QVERIFY(fx.runTool("paleo.map_layer_set_opacity",
                     {{"layer_id", fx.pts}, {"opacity", 0.5},
                      {"state_token", fx.tokenOf(fx.pts)}}, &ok, &payload));
  QVERIFY(!ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("project_read_only"));
  QVERIFY(node->itemVisibilityChecked());
  QCOMPARE(fx.layers.layer(fx.pts)->opacity(), 1.0);
  QVERIFY(!QgsProject::instance()->isDirty());
  // readiness 不广播修改工具（查询照常广播）。
  const auto mutationReadiness = fx.runner.readiness("paleo.map_layer_set_visibility", {});
  QVERIFY(!mutationReadiness.ready);
  QCOMPARE(mutationReadiness.code, QStringLiteral("project_read_only"));
  const auto queryReadiness = fx.runner.readiness("paleo.map_layers", {});
  QVERIFY(queryReadiness.ready);
}

void TestI3MapDisplayAtoms::hostAndProviderGates() {
  {
    DisplayFixture fx;
    // 伪 documentId：宿主 live 与注入身份不符 → stale_session。
    AiChatToolContext context;
    context.capabilityHost = &fx.host;
    context.requireCapabilityHost = true;
    context.documentId = QStringLiteral("forged");
    context.documentEpoch = 1;
    context.projectDir = fx.root.path();
    fx.runner.setContext(context);
    bool ok = true;
    QJsonObject payload;
    QVERIFY(fx.runTool("paleo.map_layers", {}, &ok, &payload));
    QVERIFY(!ok);
    QCOMPARE(payload.value("code").toString(), QStringLiteral("stale_session"));
  }
  {
    DisplayFixture fx;
    // owner 与 access 成对供给：无 owner 的供给按未安装处理（review R1）。
    fx.runner.setMapDisplay(&fx.adapter, nullptr);
    QVERIFY(!fx.runner.readiness("paleo.map_layers", {}).ready);
    QCOMPARE(fx.runner.readiness("paleo.map_layers", {}).code,
             QStringLiteral("map_display_provider_unavailable"));
    fx.runner.setMapDisplay(nullptr, nullptr); // provider 撤除
    QVERIFY(!fx.runner.readiness("paleo.map_layers", {}).ready);
    QCOMPARE(fx.runner.readiness("paleo.map_layers", {}).code,
             QStringLiteral("map_display_provider_unavailable"));
    bool ok = true;
    QJsonObject payload;
    QVERIFY(fx.runTool("paleo.map_layers", {}, &ok, &payload));
    QVERIFY(!ok);
    QCOMPARE(payload.value("code").toString(), QStringLiteral("map_display_provider_unavailable"));
  }
  {
    DisplayFixture fx;
    AiChatToolContext context; // 无宿主
    context.requireCapabilityHost = true;
    fx.runner.setContext(context);
    QVERIFY(!fx.runner.readiness("paleo.map_layers", {}).ready);
    QCOMPARE(fx.runner.readiness("paleo.map_layers", {}).code, QStringLiteral("host_unavailable"));
    const auto tools = fx.runner.availableTools();
    for (const AiToolSpec &spec : tools)
      QVERIFY(!isMapDisplayTool(spec.name)); // 不向模型广播必然失败的调用
  }
}

void TestI3MapDisplayAtoms::editingLayerPolicy() {
  DisplayFixture fx;
  auto *vector = fx.vectorLayer(fx.pol);
  QVERIFY(vector->startEditing());
  bool ok = true;
  QJsonObject payload;
  // 样式/透明度：编辑中明确拒绝。
  QVERIFY(fx.runTool("paleo.map_layer_apply_style",
                     {{"layer_id", fx.pol}, {"named_preset", vector->styleManager()->styles().first()},
                      {"state_token", fx.tokenOf(fx.pol)}}, &ok, &payload));
  QVERIFY(!ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("layer_editing"));
  QVERIFY(fx.runTool("paleo.map_layer_set_opacity",
                     {{"layer_id", fx.pol}, {"opacity", 0.5},
                      {"state_token", fx.tokenOf(fx.pol)}}, &ok, &payload));
  QVERIFY(!ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("layer_editing"));
  QCOMPARE(vector->opacity(), 1.0);
  // 可见性是纯呈现：编辑中允许（树勾选不动编辑缓冲）。
  QVERIFY(fx.runTool("paleo.map_layer_set_visibility",
                     {{"layer_id", fx.pol}, {"visible", false},
                      {"state_token", fx.tokenOf(fx.pol)}}, &ok, &payload));
  QVERIFY(ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("applied"));
  QVERIFY(!fx.treeNode(vector)->itemVisibilityChecked());
  QVERIFY(vector->rollBack());
  QVERIFY(fx.get(fx.pol).value("editing").toBool() == false);
}

void TestI3MapDisplayAtoms::replayAndIdempotency() {
  DisplayFixture fx;
  const QString token = fx.tokenOf(fx.pts);
  const qint64 revision = fx.catalog.catalogRevision();
  bool ok = false;
  QJsonObject payload;
  QVERIFY(fx.runTool("paleo.map_layer_set_visibility",
                     {{"layer_id", fx.pts}, {"visible", false}, {"state_token", token}}, &ok,
                     &payload));
  QVERIFY(ok);
  const int layerCount = QgsProject::instance()->mapLayers().size();
  const int presetCount = fx.vectorLayer(fx.pol)->styleManager()->styles().size();
  // 历史回放（旧标记）：零 GUI mutation、零新层、零新预设。
  QVERIFY(fx.runTool("paleo.map_layer_set_visibility",
                     {{"layer_id", fx.pts}, {"visible", true}, {"state_token", token}}, &ok,
                     &payload));
  QVERIFY(!ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("state_token_stale"));
  QVERIFY(!fx.treeNode(fx.layers.layer(fx.pts))->itemVisibilityChecked());
  QCOMPARE(QgsProject::instance()->mapLayers().size(), layerCount);
  QCOMPARE(fx.vectorLayer(fx.pol)->styleManager()->styles().size(), presetCount);
  QCOMPARE(fx.catalog.catalogRevision(), revision); // 显示改动不落 Catalog
  // 同 ID 换实例（层位释放后重实例化）：旧标记不授予操作资格。
  const QString staleToken = fx.tokenOf(fx.pts);
  fx.layers.releaseHorizon("T1");
  QVERIFY(fx.runTool("paleo.map_layer_set_visibility",
                     {{"layer_id", fx.pts}, {"visible", true}, {"state_token", staleToken}}, &ok,
                     &payload));
  QVERIFY(!ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("layer_not_materialized"));
}

void TestI3MapDisplayAtoms::runnerContractAndI4Isolation() {
  DisplayFixture fx;
  QSignalSpy started(&fx.runner, &AiChatToolRunner::toolStarted);
  QSignalSpy finished(&fx.runner, &AiChatToolRunner::toolFinished);
  bool ok = false;
  QJsonObject payload;
  QVERIFY(fx.runTool("paleo.map_layer_get", {{"layer_id", fx.pts}}, &ok, &payload));
  QVERIFY(ok);
  QCOMPARE(payload.value("contract").toString(), QStringLiteral("paleo.map-display.v1"));
  QCOMPARE(started.size(), 1);
  QCOMPARE(finished.size(), 1);
  // I4 名称不被本族吞掉：未登记错误（诚实失败）。
  QVERIFY(fx.runTool("paleo.map_locate", {{"well_id", "w1"}}, &ok, &payload));
  QVERIFY(!ok);
  QVERIFY(payload.contains(QStringLiteral("error")));
  // native 广播含五工具（有宿主有 provider 有工程）。
  const auto tools = fx.runner.availableTools();
  int mapTools = 0;
  for (const AiToolSpec &spec : tools)
    if (isMapDisplayTool(spec.name))
      ++mapTools;
  QCOMPARE(mapTools, 5);
}

void TestI3MapDisplayAtoms::strictArgumentRejections() {
  DisplayFixture fx;
  bool ok = true;
  QJsonObject payload;
  // 布尔必须是 JSON 原生（"false" 文本拒绝）。
  QVERIFY(fx.runTool("paleo.map_layer_set_visibility",
                     {{"layer_id", fx.pts}, {"visible", "false"},
                      {"state_token", fx.tokenOf(fx.pts)}}, &ok, &payload));
  QVERIFY(!ok);
  // 数值域：opacity 1.5 越界（严格外部档）。
  QVERIFY(fx.runTool("paleo.map_layer_set_opacity",
                     {{"layer_id", fx.pts}, {"opacity", 1.5},
                      {"state_token", fx.tokenOf(fx.pts)}}, &ok, &payload));
  QVERIFY(!ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("argument_out_of_range"));
  // 文本数值（"0.5"）拒绝。
  QVERIFY(fx.runTool("paleo.map_layer_set_opacity",
                     {{"layer_id", fx.pts}, {"opacity", "0.5"},
                      {"state_token", fx.tokenOf(fx.pts)}}, &ok, &payload));
  QVERIFY(!ok);
  // 未知字段拒绝。
  QVERIFY(fx.runTool("paleo.map_layers", {{"source_path", "/etc/passwd"}}, &ok, &payload));
  QVERIFY(!ok);
  // 互斥：named_preset 与 semantic_symbol 同时给。
  QVERIFY(fx.runTool("paleo.map_layer_apply_style",
                     {{"layer_id", fx.pol}, {"named_preset", "x"},
                      {"semantic_symbol", QJsonObject{{"family", "line"}, {"id", "y"}}},
                      {"state_token", fx.tokenOf(fx.pol)}}, &ok, &payload));
  QVERIFY(!ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("style_target_conflict"));
  QCOMPARE(payload.value("field").toString(), QStringLiteral("named_preset"));
  // 都不给。
  QVERIFY(fx.runTool("paleo.map_layer_apply_style", {{"layer_id", fx.pol},
                                                     {"state_token", fx.tokenOf(fx.pol)}},
                     &ok, &payload));
  QVERIFY(!ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("style_target_required"));
  // semantic_symbol 形状。
  QVERIFY(fx.runTool("paleo.map_layer_apply_style",
                     {{"layer_id", fx.pol}, {"semantic_symbol", QJsonObject{{"family", "nope"},
                                                                            {"id", "x"}}},
                      {"state_token", fx.tokenOf(fx.pol)}}, &ok, &payload));
  QVERIFY(!ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("invalid_semantic_family"));
  // 嵌套未知键拒绝（review R3：嵌套载荷同样是模型输入）。
  QVERIFY(fx.runTool("paleo.map_layer_apply_style",
                     {{"layer_id", fx.pol},
                      {"semantic_symbol", QJsonObject{{"family", "line"}, {"id", "x"},
                                                      {"named_preset", "evil"}}},
                      {"state_token", fx.tokenOf(fx.pol)}}, &ok, &payload));
  QVERIFY(!ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("unsupported_symbol_field"));
  // 标识字段长度界。
  QVERIFY(fx.runTool("paleo.map_layer_get", {{"layer_id", QString(201, 'x')}}, &ok, &payload));
  QVERIFY(!ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("invalid_layer_id"));
  QVERIFY(fx.runTool("paleo.map_layers", {{"horizon", QString(201, 'h')}}, &ok, &payload));
  QVERIFY(!ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("invalid_horizon"));
  QVERIFY(fx.runTool("paleo.map_layer_set_visibility",
                     {{"layer_id", fx.pts}, {"visible", false},
                      {"state_token", QString(65, 't')}}, &ok, &payload));
  QVERIFY(!ok);
  QCOMPARE(payload.value("code").toString(), QStringLiteral("invalid_state_token"));
}

// ---- 有窗口 GUI 证据（手动执行）------------------------------------------
// PALEO_I3_GUI_EVIDENCE_DIR=/tmp/i3-evidence ./tst_i3_map_display_atoms
// 打开真实 QgsMapCanvas + LayerTreePanel + 属性业务页，经真实 runner 驱动
// 查询→隐藏→透明度→预设/语义样式→只读拒绝，逐步截图；offscreen 车道
// 不触发（CI 零窗口，不冒充 GUI 观察）。
void guiEvidenceScenario();

void guiEvidenceScenario() {
  const QString dir = QString::fromLatin1(qgetenv("PALEO_I3_GUI_EVIDENCE_DIR"));
  QDir().mkpath(dir);
  DisplayFixture fx;
  // 真实画布 + 原生桥接 + 图层树面板 + 属性业务页（属性壳的 Paleo 页）。
  QgsMapCanvas canvas;
  canvas.setProject(QgsProject::instance());
  new QgsLayerTreeMapCanvasBridge(QgsProject::instance()->layerTreeRoot(), &canvas, &canvas);
  canvas.setLayers({fx.layers.layer(fx.pol), fx.layers.layer(fx.ras), fx.layers.layer(fx.lns),
                    fx.layers.layer(fx.pts)});
  canvas.setExtent(fx.vectorLayer(fx.pol)->extent());
  canvas.enableAntiAliasing(true);

  QWidget window;
  window.setWindowTitle(QStringLiteral("I3 map display atoms — GUI evidence"));
  auto *columns = new QHBoxLayout(&window);
  LayerTreePanel panel(QgsProject::instance(), &canvas, &fx.layers);
  columns->addWidget(&panel, 1);
  auto *right = new QVBoxLayout();
  columns->addLayout(right, 2);
  right->addWidget(&canvas, 1);
  auto *status = new QLabel(QStringLiteral("ready"));
  status->setWordWrap(true);
  status->setMinimumHeight(120);
  right->addWidget(status);
  LayerPropertiesDialog properties(&fx.layers, {&canvas});
  if (QWidget *page = properties.createBusinessPage(fx.pol, &window)) {
    page->setParent(&window);
    right->addWidget(page);
  }
  window.resize(1280, 860);
  window.show();
  const auto settle = [] {
    for (int i = 0; i < 50; ++i)
      QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  };
  const auto shoot = [&](const char *name) {
    settle();
    window.grab().save(dir + QLatin1Char('/') + QLatin1String(name) + QStringLiteral(".png"));
  };
  const auto drive = [&](const QString &tool, const QJsonObject &args) {
    bool ok = false;
    QJsonObject payload;
    if (!fx.runTool(tool, args, &ok, &payload))
      return QJsonObject{{"code", "no_terminal_frame"}};
    status->setText(QStringLiteral("%1 → %2").arg(
        tool, QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact))));
    return payload;
  };

  shoot("01_initial_map_and_tree");
  // 查询：目录与单层状态。
  drive("paleo.map_layers", {});
  shoot("02_map_layers");
  const QJsonObject detail = drive("paleo.map_layer_get", {{"layer_id", fx.pol}});
  shoot("03_layer_get");

  // 隐藏线层：树勾选消失（不触父组）。
  drive("paleo.map_layer_set_visibility",
        {{"layer_id", fx.lns}, {"visible", false},
         {"state_token", fx.tokenOf(fx.lns)}});
  shoot("04_hide_lines");

  // 栅格透明度 0.45：画面可观察变淡。
  const QJsonObject opacity = drive(
      "paleo.map_layer_set_opacity",
      {{"layer_id", fx.ras}, {"opacity", 0.45}, {"state_token", fx.tokenOf(fx.ras)}});
  shoot("05_raster_opacity");

  // 具名预设 + 语义符号（面层岩性花纹）。
  fx.vectorLayer(fx.pol)->styleManager()->addStyleFromLayer(QStringLiteral("evidence"));
  drive("paleo.map_layer_apply_style",
        {{"layer_id", fx.pol}, {"named_preset", "evidence"},
         {"state_token", opacity.value("state_token").toString()}});
  shoot("06_named_preset");
  QString lithId;
  for (const QVariant &entry : GeoPatterns::lithologyDefinitions())
    if (lithId.isEmpty())
      lithId = entry.toMap().value(QStringLiteral("id")).toString();
  drive("paleo.map_layer_apply_style",
        {{"layer_id", fx.pol},
         {"semantic_symbol", QJsonObject{{"family", "lithology"}, {"id", lithId}}},
         {"state_token", fx.tokenOf(fx.pol)}});
  shoot("07_semantic_symbol");

  // 只读拒绝演示：伪造宿主身份 → stale_session 的诚实失败（零 GUI 变化）。
  AiChatToolContext bad;
  bad.capabilityHost = &fx.host;
  bad.requireCapabilityHost = true;
  bad.documentId = QStringLiteral("forged-document");
  bad.documentEpoch = 1;
  bad.projectDir = fx.root.path();
  fx.runner.setContext(bad);
  fx.runner.setMapDisplay(&fx.adapter, &fx.adapter);
  drive("paleo.map_layer_set_visibility",
        {{"layer_id", fx.pts}, {"visible", false}, {"state_token", fx.tokenOf(fx.pts)}});
  shoot("08_rejected_stale_session");
  qInfo("I3 GUI evidence written to %s", qPrintable(dir));
  Q_UNUSED(detail);
  QgsProject::instance()->removeAllMapLayers();
}

int main(int argc, char *argv[]) {
  QTemporaryDir profile;
  qputenv("QGIS_CUSTOM_CONFIGPATH", profile.path().toUtf8());
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(QString::fromLatin1(qgetenv("QGIS_PREFIX_PATH")), true);
  app.initQgis();
  TestI3MapDisplayAtoms tc;
  const int result = QTest::qExec(&tc, argc, argv);
  // guiEvidenceScenario 挂在类外（需要窗口）：仅在显式 evidence 目录时执行。
  if (result == 0 && qEnvironmentVariableIsSet("PALEO_I3_GUI_EVIDENCE_DIR"))
    guiEvidenceScenario();
  app.exitQgis();
  return result;
}

#include "tst_i3_map_display_atoms.moc"
