// 层：测试壳（#275 UIS-07 / #282 UIS-09 / #236 工程切换残留批——真实主窗壳级回归）
//
// 共用同一个 AppContext + PaleoMainWindow + attachWorkflows 真实壳，驱动
// 「工程生命周期 × 组装根接线」边界：入口调用时现取 projectDir/gpkg（#275）、
// 关窗/切工程检查 QgsProject 脏状态（#282）、切工程状态残留（#236）。
#include <QApplication>
#include <QAbstractButton>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPushButton>
#include <QDirIterator>
#include <QSettings>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <functional>
#include <memory>

#include "../src/app/appcontext.h"
#include "../src/catalog/datacatalog.h"
#include "../src/io/dataimportservice.h"
#include "../src/qgis/qgiscanvascontroller.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/services/paleotaskservice.h"
#include "../src/ui/layers/layertreepanel.h"
#include "../src/ui/pages/datalist.h"
#include "../src/ui/paleomainwindow.h"

namespace
{

// 5×5 平面散点（同 tst_surfacegridding_thread 夹具口径，网格化最小输入）。
QByteArray planeScatter()
{
  QByteArray t;
  for (int j = 0; j <= 4; ++j)
    for (int i = 0; i <= 4; ++i)
      t += QByteArray::number(i * 100.0) + ' ' + QByteArray::number(j * 100.0) + ' ' +
           QByteArray::number(1000.0 + i * 2.0 + j) + '\n';
  return t;
}

bool writeFile(const QString &path, const QByteArray &data)
{
  if (!QDir().mkpath(QFileInfo(path).absolutePath()))
    return false;
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly))
    return false;
  f.write(data);
  return true;
}

} // namespace

class TestShellProjectLifecycle : public QObject
{
  Q_OBJECT
public:
  explicit TestShellProjectLifecycle(AppContext *ctx, QObject *parent = nullptr)
    : QObject(parent), m_ctx(ctx)
  {
  }

private:
  AppContext *m_ctx = nullptr;
  PaleoMainWindow *m_win = nullptr;

  DataListPanel *listPanel() const { return m_win->findChild<DataListPanel *>(); }
  LayerTreePanel *layerPanel() const { return m_win->findChild<LayerTreePanel *>(); }

  // 注册层位资产 + 受管 RAW 版本（散点文本真实落盘 projectDir 下）。
  bool addHorizonAsset(DataCatalog *cat, const QDir &dir, const QString &assetId)
  {
    if (!cat)
      return false;
    CatalogAsset a;
    a.id = assetId;
    a.type = QStringLiteral("horizon");
    a.format = QStringLiteral("csv");
    a.displayName = assetId + QStringLiteral(".csv");
    QString err;
    if (!cat->addAsset(a, &err))
      return false;
    const QString verId = cat->nextVersionId();
    const QString rel = DataCatalog::managedPath(QStringLiteral("RAW"), assetId, verId,
                                                 QStringLiteral("h.csv"));
    const QString abs = dir.filePath(rel);
    if (!writeFile(abs, planeScatter()))
      return false;
    CatalogVersion v;
    v.id = verId;
    v.assetId = assetId;
    v.stage = QStringLiteral("RAW");
    v.versionNumber = 1;
    v.managed = true;
    v.path = rel;
    v.fileName = QStringLiteral("h.csv");
    v.sha256 = DataCatalog::sha256FileHex(abs, &err);
    if (v.sha256.isEmpty())
      return false;
    return cat->addVersion(v, &err);
  }

  // 轮询等待任务终态（网格化异步跑在任务池）。
  static bool waitFinished(PaleoTask *task, int timeoutMs = 30000) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs)
    {
      if (task->state() != PaleoTask::State::Running)
        return true;
      QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return task->state() != PaleoTask::State::Running;
  }

  // armed 在模态窗出现后点 griddingOkButton（无该按钮则 reject）。
  static void armGriddingDialogCloser()
  {
    auto closer = std::make_shared<int>(0);
    auto fn = std::make_shared<std::function<void()>>();
    *fn = [closer, fn]() mutable {
      if (++(*closer) > 200) // ~10 s 上限，防悬挂
        return;
      if (auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
      {
        if (auto *ok = dlg->findChild<QAbstractButton *>(QStringLiteral("griddingOkButton")))
          ok->click();
        else
          dlg->reject();
        return;
      }
      QTimer::singleShot(50, [fn]() { (*fn)(); });
    };
    QTimer::singleShot(50, [fn]() { (*fn)(); });
  }

  // armed 关闭随后出现的任意模态窗（WellTopsEditorDialog 等）；出现与否
  // 返回给调用方断言（门放行 → 对话框真实弹出）。
  static void armAnyDialogCloser(bool *seen)
  {
    auto closer = std::make_shared<std::pair<int, bool *>>(0, seen);
    auto fn = std::make_shared<std::function<void()>>();
    *fn = [closer, fn]() mutable {
      if (++closer->first > 200)
        return;
      if (auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
      {
        *closer->second = true;
        dlg->reject();
        return;
      }
      QTimer::singleShot(50, [fn]() { (*fn)(); });
    };
    QTimer::singleShot(50, [fn]() { (*fn)(); });
  }

private slots:
  void initTestCase()
  {
    QVERIFY2(m_ctx->ready(), "AppContext failed to bring up QgisRuntime");
    QSettings(QStringLiteral("paleo"), QStringLiteral("paleo")).clear();
    m_win = new PaleoMainWindow(m_ctx->canvasCtl(), m_ctx->projectSvc(),
                                m_ctx->layerSvc(), m_ctx->toolSvc(), m_ctx->selection());
    m_win->attachWorkflows(m_ctx->predictionWf(), m_ctx->constraintWf(),
                           m_ctx->compositionWf(), m_ctx->validationWf(),
                           m_ctx->importSvc(), m_ctx->seismicLink(),
                           m_ctx->processingSvc(), m_ctx->store(),
                           m_ctx->editingSvc(), m_ctx->layoutSvc(), m_ctx->taskSvc());
    QVERIFY(listPanel());
    m_win->show();
    QApplication::processEvents();
  }

  void cleanupTestCase()
  {
    delete m_win;
    m_win = nullptr;
  }

  // #275：attach 期工程未开，三入口闸住并提示「先打开工程」（门本身保留）。
  void gatesBlockedBeforeProjectOpen()
  {
    QVERIFY(m_ctx->projectSvc()->projectPath().isEmpty());
    auto *bar = m_win->statusBar();
    bar->clearMessage();

    emit listPanel()->gridHorizonRequested(QStringLiteral("ast-nope"));
    QVERIFY2(bar->currentMessage().contains(QStringLiteral("先打开工程")),
             "网格化入口在无工程时必须闸住并提示先打开工程");

    bar->clearMessage();
    emit listPanel()->topsEditRequested(QStringLiteral("ast-nope"));
    QVERIFY2(bar->currentMessage().contains(QStringLiteral("先打开工程")),
             "编辑分层入口在无工程时必须闸住并提示先打开工程");

    bar->clearMessage();
    // 面运算信号在 LayerTreePanel 上——直发信号，走同一 lambda。
    if (auto *lp = layerPanel())
    {
      bar->clearMessage();
      emit lp->surfaceOpsRequested(QString());
      QVERIFY2(bar->currentMessage().contains(QStringLiteral("先打开工程")) ||
                   bar->currentMessage().contains(QStringLiteral("需要已打开的工程")),
               "面运算入口在无工程时必须闸住并提示");
    }
  }

  // #275：打开工程后网格化入口放行——参数表 → 任务真实启动。
  void griddingTaskStartsAfterProjectOpen()
  {
    QTemporaryDir dirA;
    QVERIFY(dirA.isValid());
    QVERIFY(m_ctx->projectSvc()->createProject(dirA.filePath(QStringLiteral("a.qgz"))));
    QApplication::processEvents(); // projectOpened → catalog 原地重绑

    QVERIFY(addHorizonAsset(m_ctx->importSvc()->catalog(), QDir(dirA.path()),
                            QStringLiteral("hz-a1")));

    QSignalSpy added(m_ctx->taskSvc(), &PaleoTaskService::taskAdded);
    armGriddingDialogCloser();
    emit listPanel()->gridHorizonRequested(QStringLiteral("hz-a1"));
    // 门放行 → 弹参数表（closer 代点 OK）→ taskSvc->start。
    QTRY_VERIFY2_WITH_TIMEOUT(!added.isEmpty(), "网格化任务必须真实启动", 15000);
    QVERIFY2(!m_win->statusBar()->currentMessage().contains(QStringLiteral("先打开工程")),
             "工程已打开时不得再提示先打开工程");
    auto *task = added.last().at(0).value<PaleoTask *>();
    QVERIFY(task);
    QVERIFY2(waitFinished(task), "网格化任务须在时限内完成");
    QCOMPARE(task->state(), PaleoTask::State::Succeeded);
  }

  // #275：切到第二个工程后入口跟随新工程（projectDir 不钉死首工程），
  // 产物落第二个工程受管区。
  void griddingFollowsProjectSwitch()
  {
    QTemporaryDir dirB;
    QVERIFY(dirB.isValid());
    QVERIFY(m_ctx->projectSvc()->createProject(dirB.filePath(QStringLiteral("b.qgz"))));
    QApplication::processEvents();

    QVERIFY(addHorizonAsset(m_ctx->importSvc()->catalog(), QDir(dirB.path()),
                            QStringLiteral("hz-b1")));

    QSignalSpy added(m_ctx->taskSvc(), &PaleoTaskService::taskAdded);
    armGriddingDialogCloser();
    emit listPanel()->gridHorizonRequested(QStringLiteral("hz-b1"));
    QTRY_VERIFY2_WITH_TIMEOUT(!added.isEmpty(), "切工程后网格化任务必须真实启动", 15000);
    auto *task = added.last().at(0).value<PaleoTask *>();
    QVERIFY(task);
    QVERIFY2(waitFinished(task), "网格化任务须在时限内完成");
    QCOMPARE(task->state(), PaleoTask::State::Succeeded);

    // 产物（DERIVED 受管 tif）必须落在第二个工程目录下。
    bool foundTif = false;
    QDirIterator it(dirB.path(), {QStringLiteral("*.tif")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
      it.next();
      foundTif = true;
    }
    QVERIFY2(foundTif, "网格化产物必须写进第二个工程的受管区（projectDir 调用时现取）");
  }

  // #275：编辑分层入口调用时现取 projectDir——开工程后不再闸住，且对话框
  // 真实弹出（WellTopsEditorDialog 模态，测试代为关闭）。
  void topsEditGateFetchesProjectAtCallTime()
  {
    auto *bar = m_win->statusBar();
    bar->clearMessage();
    bool seen = false;
    armAnyDialogCloser(&seen);
    emit listPanel()->topsEditRequested(QStringLiteral("ast-nope"));
    QTRY_VERIFY_WITH_TIMEOUT(seen, 10000);
    QVERIFY2(!bar->currentMessage().contains(QStringLiteral("先打开工程")),
             "工程已打开时编辑分层不得再提示先打开工程");
  }

  // #275：面运算入口调用时现取 projectDir——开工程后门放行（真实进入
  // 参数表模态，测试代关），不得再报「先打开工程」。
  void surfaceOpsGateFetchesProjectAtCallTime()
  {
    auto *lp = layerPanel();
    if (!lp)
      QSKIP("LayerTreePanel 缺席（无图层平台环境）");
    auto *bar = m_win->statusBar();
    bar->clearMessage();
    // 门放行 → promptIsopach 参数表真实弹出（有 ≥2 栅格声明时是模态）。
    bool seen = false;
    armAnyDialogCloser(&seen);
    emit lp->surfaceOpsRequested(QString());
    QTRY_VERIFY_WITH_TIMEOUT(seen, 10000);
    QApplication::processEvents();
    QVERIFY2(!bar->currentMessage().contains(QStringLiteral("先打开工程")) &&
                 !bar->currentMessage().contains(QStringLiteral("需要已打开的工程")),
             "工程已打开时面运算不得再报先打开工程");
  }

};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");
  AppContext ctx(QStringLiteral("/usr"));
  if (!ctx.ready())
    qFatal("AppContext failed to initialize the QGIS runtime");
  TestShellProjectLifecycle tc(&ctx);
  return QTest::qExec(&tc, argc, argv);
}
#include "tst_shell_projectlifecycle.moc"
