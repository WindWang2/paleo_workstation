// 层：测试壳（#275 UIS-07 / #282 UIS-09 / #236 工程切换残留批 / #226 解释
// 登记链接线——真实主窗壳级回归）
//
// 共用同一个 AppContext + PaleoMainWindow + attachWorkflows 真实壳，驱动
// 「工程生命周期 × 组装根接线」边界：入口调用时现取 projectDir/gpkg（#275）、
// 关窗/切工程检查 QgsProject 脏状态（#282）、切工程状态残留（#236）。
#include <QApplication>
#include <QAbstractButton>
#include <QDateTime>
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
#include <QListWidget>

#include <functional>
#include <memory>

#include "../src/app/appcontext.h"
#include "../src/catalog/datacatalog.h"
#include "../src/io/dataimportservice.h"
#include "../src/qgis/qgiscanvascontroller.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/services/paleotaskservice.h"
#include "../src/ui/layers/layertreepanel.h"
#include "../src/ui/notifications/paleonotify.h"
#include "../src/ui/pages/datalist.h"
#include "../src/ui/paleomainwindow.h"
#include "../src/ui/seismicsection/seismicsectiondockwidget.h"

#include <qgis/qgsproject.h>

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
  // #282：跨测试函数存活的工程目录（函数内 QTemporaryDir 会随测试返回销毁，
  // 后续 writeProject/关窗就写给已删目录）。
  QTemporaryDir m_dirA;
  QTemporaryDir m_dirB;
  QString m_qgzA;
  QString m_qgzB;

  DataListPanel *listPanel() const { return m_win->findChild<DataListPanel *>(); }
  LayerTreePanel *layerPanel() const { return m_win->findChild<LayerTreePanel *>(); }
  seismic::SeismicSectionDockWidget *sectionDock() const
  {
    return m_win->findChild<seismic::SeismicSectionDockWidget *>();
  }
  QListWidget *recentList() const
  {
    return m_win->findChild<QListWidget *>(QStringLiteral("recentProjectsList"));
  }

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

  // #226：注册地震资产（合成 SGY 不必真实存在——注入只看资产/版本身份，
  // 体加载在 syncSeismicVolumeToDocks 内部按文件存在性跳过）。
  bool addSeismicAsset(DataCatalog *cat, const QString &assetId)
  {
    if (!cat)
      return false;
    CatalogAsset a;
    a.id = assetId;
    a.type = QStringLiteral("seismic");
    a.format = QStringLiteral("sgy");
    a.displayName = assetId + QStringLiteral(".sgy");
    QString err;
    if (!cat->addAsset(a, &err))
      return false;
    CatalogVersion v;
    v.id = cat->nextVersionId();
    v.assetId = assetId;
    v.stage = QStringLiteral("RAW");
    v.versionNumber = 1;
    v.managed = false;
    v.path = QStringLiteral("sg.sgy"); // 相对工程目录；不存在 → 不装体
    v.fileName = QStringLiteral("sg.sgy");
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
    m_win->setProjectSaveAskForTesting(nullptr); // 缝里的栈上 lambda 不得存活
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

  // #282：切换工程前查 QgsProject 脏状态——「取消」原地不动，「保存」
  // 先落盘再切。询问走注入 seam（替身模态框）。
  // 注：QgisProjectService 单实例复用同一 QgsProject（clear+read），当前
  // 工程就是切换前的那个；新建的第二个工程经 createProject 完整落成，
  // 切回第一个工程即可同时覆盖「取消原地不动 / 保存落盘再切」两分支。
  void switchProjectAsksWhenDirty()
  {
    QVERIFY(m_dirA.isValid());
    m_qgzA = m_dirA.filePath(QStringLiteral("a.qgz"));
    QVERIFY(m_ctx->projectSvc()->createProject(m_qgzA));
    QApplication::processEvents();
    QCOMPARE(m_ctx->projectSvc()->projectPath(), m_qgzA);

    // 第二个工程完整落成（createProject 自带 manifest/受管目录结构——
    // writeProject 需要它）。当前工程切到 B。
    QVERIFY(m_dirB.isValid());
    m_qgzB = m_dirB.filePath(QStringLiteral("b.qgz"));
    QVERIFY(m_ctx->projectSvc()->createProject(m_qgzB));
    QApplication::processEvents();
    QCOMPARE(m_ctx->projectSvc()->projectPath(), m_qgzB);

    // 真实用户动作改脏当前工程 B：翻转工程吸附开关（setSnappingConfig →
    // setDirty）。询问语义针对「切走前在场的工程」，造脏必须落在 B 上。
    QgsProject *proj = m_ctx->projectSvc()->project();
    auto snap = proj->snappingConfig();
    snap.setEnabled(!snap.enabled());
    proj->setSnappingConfig(snap);
    QVERIFY2(proj->isDirty(), "吸附开关切换后工程应为脏状态");

    auto *list = recentList();
    QVERIFY(list);
    auto *item = new QListWidgetItem(m_qgzA, list);
    item->setData(Qt::UserRole, m_qgzA);

    int askCount = 0;
    auto choice = PaleoNotify::SaveChoice::Cancel;
    m_win->setProjectSaveAskForTesting(
        [&askCount, &choice](const QString &, const QString &) {
          ++askCount;
          return choice;
        });

    // 「取消」：原地不动（仍停留在 B），目标工程不得被打开。
    emit list->itemActivated(item);
    QApplication::processEvents();
    QCOMPARE(askCount, 1);
    QCOMPARE(m_ctx->projectSvc()->projectPath(), m_qgzB);

    // 「保存」：先写当前工程（B 的 .qgz mtime 更新）再切到 A，只问一次。
    // 切换经 ProjectOpenWorkflow 异步打开（openProjectAsync）——等终态。
    QTest::qWait(60); // 保证 mtime 可辨
    const QDateTime before = QFileInfo(m_qgzB).lastModified();
    choice = PaleoNotify::SaveChoice::Save;
    emit list->itemActivated(item);
    QApplication::processEvents();
    QCOMPARE(askCount, 2);
    QTRY_VERIFY_WITH_TIMEOUT(m_ctx->projectSvc()->projectPath() == m_qgzA, 5000);
    QVERIFY2(QFileInfo(m_qgzB).lastModified() > before,
             "选保存后当前工程 .qgz 必须落盘（mtime 更新）");

    m_win->setProjectSaveAskForTesting(nullptr);
  }

  // #282：干净的工程切换不打扰——一次询问都不弹。
  void switchCleanProjectDoesNotAsk()
  {
    // 当前工程（上一测试切回的 A）完整落成 → 落盘清脏。
    QVERIFY(m_ctx->projectSvc()->writeProject());
    QVERIFY2(!m_ctx->projectSvc()->project()->isDirty(), "写盘后工程应为干净");
    QVERIFY2(!m_ctx->projectSvc()->project()->isDirty(), "写盘后工程应为干净");

    int askCount = 0;
    m_win->setProjectSaveAskForTesting(
        [&askCount](const QString &, const QString &) {
          ++askCount;
          return PaleoNotify::SaveChoice::Cancel;
        });
    // 切回 B（上一测试完整落成的工程；其 .qgz 已带 native 吸附配置，
    // 重开不再触发 snapping 替换改脏）。
    auto *list = recentList();
    QVERIFY(list);
    QString target;
    for (int i = 0; i < list->count() && target.isEmpty(); ++i)
      if (QListWidgetItem *it = list->item(i))
        if (it->data(Qt::UserRole).toString() == m_qgzB)
          target = m_qgzB;
    QVERIFY2(!target.isEmpty(), "最近列表应含上一测试创建的 B");
    auto *item = new QListWidgetItem(target, list);
    item->setData(Qt::UserRole, target);
    emit list->itemActivated(item);
    // 切换经 ProjectOpenWorkflow 异步打开——等终态再断言（不脏则一次都不问）。
    QTRY_VERIFY_WITH_TIMEOUT(m_ctx->projectSvc()->projectPath() == m_qgzB, 5000);
    QCOMPARE(askCount, 0); // 不脏不问
    m_win->setProjectSaveAskForTesting(nullptr);
  }

  // #282：关窗同样查工程脏状态——取消不关、保存关且落盘。
  void closeWindowAsksWhenDirty()
  {
    // 上一用例异步切到 B 后它完整落成——等尾事件稳定再开始。
    QApplication::processEvents();
    const QString qgz = m_ctx->projectSvc()->projectPath();
    QVERIFY(!qgz.isEmpty());
    QgsProject *proj = m_ctx->projectSvc()->project();
    auto snap = proj->snappingConfig();
    snap.setEnabled(!snap.enabled());
    proj->setSnappingConfig(snap);
    QVERIFY(proj->isDirty());
    int askCount = 0;
    auto choice = PaleoNotify::SaveChoice::Cancel;
    m_win->setProjectSaveAskForTesting(
        [&askCount, &choice](const QString &, const QString &) {
          ++askCount;
          return choice;
        });

    m_win->close(); // 取消 → 不关
    QVERIFY(m_win->isVisible());
    QCOMPARE(askCount, 1);

    QTest::qWait(60);
    const QDateTime before = QFileInfo(qgz).lastModified();
    choice = PaleoNotify::SaveChoice::Save;
    m_win->close(); // 保存 → 关 + 落盘
    QVERIFY2(!m_win->isVisible(), "选保存后关窗必须真正关闭");
    QCOMPARE(askCount, 2);
    QVERIFY(QFileInfo(qgz).lastModified() > before);
    m_win->setProjectSaveAskForTesting(nullptr);
  }

  // #226：解释登记链生产接线——打开工程即注入 catalog + 源体资产/版本 +
  // 解释产物目录（工程受管 artifacts/derived/interpretation），且切工程
  // 后跟随新工程（不钉死首工程）。
  void interpretationCatalogFollowsProject()
  {
    auto *dock = sectionDock();
    QVERIFY(dock);

    QTemporaryDir dirA;
    QVERIFY(dirA.isValid());
    QVERIFY(m_ctx->projectSvc()->createProject(dirA.filePath(QStringLiteral("sa.qgz"))));
    QApplication::processEvents();
    QVERIFY(addSeismicAsset(m_ctx->importSvc()->catalog(),
                            QStringLiteral("seis_lifecycle_a")));
    QApplication::processEvents(); // catalog changed → syncSeismicVolumeToDocks

    QCOMPARE(dock->interpretationCatalog(), m_ctx->importSvc()->catalog());
    QCOMPARE(dock->interpretationAssetId(), QStringLiteral("seis_lifecycle_a"));
    const QString outA = dock->interpretationOutputDir();
    QVERIFY2(QDir::cleanPath(outA).startsWith(
                 QDir::cleanPath(dirA.path())),
             "解释产物目录必须落在当前工程受管区");
    QVERIFY2(outA.endsWith(QStringLiteral("artifacts/derived/interpretation")),
             qPrintable(QStringLiteral("实际：%1").arg(outA)));

    // 切到工程 B：注入跟随新工程。
    QTemporaryDir dirB;
    QVERIFY(dirB.isValid());
    QVERIFY(m_ctx->projectSvc()->createProject(dirB.filePath(QStringLiteral("sb.qgz"))));
    QApplication::processEvents();
    QVERIFY(addSeismicAsset(m_ctx->importSvc()->catalog(),
                            QStringLiteral("seis_lifecycle_b")));
    QApplication::processEvents();

    QCOMPARE(dock->interpretationCatalog(), m_ctx->importSvc()->catalog());
    QCOMPARE(dock->interpretationAssetId(), QStringLiteral("seis_lifecycle_b"));
    QVERIFY2(QDir::cleanPath(dock->interpretationOutputDir())
                 .startsWith(QDir::cleanPath(dirB.path())),
             "切工程后解释目录必须跟随新工程");
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
