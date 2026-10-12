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

#include "../src/app/appcontext.h"
#include "../src/domain/mappinghorizons.h"
#include "../src/io/dataimportservice.h" // importSvc()->catalog() 需完整类型
#include "../src/linkage/selectioncontext.h"
#include "../src/metadata/mapversionstore.h"
#include "../src/qgis/qgisprojectservice.h" // projectSvc()->createProject 需完整类型
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

  private:
    AppContext *m_ctx = nullptr;
    PaleoMainWindow *m_win = nullptr;
};

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
