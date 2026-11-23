// 层：测试壳
// tst_i4_navigation_atoms — I4：基于业务身份的井、地震与地图导航原子。
//
// 覆盖任务书 §5.6 验收矩阵的可自动化行：严格 schema/契约、井选择的跨类型
// 隔离与整批拒绝、清除的 token/异域判定、严格 CRS 定位与真实范围回读、
// 固定版本井资料三事实、地震测线真实显示完成与顶替/取消、版本精确 SHA
// 闸与 per-request 只读策略（不写 Catalog）。offscreen 断言；真实可见
// 窗口验证走 PALEO_I4_GUI_EVIDENCE_DIR 手动流程（实施记录另载）。
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "helpers/previewfixture.h"
#include "../src/ai/chat/domaintools.h"
#include "../src/ai/chat/navigationtools.h"
#include "../src/catalog/datacatalog.h"
#include "../src/linkage/seismicmaplink.h"
#include "../src/linkage/selectioncontext.h"
#include "../src/linkage/wellmaplink.h"
#include "../src/qgis/agentmapnavigation.h"
#include "../src/qgis/qgiscanvascontroller.h"
#include "../src/services/paleotaskservice.h"
#include "../src/ui/agentnavigationadapter.h"
#include "../src/workflow/agentnavigation.h"

#include "../src/qgis/qgisruntime.h"
#include <qgsfeature.h>
#include <qgsgeometry.h>
#include <qgsmapcanvas.h>
#include <qgspointxy.h>
#include <qgsrectangle.h>
#include <qgsvectorlayer.h>

#include <QComboBox>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QTabWidget>

using paleo::tests::preview::makeStack;
using paleo::tests::preview::importAll;
using paleo::tests::preview::Imported;

namespace {
QgsVectorLayer *makeMemoryLayer(const QString &idField, const QStringList &ids)
{
  auto *layer = new QgsVectorLayer(
      QStringLiteral("Point?field=%1:string&field=name:string").arg(idField),
      QStringLiteral("mem"), QStringLiteral("memory"));
  layer->setProviderEncoding(QStringLiteral("UTF-8"));
  QList<QgsFeature> features;
  for (int i = 0; i < ids.size(); ++i) {
    QgsFeature feature(layer->dataProvider()->fields());
    feature.setAttribute(idField, ids.at(i));
    feature.setAttribute(QStringLiteral("name"), ids.at(i));
    feature.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(1000.0 * (i + 1), 2000.0)));
    features.append(feature);
  }
  layer->dataProvider()->addFeatures(features);
  layer->updateExtents();
  return layer;
}

struct Outcome
{
  bool delivered = false;
  bool ok = false;
  QJsonObject payload;
};

// 驱动 workflow 发起一次调用；终态经 QTRY 等待（同步完成也走事件循环——
// 与 runner 的异步回执同语义；恰一次终态断言在回执内）。
void requestNavigation(paleo::agent::navigation::AgentNavigation &navigation, const QString &name,
                       const QJsonObject &args, DataCatalog *catalog, Outcome *outcome)
{
  outcome->delivered = false;
  outcome->ok = false;
  outcome->payload = QJsonObject();
  paleo::agent::navigation::AgentNavigation::Environment env;
  env.catalog = catalog;
  env.documentId = QStringLiteral("doc-test");
  navigation.run(name, args, env, [outcome](bool ok, const QJsonObject &payload) {
    QVERIFY(!outcome->delivered); // 恰一次终态
    outcome->delivered = true;
    outcome->ok = ok;
    outcome->payload = payload;
  });
}
Outcome runNavigation(paleo::agent::navigation::AgentNavigation &navigation, const QString &name,
                      const QJsonObject &args, DataCatalog *catalog)
{
  Outcome outcome;
  requestNavigation(navigation, name, args, catalog, &outcome);
  // QTRY 宏内含 return，不能落在非 void 函数里——手动泵事件循环等待终态。
  for (int i = 0; i < 2000 && !outcome.delivered; ++i)
    QTest::qWait(10);
  return outcome;
}
} // namespace

class TestI4NavigationAtoms : public QObject
{
  Q_OBJECT
private slots:
  void initTestCase();
  void cleanupTestCase();

  void toolRegistryExposesExactSixNames();
  void strictValidationRejectsBeforeAnyAction();
  void dispatchRoutesExactNamesOnly();

  void selectWellsReplacesBusinessSelectionWithCrossDomainIsolation();
  void selectWellsUnknownTargetsRejectedWhole();
  void clearWellSelectionHonorsTokenAndForeignDomain();

  void mapLocateWellsStrictTransformAndReadback();
  void mapLocateRejectsMissingLayerAndInvalidExtent();

  void openWellViewTypedFactsAndVersionPinning();
  void openWellViewRejectsUnlinkedWellAndForeignVersion();
  void shaMismatchReadOnlyNavigationNeverWritesCatalog();
  void versionPreciseShaCacheDoesNotGrantOtherVersions();

  void seismicLineRealDisplayLifecycle();
  void seismicLineRejectsOutOfRangeAndReportsDecodeFailure();
  void seismicLineSupersededByUserChangeAndCancelledByTabClose();
  void seismicLineReopenAtDisplayedLineReportsRealFailure();
  void guiEvidenceDemo();

private:
  struct Fixture
  {
    QTemporaryDir tmp;
    std::unique_ptr<paleo::tests::preview::PreviewStack> stack;
    Imported ids;
    QStringList wellIds() const
    {
      QStringList out;
      for (const CatalogEntity &e : stack->importSvc->catalog()->entities(QStringLiteral("well")))
        out.append(e.id);
      return out;
    }
    DataCatalog *catalog() const { return stack->importSvc->catalog(); }
  };
  bool makeFixture(Fixture &fixture);
};

void TestI4NavigationAtoms::initTestCase()
{
  QVERIFY(qgetenv("QT_QPA_PLATFORM").isEmpty() ||
          qgetenv("QT_QPA_PLATFORM") == "offscreen");
}

void TestI4NavigationAtoms::cleanupTestCase() {}

bool TestI4NavigationAtoms::makeFixture(Fixture &fixture)
{
  if (!fixture.tmp.isValid())
    return false;
  fixture.stack = makeStack(fixture.tmp.filePath(QStringLiteral("proj")));
  if (!fixture.stack)
    return false;
  fixture.ids = importAll(*fixture.stack, fixture.tmp);
  return !fixture.ids.tops.isEmpty() && !fixture.ids.sgy.isEmpty() &&
         !fixture.ids.wellHead.isEmpty();
}

// ---- W1：注册表与契约 ------------------------------------------------------

void TestI4NavigationAtoms::toolRegistryExposesExactSixNames()
{
  using namespace paleo::agent::navigation;
  const QStringList expected{kStateTool, kSelectWellsTool, kClearWellSelectionTool,
                             kMapLocateTool,        kOpenWellViewTool, kOpenSeismicLineTool};
  const auto specs = builtinAiToolSpecs();
  for (const QString &name : expected) {
    const AiToolSpec spec = aiToolSpec(name);
    QVERIFY2(spec.name == name, qPrintable(name));
    QCOMPARE(spec.effects, unsigned(paleo::host::Read));
    QVERIFY(!spec.legacyAdapter);
    QCOMPARE(spec.exposure, CapabilityExposure::Callable);
    QVERIFY(spec.resultSchemaJson.contains(QStringLiteral("paleo.navigation.v1")));
    // 六工具 schema：additionalProperties=false（未知字段拒绝）。
    const auto schema = spec.toJsonSchema();
    QCOMPARE(schema.value(QStringLiteral("additionalProperties")).toBool(false), false);
  }
  // timeMs/深度坐标/折线/相机不在地震工具 schema。
  const AiToolSpec seismic = aiToolSpec(QStringLiteral("paleo.open_seismic_line"));
  for (const auto &parameter : seismic.parameters)
    QVERIFY(parameter.name != QLatin1String("time_ms") && parameter.name != QLatin1String("timeMs"));
}

void TestI4NavigationAtoms::strictValidationRejectsBeforeAnyAction()
{
  using namespace paleo::agent::navigation;
  QString field;
  // 空数组 / 非字符串元素 / 超上限。
  QVERIFY(!validateNavigationArguments(
      kSelectWellsTool, {{QStringLiteral("well_ids"), QJsonArray{}}}, &field).isEmpty());
  QVERIFY(!validateNavigationArguments(
              kSelectWellsTool,
              {{QStringLiteral("well_ids"), QJsonArray{1, 2}}}, &field).isEmpty());
  {
    QJsonArray many;
    for (int i = 0; i < kMaxWellIds + 1; ++i)
      many.append(QString::number(i));
    QVERIFY(!validateNavigationArguments(kSelectWellsTool,
                                         {{QStringLiteral("well_ids"), many}}, &field).isEmpty());
  }
  // map_locate 互斥：两者同给 / 两者皆无（只给 layer_id 是合法单目标）。
  QVERIFY(!validateNavigationArguments(
              kMapLocateTool,
              {{QStringLiteral("layer_id"), QStringLiteral("wells")},
               {QStringLiteral("well_ids"), QJsonArray{QStringLiteral("ent-1")}},
               {QStringLiteral("mode"), QStringLiteral("fit")}},
              &field).isEmpty());
  QVERIFY(!validateNavigationArguments(kMapLocateTool,
                                       {{QStringLiteral("mode"), QStringLiteral("fit")}},
                                       &field).isEmpty());
  // 地震轴枚举与线号表示（严格整数；分数拒绝）。
  QVERIFY(!validateNavigationArguments(
              kOpenSeismicLineTool,
              {{QStringLiteral("asset_id"), QStringLiteral("ast-1")},
               {QStringLiteral("version_id"), QStringLiteral("ver-1")},
               {QStringLiteral("axis"), QStringLiteral("diagonal")},
               {QStringLiteral("line_number"), 100}},
              &field).isEmpty());
  QVERIFY(!validateNavigationArguments(
              kOpenSeismicLineTool,
              {{QStringLiteral("asset_id"), QStringLiteral("ast-1")},
               {QStringLiteral("version_id"), QStringLiteral("ver-1")},
               {QStringLiteral("axis"), QStringLiteral("inline")},
               {QStringLiteral("line_number"), 100.5}},
              &field).isEmpty());
  QVERIFY(validateNavigationArguments(
              kOpenSeismicLineTool,
              {{QStringLiteral("asset_id"), QStringLiteral("ast-1")},
               {QStringLiteral("version_id"), QStringLiteral("ver-1")},
               {QStringLiteral("axis"), QStringLiteral("inline")},
               {QStringLiteral("line_number"), 100}},
              &field).isEmpty());
  // 井资料三段 ID 缺一/超长。
  QVERIFY(!validateNavigationArguments(
              kOpenWellViewTool,
              {{QStringLiteral("well_id"), QStringLiteral("ent-1")},
               {QStringLiteral("asset_id"), QStringLiteral("ast-1")}},
              &field).isEmpty());
}

void TestI4NavigationAtoms::dispatchRoutesExactNamesOnly()
{
  using namespace paleo::agent::navigation;
  // 精确六名可路由（合法空参 navigation_state）；I3 的 map_layers 与任何
  // 前缀变体不被本族吞掉。
  QCOMPARE(dispatchAiTool(kStateTool, {}).status, AiToolDispatchStatus::Routed);
  QCOMPARE(dispatchAiTool(QStringLiteral("paleo.map_layers"), {}).status,
           AiToolDispatchStatus::NotFound);
  QCOMPARE(dispatchAiTool(QStringLiteral("paleo.map_locate_v2"), {}).status,
           AiToolDispatchStatus::NotFound);
  QVERIFY(isNavigationTool(kMapLocateTool));
  QVERIFY(!isNavigationTool(QStringLiteral("paleo.map_layer_set_visibility")));
  // 参数类型不合格 = Disabled（工具名已知但调用不成）。空数组在通用宽松档
  // 通过（jsonTypeMatches 只看类型）——集合形状的严格拒绝在 runner 分支
  // （strictValidation 用例已钉）。
  const auto dispatch = dispatchAiTool(
      kSelectWellsTool, {{QStringLiteral("well_ids"), QStringLiteral("ent-1")}});
  QCOMPARE(dispatch.status, AiToolDispatchStatus::Disabled);
}

// ---- W3：井选择/清除与跨类型隔离 -------------------------------------------

void TestI4NavigationAtoms::selectWellsReplacesBusinessSelectionWithCrossDomainIsolation()
{
  Fixture fixture;
  QVERIFY(makeFixture(fixture));
  const QStringList wells = fixture.wellIds();
  QVERIFY(wells.size() >= 2);

  SelectionContext context;
  WellMapLink wellLink(nullptr, &context);
  SeismicMapLink seismicLink(nullptr, &context);
  // 井层与地震线层各带业务 ID 字段；地震线 ID 故意与第一口井同字串——
  // 隔离前这条广播会误选地震线。
  const QString collidingId = wells.first();
  auto *wellLayer = makeMemoryLayer(QStringLiteral("id"), wells);
  auto *lineLayer = makeMemoryLayer(QStringLiteral("line_id"),
                                    {collidingId, QStringLiteral("SL-99")});
  QVERIFY(wellLayer->isValid() && lineLayer->isValid());
  wellLink.setWellLayer(wellLayer, QStringLiteral("id"));
  seismicLink.setSeismicLayer(lineLayer, QStringLiteral("line_id"));

  AgentNavigationAdapter adapter(fixture.stack->preview.get(), &context, &wellLink);
  paleo::agent::navigation::AgentNavigation navigation;
  navigation.setSelectionAccess(&adapter, &adapter);
  navigation.setPreviewAccess(&adapter, &adapter);

  // 预置：地震域自己的选择（外来源——seismic_map 是本链路自身的回声源，
  // 会被方向 B 跳过；用剖面视图来源模拟真实生产者）。
  context.setSelection({collidingId}, QStringLiteral("seismic_section"));
  QCOMPARE(lineLayer->selectedFeatureCount(), 1);

  const QJsonObject args{{QStringLiteral("well_ids"),
                          QJsonArray{wells.at(0), wells.at(1), wells.at(0)}}}; // 重复 ID 去重
  const Outcome outcome = runNavigation(navigation, paleo::agent::navigation::kSelectWellsTool,
                                        args, fixture.catalog());
  QVERIFY(outcome.ok);
  QCOMPARE(outcome.payload.value(QStringLiteral("requested_count")).toInt(), 2);
  // 回读：业务总线 + 原生 QgsVectorLayer 选中集（非信号次数）。
  QCOMPARE(context.selectedIds().size(), 2);
  QVERIFY(context.selectedIds().contains(wells.at(0)));
  QVERIFY(context.selectedIds().contains(wells.at(1)));
  QCOMPARE(wellLayer->selectedFeatureCount(), 2);
  // 同字串 line_id 未被误选/清空；activeHorizon/activeUnit 不受影响。
  QCOMPARE(lineLayer->selectedFeatureCount(), 1);
  QVERIFY(context.activeHorizon().isEmpty());

  // navigation_state 聚合选择事实（token 可供清除回传）。
  const Outcome state =
      runNavigation(navigation, paleo::agent::navigation::kStateTool, {}, fixture.catalog());
  QVERIFY(state.ok);
  const QJsonObject selection = state.payload.value(QStringLiteral("selection")).toObject();
  QCOMPARE(selection.value(QStringLiteral("wells")).toArray().size(), 2);
  QVERIFY(!selection.value(QStringLiteral("token")).toString().isEmpty());

  // 清除：井域清空，地震域同字串选择仍在（旧广播路径被隔离）。
  const QJsonObject clearArgs{{QStringLiteral("selection_token"),
                               selection.value(QStringLiteral("token"))}};
  const Outcome cleared = runNavigation(
      navigation, paleo::agent::navigation::kClearWellSelectionTool, clearArgs, fixture.catalog());
  QVERIFY(cleared.ok);
  QCOMPARE(cleared.payload.value(QStringLiteral("status")).toString(),
           QStringLiteral("cleared"));
  QVERIFY(context.selectedIds().isEmpty());
  QCOMPARE(wellLayer->selectedFeatureCount(), 0);
  QCOMPARE(lineLayer->selectedFeatureCount(), 1); // 用户的其他工作未被清掉

  delete wellLayer;
  delete lineLayer;
}

void TestI4NavigationAtoms::selectWellsUnknownTargetsRejectedWhole()
{
  Fixture fixture;
  QVERIFY(makeFixture(fixture));
  const QStringList wells = fixture.wellIds();
  QVERIFY(!wells.isEmpty());

  SelectionContext context;
  context.setSelection({wells.first()}, QStringLiteral("datatree"));
  AgentNavigationAdapter adapter(fixture.stack->preview.get(), &context, nullptr);
  paleo::agent::navigation::AgentNavigation navigation;
  navigation.setSelectionAccess(&adapter, &adapter);

  const QJsonObject args{{QStringLiteral("well_ids"),
                          QJsonArray{wells.first(), QStringLiteral("ent-unknown")}}};
  const Outcome outcome = runNavigation(navigation, paleo::agent::navigation::kSelectWellsTool,
                                        args, fixture.catalog());
  QVERIFY(!outcome.ok);
  QCOMPARE(outcome.payload.value(QStringLiteral("code")).toString(),
           QStringLiteral("unknown_well_ids"));
  // 整批拒绝：不留部分选中（原选择原样）。
  QCOMPARE(context.selectedIds(), QStringList{wells.first()});
}

void TestI4NavigationAtoms::clearWellSelectionHonorsTokenAndForeignDomain()
{
  Fixture fixture;
  QVERIFY(makeFixture(fixture));
  const QStringList wells = fixture.wellIds();
  QVERIFY(!wells.isEmpty());

  SelectionContext context;
  AgentNavigationAdapter adapter(fixture.stack->preview.get(), &context, nullptr);
  paleo::agent::navigation::AgentNavigation navigation;
  navigation.setSelectionAccess(&adapter, &adapter);

  // 伪造 token（模型自造，非宿主回传）→ stale_selection，不动作。
  context.setSelection({wells.first()}, QStringLiteral("datatree"));
  const Outcome stale = runNavigation(
      navigation, paleo::agent::navigation::kClearWellSelectionTool,
      {{QStringLiteral("selection_token"), QStringLiteral("not-a-host-token")}},
      fixture.catalog());
  QVERIFY(!stale.ok);
  QCOMPARE(stale.payload.value(QStringLiteral("code")).toString(), QStringLiteral("stale_selection"));
  QCOMPARE(context.selectedIds(), QStringList{wells.first()});

  // 当前选择已切到地震域（非井目标）→ foreign_selection，不清用户工作。
  context.setSelection({QStringLiteral("SL-42")}, QStringLiteral("seismic_map"));
  const Outcome foreign = runNavigation(
      navigation, paleo::agent::navigation::kClearWellSelectionTool, {}, fixture.catalog());
  QVERIFY(!foreign.ok);
  QCOMPARE(foreign.payload.value(QStringLiteral("code")).toString(),
           QStringLiteral("foreign_selection"));
  QCOMPARE(context.selectedIds(), QStringList{QStringLiteral("SL-42")});

  // 空选择 → no_change（ok）。
  context.setSelection({}, QStringLiteral("agent_navigation"));
  const Outcome none = runNavigation(navigation,
                                     paleo::agent::navigation::kClearWellSelectionTool, {},
                                     fixture.catalog());
  QVERIFY(none.ok);
  QCOMPARE(none.payload.value(QStringLiteral("status")).toString(), QStringLiteral("no_change"));
}

// ---- W4：严格 CRS 定位 ------------------------------------------------------

void TestI4NavigationAtoms::mapLocateWellsStrictTransformAndReadback()
{
  Fixture fixture;
  QVERIFY(makeFixture(fixture));
  QStringList withCoords;
  for (const CatalogEntity &well : fixture.catalog()->entities(QStringLiteral("well")))
    if (well.hasSurface)
      withCoords.append(well.id);
  if (withCoords.isEmpty())
    QSKIP("夹具井无坐标（surfaceX/Y 缺失）——坐标拒绝路径由无坐标用例覆盖");

  QgisCanvasController canvasCtl;
  QgsMapCanvas *canvas = canvasCtl.canvas(); // 定位是显式导航动作：画布允许创建
  canvas->setDestinationCrs(
      QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt()));
  canvas->setExtent(QgsRectangle(-100000, -100000, 100000, 100000));
  AgentMapNavigation mapNavigation(&canvasCtl, fixture.stack->layerSvc.get(),
                                   &fixture.stack->projectSvc);
  paleo::agent::navigation::AgentNavigation navigation;
  navigation.setMapAccess(&mapNavigation, &mapNavigation);

  // fit：实际视野回读后包含全部目标井坐标（工程网格局部米 → 同 CRS 恒等）。
  const QJsonObject args{{QStringLiteral("well_ids"), QJsonArray::fromStringList(withCoords)},
                         {QStringLiteral("mode"), QStringLiteral("fit")}};
  const Outcome fit = runNavigation(navigation, paleo::agent::navigation::kMapLocateTool, args,
                                    fixture.catalog());
  QVERIFY2(fit.ok, qPrintable(QJsonDocument(fit.payload).toJson(QJsonDocument::Compact)));
  const auto extent = fit.payload.value(QStringLiteral("extent")).toArray();
  QCOMPARE(extent.size(), 4);
  for (const QString &id : withCoords) {
    const CatalogEntity well = fixture.catalog()->entityById(id);
    QVERIFY(extent.at(0).toDouble() <= well.surfaceX && well.surfaceX <= extent.at(2).toDouble());
    QVERIFY(extent.at(1).toDouble() <= well.surfaceY && well.surfaceY <= extent.at(3).toDouble());
  }
  QVERIFY(!fit.payload.value(QStringLiteral("crs")).toString().isEmpty());

  // center：保留当前视野宽高，中心为目标。
  const double widthBefore = canvas->extent().width();
  const double heightBefore = canvas->extent().height();
  const QJsonObject centerArgs{{QStringLiteral("well_ids"), QJsonArray{withCoords.first()}},
                               {QStringLiteral("mode"), QStringLiteral("center")}};
  const Outcome center = runNavigation(navigation, paleo::agent::navigation::kMapLocateTool,
                                       centerArgs, fixture.catalog());
  QVERIFY(center.ok);
  QVERIFY(qAbs(canvas->extent().width() - widthBefore) < 1.0);
  QVERIFY(qAbs(canvas->extent().height() - heightBefore) < 1.0);
  const CatalogEntity target = fixture.catalog()->entityById(withCoords.first());
  QVERIFY(canvas->extent().contains(QgsPointXY(target.surfaceX, target.surfaceY)));

  // 无坐标井：整批拒绝（不伪造坐标、不静默跳过）。
  CatalogEntity coordinateless;
  coordinateless.id = QStringLiteral("ent-coordinateless");
  coordinateless.entityType = QStringLiteral("well");
  coordinateless.name = QStringLiteral("NOCOORD");
  QVERIFY(fixture.catalog()->addEntity(coordinateless));
  const QJsonObject badArgs{
      {QStringLiteral("well_ids"),
       QJsonArray{QStringLiteral("ent-coordinateless")}},
      {QStringLiteral("mode"), QStringLiteral("fit")}};
  const double beforeXMin = canvas->extent().xMinimum();
  const Outcome rejected =
      runNavigation(navigation, paleo::agent::navigation::kMapLocateTool, badArgs,
                    fixture.catalog());
  QVERIFY(!rejected.ok);
  QCOMPARE(rejected.payload.value(QStringLiteral("code")).toString(),
           QStringLiteral("well_without_coordinates"));
  QCOMPARE(canvas->extent().xMinimum(), beforeXMin); // 失败不移动
}

void TestI4NavigationAtoms::mapLocateRejectsMissingLayerAndInvalidExtent()
{
  Fixture fixture;
  QVERIFY(makeFixture(fixture));
  QgisCanvasController canvasCtl;
  QgsMapCanvas *canvas = canvasCtl.canvas();
  canvas->setDestinationCrs(
      QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt()));
  canvas->setExtent(QgsRectangle(0, 0, 1000, 1000));
  AgentMapNavigation mapNavigation(&canvasCtl, fixture.stack->layerSvc.get(),
                                   &fixture.stack->projectSvc);
  paleo::agent::navigation::AgentNavigation navigation;
  navigation.setMapAccess(&mapNavigation, &mapNavigation);

  // 未实例化/未声明的受管图层：如实拒绝，不实例化非当前层位。
  const QJsonObject missing{{QStringLiteral("layer_id"), QStringLiteral("no-such-layer")},
                            {QStringLiteral("mode"), QStringLiteral("fit")}};
  const Outcome missingOutcome = runNavigation(navigation, paleo::agent::navigation::kMapLocateTool,
                                               missing, fixture.catalog());
  QVERIFY(!missingOutcome.ok);
  QCOMPARE(missingOutcome.payload.value(QStringLiteral("code")).toString(),
           QStringLiteral("layer_not_present"));

  // 已实例化但无有效范围的图层（空 geojson）：extent 不可用即拒绝。
  const QString emptyPath = fixture.tmp.filePath(QStringLiteral("empty.geojson"));
  {
    QFile emptyFile(emptyPath);
    QVERIFY(emptyFile.open(QIODevice::WriteOnly));
    QVERIFY(emptyFile.write(R"({"type":"FeatureCollection","features":[]})") > 0);
  }
  LayerDeclaration decl;
  decl.layerId = QStringLiteral("i4-empty");
  decl.type = QStringLiteral("vector");
  decl.source = emptyPath;
  decl.group = QStringLiteral("00_Data");
  decl.title = QStringLiteral("I4 empty");
  QString declareError;
  QVERIFY(fixture.stack->layerSvc->declare(decl, &declareError));
  QString instantiateError;
  QVERIFY(fixture.stack->layerSvc->instantiate(QStringLiteral("i4-empty"), &instantiateError) !=
          nullptr);
  const QJsonObject emptyArgs{{QStringLiteral("layer_id"), QStringLiteral("i4-empty")},
                              {QStringLiteral("mode"), QStringLiteral("fit")}};
  const Outcome emptyOutcome =
      runNavigation(navigation, paleo::agent::navigation::kMapLocateTool, emptyArgs,
                    fixture.catalog());
  QVERIFY(!emptyOutcome.ok);
  QCOMPARE(emptyOutcome.payload.value(QStringLiteral("code")).toString(),
           QStringLiteral("layer_extent_unavailable"));

  // 画布目标 CRS 无效：严格转换失败即拒绝（不回落原始坐标），旧视野不动。
  QStringList withCoords;
  for (const CatalogEntity &well : fixture.catalog()->entities(QStringLiteral("well")))
    if (well.hasSurface)
      withCoords.append(well.id);
  if (withCoords.isEmpty())
    QSKIP("夹具井无坐标");
  canvas->setDestinationCrs(QgsCoordinateReferenceSystem()); // invalid
  const double beforeXMin = canvas->extent().xMinimum();
  const QJsonObject badCrsArgs{{QStringLiteral("well_ids"), QJsonArray::fromStringList(withCoords)},
                               {QStringLiteral("mode"), QStringLiteral("fit")}};
  const Outcome transformFailed =
      runNavigation(navigation, paleo::agent::navigation::kMapLocateTool, badCrsArgs,
                    fixture.catalog());
  QVERIFY(!transformFailed.ok);
  QCOMPARE(transformFailed.payload.value(QStringLiteral("code")).toString(),
           QStringLiteral("coordinate_transform_failed"));
  QCOMPARE(canvas->extent().xMinimum(), beforeXMin); // 失败不移动、不伪坐标回落
}

// ---- W5/W7：固定版本井资料与只读 SHA 策略 ----------------------------------

void TestI4NavigationAtoms::openWellViewTypedFactsAndVersionPinning()
{
  Fixture fixture;
  QVERIFY(makeFixture(fixture));
  DataCatalog *catalog = fixture.catalog();
  // 井分层资产（多井文件 DC.dat）：至少一口已决井关联。
  const CatalogAsset asset = catalog->assetById(fixture.ids.tops);
  QVERIFY(!asset.id.isEmpty());
  QCOMPARE(asset.type, QStringLiteral("well_stratification"));
  QString linkedWell;
  for (const EntityAssetLink &l : catalog->linksForAsset(asset.id))
    if (l.entityType == QLatin1String("well") && !l.unresolved && !l.entityId.isEmpty()) {
      linkedWell = l.entityId;
      break;
    }
  QVERIFY(!linkedWell.isEmpty());
  const CatalogVersion v1 = catalog->currentVersion(asset.id);
  QVERIFY(!v1.id.isEmpty());

  // 第二版本：同资产登记 v2（受管副本，指向同一文件——版本隔离语义测试）。
  CatalogVersion v2 = v1;
  v2.id = QStringLiteral("ver-i4-second");
  v2.versionNumber = v1.versionNumber + 1;
  QVERIFY(catalog->addVersion(v2));

  AgentNavigationAdapter adapter(fixture.stack->preview.get(), new SelectionContext(), nullptr);
  paleo::agent::navigation::AgentNavigation navigation;
  navigation.setPreviewAccess(&adapter, &adapter);

  const QJsonObject args{{QStringLiteral("well_id"), linkedWell},
                         {QStringLiteral("asset_id"), asset.id},
                         {QStringLiteral("version_id"), v2.id}};
  const Outcome outcome = runNavigation(navigation, paleo::agent::navigation::kOpenWellViewTool,
                                        args, catalog);
  QVERIFY2(outcome.ok,
           qPrintable(QJsonDocument(outcome.payload).toJson(QJsonDocument::Compact)));
  QCOMPARE(outcome.payload.value(QStringLiteral("status")).toString(), QStringLiteral("shown"));
  QCOMPARE(outcome.payload.value(QStringLiteral("page_open")).toBool(), true);
  QCOMPARE(outcome.payload.value(QStringLiteral("page_focused")).toBool(), true);
  QCOMPARE(outcome.payload.value(QStringLiteral("version_id")).toString(), v2.id);
  QCOMPARE(outcome.payload.value(QStringLiteral("well_id")).toString(), linkedWell);

  // 旧版本页已开 → 指定另一版保真切换（actual version 跟随请求）。
  const QJsonObject v1Args{{QStringLiteral("well_id"), linkedWell},
                           {QStringLiteral("asset_id"), asset.id},
                           {QStringLiteral("version_id"), v1.id}};
  const Outcome switched = runNavigation(navigation, paleo::agent::navigation::kOpenWellViewTool,
                                         v1Args, catalog);
  QVERIFY(switched.ok);
  QCOMPARE(switched.payload.value(QStringLiteral("version_id")).toString(), v1.id);
  QCOMPARE(fixture.stack->preview->tabCount(), 1); // 同资产复用标签，不翻倍
}

void TestI4NavigationAtoms::openWellViewRejectsUnlinkedWellAndForeignVersion()
{
  Fixture fixture;
  QVERIFY(makeFixture(fixture));
  DataCatalog *catalog = fixture.catalog();
  const CatalogAsset tops = catalog->assetById(fixture.ids.tops);
  QVERIFY(!tops.id.isEmpty());
  const CatalogVersion version = catalog->currentVersion(tops.id);

  // 非本资产版本 → version_not_in_asset。
  const CatalogAsset wellHead = catalog->assetById(fixture.ids.wellHead);
  const CatalogVersion foreignVersion = catalog->currentVersion(wellHead.id);
  AgentNavigationAdapter adapter(fixture.stack->preview.get(), new SelectionContext(), nullptr);
  paleo::agent::navigation::AgentNavigation navigation;
  navigation.setPreviewAccess(&adapter, &adapter);
  const QJsonObject foreignArgs{{QStringLiteral("well_id"), QStringLiteral("ent-any")},
                                {QStringLiteral("asset_id"), tops.id},
                                {QStringLiteral("version_id"), foreignVersion.id}};
  const Outcome foreign = runNavigation(navigation, paleo::agent::navigation::kOpenWellViewTool,
                                        foreignArgs, catalog);
  QVERIFY(!foreign.ok);
  QCOMPARE(foreign.payload.value(QStringLiteral("code")).toString(),
           QStringLiteral("version_not_in_asset"));

  // 未关联井（catalog 存在但不挂该资产）→ well_not_linked_to_asset，页面不建。
  // 夹具井全已决关联——新建一口无关联井实体覆盖该分支。
  CatalogEntity unlinkedEntity;
  unlinkedEntity.id = QStringLiteral("ent-i4-unlinked");
  unlinkedEntity.entityType = QStringLiteral("well");
  unlinkedEntity.name = QStringLiteral("I4-UNLINKED");
  QVERIFY(catalog->addEntity(unlinkedEntity));
  const QString unlinkedWell = unlinkedEntity.id;
  const QJsonObject unlinkedArgs{{QStringLiteral("well_id"), unlinkedWell},
                                 {QStringLiteral("asset_id"), tops.id},
                                 {QStringLiteral("version_id"), version.id}};
  const Outcome unlinked = runNavigation(navigation, paleo::agent::navigation::kOpenWellViewTool,
                                         unlinkedArgs, catalog);
  QVERIFY(!unlinked.ok);
  QCOMPARE(unlinked.payload.value(QStringLiteral("code")).toString(),
           QStringLiteral("well_not_linked_to_asset"));
  QCOMPARE(fixture.stack->preview->tabCount(), 0);
  Q_UNUSED(version);
}

void TestI4NavigationAtoms::shaMismatchReadOnlyNavigationNeverWritesCatalog()
{
  Fixture fixture;
  QVERIFY(makeFixture(fixture));
  DataCatalog *catalog = fixture.catalog();
  // 外链版本 + 错误指纹：Agent 只读导航须失败且不写 Catalog（revision 不变）。
  const CatalogAsset tops = catalog->assetById(fixture.ids.tops);
  const CatalogVersion current = catalog->currentVersion(tops.id);
  CatalogVersion tampered = current;
  tampered.id = QStringLiteral("ver-i4-tampered");
  tampered.versionNumber = current.versionNumber + 1;
  tampered.managed = false;
  // 指向真实文件 + 错误指纹：复验必然失配（不是缺文件）。
  tampered.path = QDir(catalog->projectDir()).filePath(current.path);
  QVERIFY(QFile::exists(tampered.path));
  tampered.sha256 = QStringLiteral("0000000000000000000000000000000000000000000000000000000000000000");
  QVERIFY(catalog->addVersion(tampered));

  const int revisionBefore = catalog->catalogRevision();
  AgentNavigationAdapter adapter(fixture.stack->preview.get(), new SelectionContext(), nullptr);
  paleo::agent::navigation::AgentNavigation navigation;
  navigation.setPreviewAccess(&adapter, &adapter);
  QString linkedWell;
  for (const EntityAssetLink &l : catalog->linksForAsset(tops.id))
    if (l.entityType == QLatin1String("well") && !l.unresolved && !l.entityId.isEmpty()) {
      linkedWell = l.entityId;
      break;
    }
  QVERIFY(!linkedWell.isEmpty());
  const QJsonObject args{{QStringLiteral("well_id"), linkedWell},
                         {QStringLiteral("asset_id"), tops.id},
                         {QStringLiteral("version_id"), tampered.id}};
  const Outcome outcome = runNavigation(navigation, paleo::agent::navigation::kOpenWellViewTool,
                                        args, catalog);
  QVERIFY(!outcome.ok);
  QCOMPARE(outcome.payload.value(QStringLiteral("status")).toString(),
           QStringLiteral("sha_mismatch"));
  QCOMPARE(catalog->catalogRevision(), revisionBefore); // 只读策略：零 Catalog 写
}

void TestI4NavigationAtoms::versionPreciseShaCacheDoesNotGrantOtherVersions()
{
  Fixture fixture;
  QVERIFY(makeFixture(fixture));
  DataCatalog *catalog = fixture.catalog();
  const CatalogAsset tops = catalog->assetById(fixture.ids.tops);
  const CatalogVersion current = catalog->currentVersion(tops.id);

  // v-good：受管（闸门短路，视为已验）→ typed open 成功并写版本精确缓存。
  // v-bad：同资产、不同版本、坏指纹——asset 级缓存（旧行为）会放行；版本
  // 精确键必须拒绝。
  CatalogVersion bad = current;
  bad.id = QStringLiteral("ver-i4-badb");
  bad.versionNumber = current.versionNumber + 1;
  bad.managed = false;
  bad.path = QDir(catalog->projectDir()).filePath(current.path); // 真实文件 + 坏指纹
  QVERIFY(QFile::exists(bad.path));
  bad.sha256 = QStringLiteral("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
  QVERIFY(catalog->addVersion(bad));

  QString linkedWell;
  for (const EntityAssetLink &l : catalog->linksForAsset(tops.id))
    if (l.entityType == QLatin1String("well") && !l.unresolved && !l.entityId.isEmpty()) {
      linkedWell = l.entityId;
      break;
    }
  QVERIFY(!linkedWell.isEmpty());
  AgentNavigationAdapter adapter(fixture.stack->preview.get(), new SelectionContext(), nullptr);
  paleo::agent::navigation::AgentNavigation navigation;
  navigation.setPreviewAccess(&adapter, &adapter);

  // 先打开 v-good（受管 → 立即 shown）。
  const QJsonObject goodArgs{{QStringLiteral("well_id"), linkedWell},
                             {QStringLiteral("asset_id"), tops.id},
                             {QStringLiteral("version_id"), current.id}};
  QVERIFY(runNavigation(navigation, paleo::agent::navigation::kOpenWellViewTool, goodArgs,
                        catalog).ok);
  // 再打开 v-bad：同资产已验不放行异版本。
  const QJsonObject badArgs{{QStringLiteral("well_id"), linkedWell},
                            {QStringLiteral("asset_id"), tops.id},
                            {QStringLiteral("version_id"), bad.id}};
  const Outcome badOutcome = runNavigation(navigation, paleo::agent::navigation::kOpenWellViewTool,
                                           badArgs, catalog);
  QVERIFY(!badOutcome.ok);
  QCOMPARE(badOutcome.payload.value(QStringLiteral("status")).toString(),
           QStringLiteral("sha_mismatch"));
  // 页面仍显示 v-good（未被坏版本顶掉）。
  QCOMPARE(fixture.stack->preview->versionIdAt(
               fixture.stack->preview->tabCount() - 1),
           current.id);
}

// ---- W6/W8：地震测线真实显示与生命周期 -------------------------------------

void TestI4NavigationAtoms::seismicLineRealDisplayLifecycle()
{
  Fixture fixture;
  QVERIFY(makeFixture(fixture));
  DataCatalog *catalog = fixture.catalog();
  PaleoProjectStore taskStore;
  PaleoTaskService taskSvc(&taskStore);
  fixture.stack->preview->setTaskService(&taskSvc);

  const CatalogAsset sgy = catalog->assetById(fixture.ids.sgy);
  QVERIFY(!sgy.id.isEmpty());
  const CatalogVersion version = catalog->currentVersion(sgy.id);
  QString surveyId;
  for (const EntityAssetLink &l : catalog->linksForAsset(sgy.id))
    if (l.role == QLatin1String("seismic_volume") && !l.unresolved && !l.entityId.isEmpty())
      surveyId = l.entityId;
  const CatalogEntity survey =
      surveyId.isEmpty() ? CatalogEntity() : catalog->entityById(surveyId);
  QVERIFY2(survey.inlineMax > survey.inlineMin,
           "夹具地震资产缺少冻结 survey 几何（inline 范围）");
  const int existingInline = 1000; // 与 tst_datapreview 的解码断言同源

  AgentNavigationAdapter adapter(fixture.stack->preview.get(), new SelectionContext(), nullptr);
  paleo::agent::navigation::AgentNavigation navigation;
  navigation.setPreviewAccess(&adapter, &adapter);

  const QJsonObject args{{QStringLiteral("asset_id"), sgy.id},
                         {QStringLiteral("version_id"), version.id},
                         {QStringLiteral("axis"), QStringLiteral("inline")},
                         {QStringLiteral("line_number"), existingInline}};
  const Outcome outcome = runNavigation(navigation, paleo::agent::navigation::kOpenSeismicLineTool,
                                        args, catalog);
  QVERIFY2(outcome.ok,
           qPrintable(QJsonDocument(outcome.payload).toJson(QJsonDocument::Compact)));
  const QString status = outcome.payload.value(QStringLiteral("status")).toString();
  QVERIFY2(status == QLatin1String("shown") || status == QLatin1String("partial"),
           qPrintable(status));
  QCOMPARE(outcome.payload.value(QStringLiteral("version_id")).toString(), version.id);
  QCOMPARE(outcome.payload.value(QStringLiteral("axis")).toString(), QStringLiteral("inline"));
  QCOMPARE(outcome.payload.value(QStringLiteral("line_number")).toInt(), existingInline);
  // 页面真实显示目标线（标题后缀 = IL1000）。
  QTRY_COMPARE_WITH_TIMEOUT(fixture.stack->preview->findChild<QTabWidget *>(
                                QStringLiteral("dataPreviewTabs"))
                                ->tabText(fixture.stack->preview->tabCount() - 1),
                            QStringLiteral("vol.sgy · IL1000"), 8000);
}

void TestI4NavigationAtoms::seismicLineRejectsOutOfRangeAndReportsDecodeFailure()
{
  Fixture fixture;
  QVERIFY(makeFixture(fixture));
  DataCatalog *catalog = fixture.catalog();
  PaleoProjectStore taskStore;
  PaleoTaskService taskSvc(&taskStore);
  fixture.stack->preview->setTaskService(&taskSvc);

  const CatalogAsset sgy = catalog->assetById(fixture.ids.sgy);
  const CatalogVersion version = catalog->currentVersion(sgy.id);
  QString surveyId;
  for (const EntityAssetLink &l : catalog->linksForAsset(sgy.id))
    if (l.role == QLatin1String("seismic_volume") && !l.unresolved && !l.entityId.isEmpty())
      surveyId = l.entityId;
  const CatalogEntity survey =
      surveyId.isEmpty() ? CatalogEntity() : catalog->entityById(surveyId);
  QVERIFY2(survey.inlineMax > survey.inlineMin, "夹具缺 survey 几何");
  const int outOfRange = int(survey.inlineMax) + 7;

  AgentNavigationAdapter adapter(fixture.stack->preview.get(), new SelectionContext(), nullptr);
  paleo::agent::navigation::AgentNavigation navigation;
  navigation.setPreviewAccess(&adapter, &adapter);

  // 越界：同步拒绝（发生在任何控件写入前——不产生钳位解码）。
  const QJsonObject outArgs{{QStringLiteral("asset_id"), sgy.id},
                            {QStringLiteral("version_id"), version.id},
                            {QStringLiteral("axis"), QStringLiteral("inline")},
                            {QStringLiteral("line_number"), outOfRange}};
  const Outcome out = runNavigation(navigation, paleo::agent::navigation::kOpenSeismicLineTool,
                                    outArgs, catalog);
  QVERIFY(!out.ok);
  // 工作流层拒绝（先于 adapter）：code 即机器拒绝面（status 是 adapter 的
  // typed 词汇，此处不存在——两套词汇不混用）。
  QCOMPARE(out.payload.value(QStringLiteral("code")).toString(),
           QStringLiteral("line_out_of_survey_range"));
  // 页面未创建（拒绝先于打开）。
  QCOMPARE(fixture.stack->preview->tabCount(), 0);

  // 范围内但解码失败的线：如实 failed（此处用坏指纹外链版本制造真实失败）。
  CatalogVersion broken = version;
  broken.id = QStringLiteral("ver-i4-sgy-bad");
  broken.versionNumber = version.versionNumber + 1;
  broken.managed = false;
  broken.path = fixture.ids.sgySource; // 真实 SEG-Y + 坏指纹 → 解码 SHA 闸失败
  QVERIFY(QFile::exists(broken.path));
  broken.sha256 = QStringLiteral("1111111111111111111111111111111111111111111111111111111111111111");
  QVERIFY(catalog->addVersion(broken));
  const QJsonObject brokenArgs{{QStringLiteral("asset_id"), sgy.id},
                               {QStringLiteral("version_id"), broken.id},
                               {QStringLiteral("axis"), QStringLiteral("inline")},
                               {QStringLiteral("line_number"), 1000}};
  const Outcome failed = runNavigation(navigation, paleo::agent::navigation::kOpenSeismicLineTool,
                                       brokenArgs, catalog);
  QVERIFY(!failed.ok);
  QCOMPARE(failed.payload.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
}

void TestI4NavigationAtoms::seismicLineSupersededByUserChangeAndCancelledByTabClose()
{
  Fixture fixture;
  QVERIFY(makeFixture(fixture));
  DataCatalog *catalog = fixture.catalog();
  PaleoProjectStore taskStore;
  PaleoTaskService taskSvc(&taskStore);
  fixture.stack->preview->setTaskService(&taskSvc);

  const CatalogAsset sgy = catalog->assetById(fixture.ids.sgy);
  const CatalogVersion version = catalog->currentVersion(sgy.id);
  QString surveyId;
  for (const EntityAssetLink &l : catalog->linksForAsset(sgy.id))
    if (l.role == QLatin1String("seismic_volume") && !l.unresolved && !l.entityId.isEmpty())
      surveyId = l.entityId;
  const CatalogEntity survey =
      surveyId.isEmpty() ? CatalogEntity() : catalog->entityById(surveyId);
  QVERIFY2(survey.inlineMax > survey.inlineMin, "夹具缺 survey 几何");
  // 1001（当前显示）之外的在测线：取范围内安全值（不依赖稀疏存在性——
  // 顶替/取消路径在解码完成前即收口）。
  const int maxInline = int(survey.inlineMax);
  const int agentLine = maxInline >= 1002 ? 1002 : 1000;
  const int pendingLine = maxInline >= 1003 ? 1003 : (maxInline >= 1002 ? 1002 : 1000);
  AgentNavigationAdapter adapter(fixture.stack->preview.get(), new SelectionContext(), nullptr);
  paleo::agent::navigation::AgentNavigation navigation;
  navigation.setPreviewAccess(&adapter, &adapter);
  paleo::agent::navigation::AgentNavigation::Environment env;
  env.catalog = catalog;

  // ---- 顶替：agent 请求 1002 在途，用户同步改线 1001（同一事件栈，请求
  // 任务尚未跑完）→ 单终态 superseded，用户画面不被改回。
  {
    // 先建页并等 IL1000 上屏（确定初始态）。
    Outcome initial;
    navigation.run(paleo::agent::navigation::kOpenSeismicLineTool,
                   {{QStringLiteral("asset_id"), sgy.id},
                    {QStringLiteral("version_id"), version.id},
                    {QStringLiteral("axis"), QStringLiteral("inline")},
                    {QStringLiteral("line_number"), 1000}},
                   env, [&initial](bool ok, const QJsonObject &payload) {
                     QVERIFY(!initial.delivered);
                     initial.delivered = true;
                     initial.ok = ok;
                     initial.payload = payload;
                   });
    QTRY_VERIFY_WITH_TIMEOUT(initial.delivered, 15000);
    auto *tabs =
        fixture.stack->preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));

    Outcome superseded;
    int lateFrames = 0;
    navigation.run(paleo::agent::navigation::kOpenSeismicLineTool,
                   {{QStringLiteral("asset_id"), sgy.id},
                    {QStringLiteral("version_id"), version.id},
                    {QStringLiteral("axis"), QStringLiteral("inline")},
                    {QStringLiteral("line_number"), agentLine}},
                   env, [&superseded, &lateFrames](bool ok, const QJsonObject &payload) {
                     if (superseded.delivered) {
                       ++lateFrames; // 迟到回执：不得产生第二终态
                       return;
                     }
                     superseded.delivered = true;
                     superseded.ok = ok;
                     superseded.payload = payload;
                   });
    // typed 打开会重建既有页控件——改线动作作用于当前真实控件（不是早前
    // 捕获的旧指针）。
    QWidget *page = tabs->widget(tabs->currentIndex());
    auto *no = page->findChild<QSpinBox *>(QStringLiteral("lineSpin"));
    QVERIFY(no);
    no->setValue(1001); // 用户改线（agent 请求在途——同栈未让出事件循环）
    QTRY_VERIFY_WITH_TIMEOUT(superseded.delivered, 15000);
    QCOMPARE(superseded.payload.value(QStringLiteral("status")).toString(),
             QStringLiteral("superseded"));
    QVERIFY(!superseded.ok);
    // 用户画面保持在自己的线（IL1001），不被 agent 目标改回。
    QTRY_COMPARE_WITH_TIMEOUT(tabs->tabText(tabs->currentIndex()),
                              QStringLiteral("vol.sgy · IL1001"), 15000);
    QTest::qWait(300); // 收迟到回执窗口
    QCOMPARE(lateFrames, 0);
  }

  // ---- 关页：在途请求 → cancelled（closeAssetTab 的挂起回执收口）。
  {
    Outcome cancelled;
    navigation.run(paleo::agent::navigation::kOpenSeismicLineTool,
                   {{QStringLiteral("asset_id"), sgy.id},
                    {QStringLiteral("version_id"), version.id},
                    {QStringLiteral("axis"), QStringLiteral("inline")},
                    {QStringLiteral("line_number"), pendingLine}},
                   env, [&cancelled](bool ok, const QJsonObject &payload) {
                     QVERIFY(!cancelled.delivered);
                     cancelled.delivered = true;
                     cancelled.ok = ok;
                     cancelled.payload = payload;
                   });
    // 同步栈内关页：解码任务尚未完成（无事件循环让出），挂起回执按页关闭
    // 收口 cancelled。
    fixture.stack->preview->closeAssetTab(sgy.id);
    QVERIFY(cancelled.delivered);
    QCOMPARE(cancelled.payload.value(QStringLiteral("status")).toString(),
             QStringLiteral("cancelled"));
    QVERIFY(!cancelled.ok);
  }
}


// ---- 真实窗口 GUI 演示（PALEO_I4_GUI_EVIDENCE_DIR 触发；offscreen 车道跳过）----
// 序列：查询当前状态 → 选两口井 → 定位包络 → 打开明确井资料版本 → 显示
// 明确 inline；另演示只读工程成功导航与被拒绝的非法目标。逐步截图 + 结果
// JSON 落盘（非 offscreen 冒充——真实 xcb 窗口、真实解码/选择/定位）。
void TestI4NavigationAtoms::seismicLineReopenAtDisplayedLineReportsRealFailure()
{
  // 审查修复（I4-R1）回归：既有页已显示 IL1000 时请求会失败的版本（坏
  // 指纹外链）同线重开——不得用「标题后缀已匹配」伪造 shown 终态吞掉
  // 真实失败（旧短路的后缀与版本无关且重建后已陈旧）。
  Fixture fixture;
  QVERIFY(makeFixture(fixture));
  DataCatalog *catalog = fixture.catalog();
  PaleoProjectStore taskStore;
  PaleoTaskService taskSvc(&taskStore);
  fixture.stack->preview->setTaskService(&taskSvc);

  const CatalogAsset sgy = catalog->assetById(fixture.ids.sgy);
  const CatalogVersion good = catalog->currentVersion(sgy.id);
  CatalogVersion broken = good;
  broken.id = QStringLiteral("ver-i4-sgy-reopen-bad");
  broken.versionNumber = good.versionNumber + 1;
  broken.managed = false;
  broken.path = fixture.ids.sgySource; // 真实 SEG-Y + 坏指纹 → 解码 SHA 闸失败
  QVERIFY(QFile::exists(broken.path));
  broken.sha256 = QStringLiteral("2222222222222222222222222222222222222222222222222222222222222222");
  QVERIFY(catalog->addVersion(broken));

  AgentNavigationAdapter adapter(fixture.stack->preview.get(), new SelectionContext(), nullptr);
  paleo::agent::navigation::AgentNavigation navigation;
  navigation.setPreviewAccess(&adapter, &adapter);

  // 先开好版本到 IL1000 并等上屏。
  const Outcome initial = runNavigation(
      navigation, paleo::agent::navigation::kOpenSeismicLineTool,
      {{QStringLiteral("asset_id"), sgy.id},
       {QStringLiteral("version_id"), good.id},
       {QStringLiteral("axis"), QStringLiteral("inline")},
       {QStringLiteral("line_number"), 1000}},
      catalog);
  QVERIFY(initial.ok);

  // 同线请求坏版本：真实失败必须回报（不得伪造 shown）。
  const Outcome failed = runNavigation(
      navigation, paleo::agent::navigation::kOpenSeismicLineTool,
      {{QStringLiteral("asset_id"), sgy.id},
       {QStringLiteral("version_id"), broken.id},
       {QStringLiteral("axis"), QStringLiteral("inline")},
       {QStringLiteral("line_number"), 1000}},
      catalog);
  QVERIFY(!failed.ok);
  const QString status = failed.payload.value(QStringLiteral("status")).toString();
  QVERIFY2(status == QLatin1String("failed") || status == QLatin1String("rejected"),
           qPrintable(status));
}

void TestI4NavigationAtoms::guiEvidenceDemo()
{
  if (qgetenv("PALEO_I4_GUI_EVIDENCE_DIR").isEmpty())
    QSKIP("PALEO_I4_GUI_EVIDENCE_DIR 未设置（真实窗口 GUI 证据为手动流程）");
  const QString dir = QString::fromLatin1(qgetenv("PALEO_I4_GUI_EVIDENCE_DIR"));
  QVERIFY(QDir().mkpath(dir));
  QFile transcript(QDir(dir).filePath(QStringLiteral("transcript.json")));
  QVERIFY(transcript.open(QIODevice::WriteOnly | QIODevice::Append));
  const auto logStep = [&transcript](const QString &name, const Outcome &outcome) {
    const QString line = QStringLiteral(R"({"step":")") + name +
                         QStringLiteral(R"(","ok":)") +
                         (outcome.ok ? QStringLiteral("true") : QStringLiteral("false")) +
                         QStringLiteral(R"(,"payload":)") +
                         QString::fromUtf8(
                             QJsonDocument(outcome.payload).toJson(QJsonDocument::Compact)) +
                         QLatin1Char('}');
    transcript.write((line + QLatin1Char('\n')).toUtf8());
  };

  Fixture fixture;
  QVERIFY(makeFixture(fixture));
  DataCatalog *catalog = fixture.catalog();
  PaleoProjectStore taskStore;
  PaleoTaskService taskSvc(&taskStore);
  fixture.stack->preview->setTaskService(&taskSvc);

  // 真实窗口：主画布（QgsMapCanvas）+ 原生预览标签。
  QgisCanvasController canvasCtl;
  QgsMapCanvas *canvas = canvasCtl.canvas();
  canvas->setDestinationCrs(
      QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt()));
  canvas->setExtent(QgsRectangle(0, 0, 8000, 8000));
  QWidget window;
  auto *layout = new QVBoxLayout(&window);
  layout->setContentsMargins(4, 4, 4, 4);
  canvas->setParent(&window);
  canvas->setMinimumSize(960, 540);
  layout->addWidget(canvas, 2);
  fixture.stack->preview->setParent(&window);
  layout->addWidget(fixture.stack->preview.get(), 1);
  window.resize(1280, 900);
  window.show();
  QVERIFY(QTest::qWaitForWindowExposed(&window));

  SelectionContext context;
  WellMapLink wellLink(canvas, &context);
  AgentNavigationAdapter adapter(fixture.stack->preview.get(), &context, &wellLink);
  AgentMapNavigation mapNavigation(&canvasCtl, fixture.stack->layerSvc.get(),
                                   &fixture.stack->projectSvc);
  paleo::agent::navigation::AgentNavigation navigation;
  navigation.setSelectionAccess(&adapter, &adapter);
  navigation.setMapAccess(&mapNavigation, &mapNavigation);
  navigation.setPreviewAccess(&adapter, &adapter);

  int shot = 0;
  const auto snap = [&dir, &window, &shot](const QString &name) {
    window.grab().save(QDir(dir).filePath(
        QStringLiteral("%1_%2.png").arg(++shot, 2, 10, QLatin1Char('0')).arg(name)));
  };

  // 1) 查询当前状态。
  const Outcome state =
      runNavigation(navigation, paleo::agent::navigation::kStateTool, {}, catalog);
  QVERIFY(state.ok);
  logStep(QStringLiteral("navigation_state"), state);
  snap(QStringLiteral("initial_state"));

  // 2) 选两口井（真实选择总线 + 井层回读）。
  QStringList wells;
  for (const CatalogEntity &well : catalog->entities(QStringLiteral("well")))
    if (well.hasSurface)
      wells.append(well.id);
  QVERIFY(wells.size() >= 2);
  const Outcome selected = runNavigation(
      navigation, paleo::agent::navigation::kSelectWellsTool,
      {{QStringLiteral("well_ids"), QJsonArray{wells.at(0), wells.at(1)}}}, catalog);
  QVERIFY(selected.ok);
  logStep(QStringLiteral("select_wells"), selected);
  QCOMPARE(context.selectedIds().size(), 2);
  snap(QStringLiteral("two_wells_selected"));

  // 3) 定位井包络（fit——实际 extent 回读核含目标）。
  const Outcome located = runNavigation(
      navigation, paleo::agent::navigation::kMapLocateTool,
      {{QStringLiteral("well_ids"), QJsonArray{wells.at(0), wells.at(1)}},
       {QStringLiteral("mode"), QStringLiteral("fit")}},
      catalog);
  QVERIFY(located.ok);
  logStep(QStringLiteral("map_locate"), located);
  snap(QStringLiteral("located_envelope"));

  // 4) 打开明确井资料版本（多井分层资产的固定版本 + 指定井）。
  const CatalogAsset tops = catalog->assetById(fixture.ids.tops);
  const CatalogVersion topsVersion = catalog->currentVersion(tops.id);
  QString linkedWell;
  for (const EntityAssetLink &l : catalog->linksForAsset(tops.id))
    if (l.entityType == QLatin1String("well") && !l.unresolved && !l.entityId.isEmpty()) {
      linkedWell = l.entityId;
      break;
    }
  QVERIFY(!linkedWell.isEmpty());
  const Outcome wellView = runNavigation(
      navigation, paleo::agent::navigation::kOpenWellViewTool,
      {{QStringLiteral("well_id"), linkedWell},
       {QStringLiteral("asset_id"), tops.id},
       {QStringLiteral("version_id"), topsVersion.id}},
      catalog);
  QVERIFY(wellView.ok);
  logStep(QStringLiteral("open_well_view"), wellView);
  snap(QStringLiteral("well_view_version"));

  // 5) 显示明确 inline（真实解码呈现）。
  const CatalogAsset sgy = catalog->assetById(fixture.ids.sgy);
  const CatalogVersion sgyVersion = catalog->currentVersion(sgy.id);
  const Outcome line = runNavigation(
      navigation, paleo::agent::navigation::kOpenSeismicLineTool,
      {{QStringLiteral("asset_id"), sgy.id},
       {QStringLiteral("version_id"), sgyVersion.id},
       {QStringLiteral("axis"), QStringLiteral("inline")},
       {QStringLiteral("line_number"), 1000}},
      catalog);
  QVERIFY(line.ok);
  logStep(QStringLiteral("open_seismic_line"), line);
  snap(QStringLiteral("seismic_inline"));

  // 6) 只读工程：导航仍成功（Read 效果不受只读降级影响）。
  catalog->setLockedReadOnly(true);
  const Outcome readOnlyLocate = runNavigation(
      navigation, paleo::agent::navigation::kMapLocateTool,
      {{QStringLiteral("well_ids"), QJsonArray{wells.at(0)}},
       {QStringLiteral("mode"), QStringLiteral("center")}},
      catalog);
  QVERIFY(readOnlyLocate.ok);
  logStep(QStringLiteral("map_locate_read_only"), readOnlyLocate);
  snap(QStringLiteral("read_only_locate"));
  catalog->setLockedReadOnly(false);

  // 7) 被拒绝的非法目标（未知井 + 越界测线）。
  const Outcome unknownWell = runNavigation(
      navigation, paleo::agent::navigation::kSelectWellsTool,
      {{QStringLiteral("well_ids"), QJsonArray{QStringLiteral("ent-does-not-exist")}}},
      catalog);
  QVERIFY(!unknownWell.ok);
  logStep(QStringLiteral("select_wells_rejected"), unknownWell);
  const Outcome outOfRange = runNavigation(
      navigation, paleo::agent::navigation::kOpenSeismicLineTool,
      {{QStringLiteral("asset_id"), sgy.id},
       {QStringLiteral("version_id"), sgyVersion.id},
       {QStringLiteral("axis"), QStringLiteral("inline")},
       {QStringLiteral("line_number"), 999999}},
      catalog);
  QVERIFY(!outOfRange.ok);
  logStep(QStringLiteral("open_seismic_line_rejected"), outOfRange);
  snap(QStringLiteral("rejected_targets"));
  // 收尾拆离：window 先于 fixture/canvasCtl 析构——把 borrowed 子件还回
  // 各自 owner（unique_ptr/控制器析构），避免双删。
  fixture.stack->preview->setParent(nullptr);
  canvas->setParent(nullptr);
  transcript.flush();
  transcript.close();
}

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestI4NavigationAtoms tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_i4_navigation_atoms.moc"
