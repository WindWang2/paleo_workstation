// 层：测试壳（B 任务 2026-10-12 深化——层位状态一致性壳级回归）
//
// 共用真实 AppContext + PaleoMainWindow + attachWorkflows/attachMapping 壳
// （main.cpp 同参接线），驱动「层位词表 × 页面状态」边界：
//   1) 工程打开 → 预测/单因素页的层位下拉按 mappingHorizons() 填充
//      （修复前生产链从未填充，两页发出的意图恒携带空层位）；
//   2) 层位 chip 切换（SelectionContext::setActiveHorizon，唯一真源）→
//      两页下拉跟随同一层位；
//   3) 发布门随层位切换重算——版本胶囊反映当前层位的版本状态，不再
//      滞留切换前的层位。
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QTemporaryDir>
#include <QtTest>

#include <qgslayertree.h>
#include <qgslayertreelayer.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>

#include "../src/app/appcontext.h"
#include "../src/domain/mappinghorizons.h"
#include "../src/io/dataimportservice.h" // importSvc()->catalog() 需完整类型
#include "../src/linkage/selectioncontext.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/mapversionstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h" // projectSvc()->createProject 需完整类型
#include "../src/ui/edittools/editingtoolbar.h"
#include "../src/ui/paleomainwindow.h"
#include "../src/ui/pages/pagepanels.h"

class TestShellHorizonState : public QObject
{
  Q_OBJECT
  public:
    explicit TestShellHorizonState(AppContext *ctx) : m_ctx(ctx) {}

  private slots:
    void initTestCase();
    void cleanupTestCase();
    void horizonCombosFollowProjectAndChip();
    void publishGateFollowsActiveHorizon();
    void factorVisibilitySurvivesPageSwitch();
    void editingToolbarListsOnlyActiveHorizonLayers();

  private:
    AppContext *m_ctx = nullptr;
    PaleoMainWindow *m_win = nullptr;
};

// 声明一条 04_SingleFactor 组的矢量层（与生产单因素成果同组词表）。
static LayerDeclaration factorDecl(const QString &layerId, const QString &horizon)
{
  LayerDeclaration d;
  d.layerId = layerId;
  d.horizon = horizon;
  d.type = QStringLiteral("vector");
#ifdef FIXTURE_GPKG
  d.source = QStringLiteral(FIXTURE_GPKG) + QStringLiteral("|layername=basin");
#else
  d.source = QStringLiteral("../testdata/fixture.gpkg|layername=basin");
#endif
  d.group = QStringLiteral("04_SingleFactor");
  d.title = layerId;
  return d;
}

void TestShellHorizonState::initTestCase()
{
  QVERIFY2(m_ctx->ready(), "AppContext failed to bring up QgisRuntime");
  m_win = new PaleoMainWindow(m_ctx->canvasCtl(), m_ctx->projectSvc(),
                              m_ctx->layerSvc(), m_ctx->toolSvc(), m_ctx->selection());
  m_win->attachWorkflows(m_ctx->predictionWf(), m_ctx->constraintWf(),
                         m_ctx->compositionWf(), m_ctx->validationWf(),
                         m_ctx->importSvc(), m_ctx->seismicLink(),
                         m_ctx->processingSvc(), m_ctx->store(),
                         m_ctx->editingSvc(), m_ctx->layoutSvc(), m_ctx->taskSvc());
  // 与 main.cpp 同参：发布门/导出/版本接线（attachWorkflows 不含 attachMapping）。
  m_win->attachMapping(m_ctx->mappingWf(), m_ctx->versionCtl(), m_ctx->versionStore(),
                       m_ctx->projectData(),
                       m_ctx->importSvc() ? m_ctx->importSvc()->catalog() : nullptr);
  m_win->show();
  QApplication::processEvents();
}

void TestShellHorizonState::cleanupTestCase()
{
  delete m_win;
  m_win = nullptr;
}

// ---- 1/2：词表填充 + chip 联动 ----
void TestShellHorizonState::horizonCombosFollowProjectAndChip()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QVERIFY(m_ctx->projectSvc()->createProject(dir.filePath(QStringLiteral("combos.qgz"))));
  QApplication::processEvents(); // projectOpened → 层位词表/页面填充

  const QStringList words = mappingHorizons();
  QVERIFY2(words.size() >= 2, "默认工程必须至少有两个编图层位");
  const auto combos =
      m_win->findChildren<QComboBox *>(QStringLiteral("horizonCombo"));
  QCOMPARE(combos.size(), 2); // 预测页 + 单因素页各一份
  for (auto *combo : combos)
  {
    QCOMPARE(combo->count(), words.size());
    for (int i = 0; i < words.size(); ++i)
      QCOMPARE(combo->itemText(i), words.at(i));
  }

  // chip 切到词表末位（与填充默认的首位不同）→ 两页下拉跟随。
  const QString target = words.last();
  m_ctx->selection()->setActiveHorizon(target);
  QApplication::processEvents();
  for (auto *combo : combos)
    QCOMPARE(combo->currentText(), target);
}

// ---- 3：发布门随层位重算 ----
void TestShellHorizonState::publishGateFollowsActiveHorizon()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QVERIFY(m_ctx->projectSvc()->createProject(dir.filePath(QStringLiteral("gate.qgz"))));
  QApplication::processEvents();

  const QStringList words = mappingHorizons();
  QVERIFY(words.size() >= 2);
  const QString hA = words.first();
  const QString hB = words.at(1);

  auto *capsule = m_win->findChild<QLabel *>(QStringLiteral("publishStateLabel"));
  QVERIFY(capsule);

  // hA 存一版；hB 无版本。切换层位时胶囊必须跟随各自层位的事实
  //（修复前：切层不重算，胶囊滞留旧层位的版本状态）。
  QString err;
  const MapVersion v = m_ctx->versionStore()->saveVersion(hA, QStringLiteral("{}"), &err);
  QVERIFY2(v.version >= 1, qPrintable(err));

  m_ctx->selection()->setActiveHorizon(hA);
  QApplication::processEvents();
  QCOMPARE(capsule->text(), QStringLiteral("编辑中 · v%1").arg(v.version));

  m_ctx->selection()->setActiveHorizon(hB);
  QApplication::processEvents();
  QCOMPARE(capsule->text(), QStringLiteral("还没有保存的版本"));

  m_ctx->selection()->setActiveHorizon(hA);
  QApplication::processEvents();
  QCOMPARE(capsule->text(), QStringLiteral("编辑中 · v%1").arg(v.version));
}

// ---- 4：单因素上图后切页回放不回滚（页主题回写对称）----
// 修复前：factorVisibilityRequested 只拨树节点、不回写 page:constraint
// 主题；切页再回来 applyPrunedTheme 用旧主题覆盖树态——用户刚上的图被
// 静默撤下（预测页「显示结果」同款场景已有回写，此处消除不对称）。
void TestShellHorizonState::factorVisibilitySurvivesPageSwitch()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QVERIFY(m_ctx->projectSvc()->createProject(dir.filePath(QStringLiteral("theme.qgz"))));
  QApplication::processEvents();

  const QStringList words = mappingHorizons();
  QVERIFY(words.size() >= 1);
  const QString hA = words.first();
  m_ctx->selection()->setActiveHorizon(hA);
  QApplication::processEvents();

  // 1) 先到单因素页，让 page:constraint 主题在因子层存在之前定格
  //   （旧主题不含该层——回放即隐藏）。
  m_win->showPage(QStringLiteral("constraint"));
  QApplication::processEvents();

  const QString layerId = QStringLiteral("factor.shelltest.%1").arg(hA);
  QString err;
  QVERIFY2(m_ctx->layerSvc()->declare(factorDecl(layerId, hA), &err), qPrintable(err));
  QgsMapLayer *layer = m_ctx->layerSvc()->instantiate(layerId, &err);
  QVERIFY2(layer != nullptr, qPrintable(err));
  QgsProject *proj = m_ctx->projectSvc()->project();
  QgsLayerTreeLayer *node = proj->layerTreeRoot()->findLayer(layer->id());
  QVERIFY(node != nullptr);
  node->setItemVisibilityChecked(false); // 先下（模拟互斥后的旧状态）
  QApplication::processEvents();

  // 2) 经页面信号上图（与 UI 勾选同一路径）。
  auto *constraintPage = m_win->findChild<ConstraintPage *>();
  QVERIFY(constraintPage != nullptr);
  QVERIFY(QMetaObject::invokeMethod(constraintPage, "factorVisibilityRequested",
                                    Q_ARG(QString, layerId), Q_ARG(bool, true)));
  QApplication::processEvents();
  QVERIFY(node->isVisible());

  // 3) 切走再切回：树态必须保持（修复前在此回滚为不可见）。
  m_win->showPage(QStringLiteral("predict"));
  QApplication::processEvents();
  m_win->showPage(QStringLiteral("constraint"));
  QApplication::processEvents();
  QVERIFY2(node->isVisible(), "切页回放不得撤下用户刚上的单因素图");
}

// ---- 5：编辑工具条只列活动层位的可编辑层 ----
// 工具条头文件契约：层位业务规则由宿主注入过滤器。修复前全仓无注入，
// 跨层位实例化层（验证/收编场景）可在任意页签进入编辑。
void TestShellHorizonState::editingToolbarListsOnlyActiveHorizonLayers()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QVERIFY(m_ctx->projectSvc()->createProject(dir.filePath(QStringLiteral("filter.qgz"))));
  QApplication::processEvents();

  const QStringList words = mappingHorizons();
  QVERIFY(words.size() >= 2);
  const QString hA = words.first();
  const QString hB = words.at(1);
  m_ctx->selection()->setActiveHorizon(hA);
  QApplication::processEvents();

  auto *toolbar = m_win->findChild<PaleoEditingToolbar *>(QStringLiteral("editingToolbar"));
  QVERIFY(toolbar != nullptr);

  const QString idA = QStringLiteral("factor.edit.%1").arg(hA);
  const QString idB = QStringLiteral("factor.edit.%1").arg(hB);
  const QString sharedId = QStringLiteral("wells.shared");
  QString err;
  QVERIFY(m_ctx->layerSvc()->declare(factorDecl(idA, hA), &err));
  LayerDeclaration shared = factorDecl(sharedId, QString());
  shared.group = QStringLiteral("00_Data");
  QVERIFY(m_ctx->layerSvc()->declare(shared, &err));
  QVERIFY(m_ctx->layerSvc()->declare(factorDecl(idB, hB), &err));

  QgsMapLayer *layerA = m_ctx->layerSvc()->instantiate(idA, &err);
  QVERIFY2(layerA != nullptr, qPrintable(err));
  QgsMapLayer *sharedLayer = m_ctx->layerSvc()->instantiate(sharedId, &err);
  QVERIFY2(sharedLayer != nullptr, qPrintable(err));
  // 跨层位显式实例化（验证链场景）：在工程里，但不属于活动层位。
  QgsMapLayer *layerB = m_ctx->layerSvc()->instantiate(idB, &err);
  QVERIFY2(layerB != nullptr, qPrintable(err));
  toolbar->refreshFromProject();
  QApplication::processEvents();

  const auto comboLists = [toolbar](QgsMapLayer *layer) {
    auto *combo = toolbar->layerCombo();
    for (int i = 0; i < combo->count(); ++i)
      if (qvariant_cast<QgsVectorLayer *>(combo->itemData(i)) == layer)
        return true;
    return false;
  };
  QVERIFY(comboLists(layerA));      // 活动层位层可编辑
  QVERIFY(comboLists(sharedLayer)); // 层位无关共享层不受限
  QVERIFY2(!comboLists(layerB), "非活动层位的图层不得进入编辑下拉（修复前无过滤器注入）");

  // 切到 hB：hB 层进入可编辑集，hA 层退出（chipbar 同款双调——
  // SelectionContext 是状态真源，图层服务实例化跟随 chip 入口）。
  m_ctx->layerSvc()->setActiveHorizon(hB);
  m_ctx->selection()->setActiveHorizon(hB);
  QApplication::processEvents();
  QVERIFY(comboLists(layerB));
  QVERIFY(!comboLists(layerA));
}

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");
  AppContext ctx(QStringLiteral("/usr"));
  if (!ctx.ready())
    qFatal("AppContext failed to initialize the QGIS runtime");
  TestShellHorizonState tc(&ctx);
  return QTest::qExec(&tc, argc, argv);
}
#include "tst_shell_horizonstate.moc"
