#include <QtTest>
#include <QDialog>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QThread>
#include <QTemporaryDir>

#include <atomic>

#include <qgsapplication.h>
#include <qgsprocessingalgorithm.h>
#include <qgsprocessingalgorithmwidgetbase.h>
#include <qgsprocessingfeedback.h>
#include <qgsprocessingparameterswidget.h>
#include <qgsprocessingprovider.h>
#include <qgsprocessingregistry.h>

#include "../src/qgis/qgisprocessingservice.h"

class StoppableTestAlgorithm : public QgsProcessingAlgorithm
{
public:
  static inline std::atomic<bool> s_started{false};
  static inline std::atomic<bool> s_completed{false};
  static inline std::atomic<bool> s_sawCancel{false};

  QString name() const override { return QStringLiteral("stoppable_test"); }
  QString displayName() const override { return QStringLiteral("Stoppable Test Algorithm"); }
  QString group() const override { return QStringLiteral("Test"); }
  QString groupId() const override { return QStringLiteral("test"); }
  StoppableTestAlgorithm *createInstance() const override { return new StoppableTestAlgorithm(); }
  Qgis::ProcessingAlgorithmFlags flags() const override
  {
    return QgsProcessingAlgorithm::flags() | Qgis::ProcessingAlgorithmFlag::CanCancel;
  }

  void initAlgorithm(const QVariantMap &) override {}

  QVariantMap processAlgorithm(const QVariantMap &parameters, QgsProcessingContext &context,
                               QgsProcessingFeedback *feedback) override
  {
    Q_UNUSED(parameters);
    s_started = true;
    s_completed = false;
    s_sawCancel = false;

    for (int i = 0; i < 300; ++i)
    {
      if (feedback && feedback->isCanceled())
      {
        s_sawCancel = true;
        break;
      }
      if (feedback)
      {
        feedback->setProgress(i);
        feedback->pushInfo(QStringLiteral("Working"));
      }
      (void)context.project();
      QThread::msleep(10);
    }
    s_completed = true;
    return QVariantMap();
  }
};

class PersistentWorkerAlgorithm : public QgsProcessingAlgorithm
{
public:
  static inline std::atomic<bool> s_started{false};
  static inline std::atomic<bool> s_completed{false};
  static inline std::atomic<int> s_iterationsAfterDestruction{0};
  static inline std::atomic<bool> s_widgetDestroyed{false};

  QString name() const override { return QStringLiteral("persistent_worker"); }
  QString displayName() const override { return QStringLiteral("Persistent Worker Algorithm"); }
  QString group() const override { return QStringLiteral("Test"); }
  QString groupId() const override { return QStringLiteral("test"); }
  PersistentWorkerAlgorithm *createInstance() const override { return new PersistentWorkerAlgorithm(); }
  Qgis::ProcessingAlgorithmFlags flags() const override
  {
    return QgsProcessingAlgorithm::flags() | Qgis::ProcessingAlgorithmFlag::CanCancel;
  }

  void initAlgorithm(const QVariantMap &) override {}

  QVariantMap processAlgorithm(const QVariantMap &parameters, QgsProcessingContext &context,
                               QgsProcessingFeedback *feedback) override
  {
    Q_UNUSED(parameters);
    s_started = true;
    s_completed = false;
    s_iterationsAfterDestruction = 0;

    // 此 Worker 故意在收到 cancel 时不立即退出（模拟耗时计算步骤），
    // 持续访问 context 与 feedback
    for (int i = 0; i < 35; ++i)
    {
      if (s_widgetDestroyed.load())
      {
        s_iterationsAfterDestruction++;
      }
      if (feedback)
      {
        feedback->setProgress(i * 2.8);
        feedback->pushInfo(QStringLiteral("Persistent step %1").arg(i));
      }
      (void)context.project();
      (void)context.transformContext();
      QThread::msleep(20);
    }
    s_completed = true;
    return QVariantMap();
  }
};

class StoppableTestProvider : public QgsProcessingProvider
{
public:
  QString id() const override { return QStringLiteral("paleotest"); }
  QString name() const override { return QStringLiteral("Paleo Test Provider"); }
  void loadAlgorithms() override
  {
    addAlgorithm(new StoppableTestAlgorithm());
    addAlgorithm(new PersistentWorkerAlgorithm());
  }
};

// Acceptance for the native Processing algorithm-dialog wiring:
//   * the service ctor registers the PaleoProvider, so paleo:* ids resolve;
//   * showAlgorithmDialog() creates the real QgsProcessingAlgorithmWidgetBase
//     (hosted in a top-level dialog via QgsDockableWidgetHelper, forced to
//     non-docked dialog mode — offscreen safe, no exec() loop);
//   * the parameter panel is populated with wrapper-generated widgets;
//   * unknown algorithm ids fail with an error string.
class TestProcDialog : public QObject
{
  Q_OBJECT

private slots:

  void initTestCase()
  {
    QgsApplication::processingRegistry()->addProvider(new StoppableTestProvider());
  }

  void knownAlgorithmCreatesNativeDialog()
  {
    QgisProcessingService svc(nullptr); // ctor registers PaleoProvider

    QString error;
    QVERIFY2(svc.showAlgorithmDialog(
                 QStringLiteral("paleo:paleo_constraint_idw"),
                 QVariantMap{{QStringLiteral("CELL_SIZE"), 2.5}},
                 nullptr, &error),
             qPrintable(error));

    QWidget *w = svc.lastAlgorithmDialog();
    QVERIFY(w);
    auto *algWidget = qobject_cast<QgsProcessingAlgorithmWidgetBase *>(w);
    QVERIFY(algWidget);
    QVERIFY(algWidget->algorithm());
    QCOMPARE(algWidget->algorithm()->id(),
             QStringLiteral("paleo:paleo_constraint_idw"));

    // Parameter panel was built by the registry-backed
    // QgsProcessingParametersWidget port — it carries per-parameter widgets
    // (constraint_idw has INPUT/FIELD/CONSTRAINTS/FACIES_CODE/CELL_SIZE/OUTPUT).
    auto *panel =
        qobject_cast<QgsProcessingParametersWidget *>(algWidget->mainWidget());
    QVERIFY(panel);
    QVERIFY(panel->findChildren<QWidget *>().size() > 5);

    // The dockable helper hosts the widget inside a separate top-level QDialog.
    QVERIFY(w->window() != nullptr);
    QVERIFY(w->window()->isWindow());
    QVERIFY(qobject_cast<QDialog *>(w->window()));

    // Preset params flow through the panel's wrappers without errors:
    // createProcessingParameters() must report the CELL_SIZE override back.
    QCOMPARE(algWidget->createProcessingParameters()
                 .value(QStringLiteral("CELL_SIZE"))
                 .toDouble(),
             2.5);

    // Cleanup: close the hosting dialog and let deferred deletes flush.
    w->window()->close();
    w->deleteLater();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
  }

  void secondAlgorithmCreatesDialog()
  {
    QgisProcessingService svc(nullptr);

    QString error;
    QVERIFY2(svc.showAlgorithmDialog(
                 QStringLiteral("paleo:paleo_geological_smoothing"),
                 QVariantMap{{QStringLiteral("PASSES"), 2}}, nullptr, &error),
             qPrintable(error));
    auto *w = svc.lastAlgorithmDialog();
    QVERIFY(w);
    auto *algWidget = qobject_cast<QgsProcessingAlgorithmWidgetBase *>(w);
    QVERIFY(algWidget->algorithm());
    QCOMPARE(algWidget->algorithm()->id(),
             QStringLiteral("paleo:paleo_geological_smoothing"));
    QCOMPARE(algWidget->createProcessingParameters()
                 .value(QStringLiteral("PASSES"))
                 .toInt(),
             2);

    w->window()->close();
    w->deleteLater();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
  }

  void unknownAlgorithmFails()
  {
    QgisProcessingService svc(nullptr);

    QString error;
    QVERIFY(!svc.showAlgorithmDialog(QStringLiteral("paleo:does_not_exist"),
                                     QVariantMap(), nullptr, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(error.contains(QStringLiteral("paleo:does_not_exist")));
    QVERIFY(svc.lastAlgorithmDialog() == nullptr);
  }

  void nonPaleoAlgorithmFails()
  {
    QgisProcessingService svc(nullptr);
    QString error;
    QVERIFY(!svc.showAlgorithmDialog(QStringLiteral("qgis:buffer"), QVariantMap(),
                                     nullptr, &error));
    QVERIFY(!error.isEmpty());
  }

  void closeEventCancelsRunningAlgorithmGracefully()
  {
    QgisProcessingService svc(nullptr);
    QString error;
    QVERIFY2(svc.showAlgorithmDialog(
                 QStringLiteral("paleotest:stoppable_test"),
                 QVariantMap(), nullptr, &error),
             qPrintable(error));

    QWidget *w = svc.lastAlgorithmDialog();
    QVERIFY(w);
    auto *algWidget = qobject_cast<QgsProcessingAlgorithmWidgetBase *>(w);
    QVERIFY(algWidget);

    StoppableTestAlgorithm::s_started = false;
    StoppableTestAlgorithm::s_completed = false;
    StoppableTestAlgorithm::s_sawCancel = false;

    QVERIFY(algWidget->runButton());
    algWidget->runButton()->click();

    QTRY_VERIFY_WITH_TIMEOUT(StoppableTestAlgorithm::s_started.load(), 3000);
    QVERIFY(algWidget->isRunning());

    // Close window while running
    w->window()->close();

    // Cancellation should be triggered and worker should observe cancel
    QTRY_VERIFY_WITH_TIMEOUT(StoppableTestAlgorithm::s_completed.load(), 4000);
    QVERIFY(StoppableTestAlgorithm::s_sawCancel.load());

    // Window should complete its delayed close once worker finishes
    QTRY_VERIFY_WITH_TIMEOUT(!algWidget->isRunning(), 3000);

    w->deleteLater();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
  }

  void widgetDestructionDuringActiveExecutionDoesNotCrashOrUaf()
  {
    QgisProcessingService svc(nullptr);
    QString error;
    QWidget *w = svc.createAlgorithmDialog(
        QStringLiteral("paleotest:stoppable_test"),
        QVariantMap(), nullptr, &error);
    QVERIFY2(w, qPrintable(error));

    auto *algWidget = qobject_cast<QgsProcessingAlgorithmWidgetBase *>(w);
    QVERIFY(algWidget);

    StoppableTestAlgorithm::s_started = false;
    StoppableTestAlgorithm::s_completed = false;
    StoppableTestAlgorithm::s_sawCancel = false;

    QVERIFY(algWidget->runButton());
    algWidget->runButton()->click();

    // Wait until background task is actively running and executing processAlgorithm()
    QTRY_VERIFY_WITH_TIMEOUT(StoppableTestAlgorithm::s_started.load(), 3000);
    QVERIFY(algWidget->isRunning());

    // Destroy the widget immediately while the worker thread is in the middle of
    // loop iterations accessing context and calling feedback->setProgress()/pushInfo()
    QPointer<QWidget> topWindow = w->window();
    delete w;
    if (topWindow)
      delete topWindow;

    // With TaskPayload and proper lifetime decoupling, the background task safely
    // finishes without heap-use-after-free, double-free, or crash.
    QTRY_VERIFY_WITH_TIMEOUT(StoppableTestAlgorithm::s_completed.load(), 5000);

    QCoreApplication::processEvents();
  }

  // ---- Challenger 2 Adversarial Stress Test: Persistent Worker Outliving UI Destruction ----
  void adversarialPersistentWorkerDestructionSafety()
  {
    QgisProcessingService svc(nullptr);
    QString error;
    QWidget *w = svc.createAlgorithmDialog(
        QStringLiteral("paleotest:persistent_worker"),
        QVariantMap(), nullptr, &error);
    QVERIFY2(w, qPrintable(error));

    auto *algWidget = qobject_cast<QgsProcessingAlgorithmWidgetBase *>(w);
    QVERIFY(algWidget);

    PersistentWorkerAlgorithm::s_started = false;
    PersistentWorkerAlgorithm::s_completed = false;
    PersistentWorkerAlgorithm::s_widgetDestroyed = false;

    QVERIFY(algWidget->runButton());
    algWidget->runButton()->click();

    // 等待后台 Worker 已经开始并正在执行循环
    QTRY_VERIFY_WITH_TIMEOUT(PersistentWorkerAlgorithm::s_started.load(), 3000);
    QVERIFY(algWidget->isRunning());

    // 标记 Widget 即将被销毁
    PersistentWorkerAlgorithm::s_widgetDestroyed = true;

    // 立即直接销毁 Widget 及顶层窗口
    QPointer<QWidget> topWindow = w->window();
    delete w;
    if (topWindow)
      delete topWindow;

    // 断言：由于 TaskPayload 安全绑定生命周期，即使 UI 控件已销毁，
    // 后台持续访问 context/feedback 的 Worker 也绝不发生野指针/崩溃，顺利执行完成
    QTRY_VERIFY_WITH_TIMEOUT(PersistentWorkerAlgorithm::s_completed.load(), 5000);
    QVERIFY(PersistentWorkerAlgorithm::s_iterationsAfterDestruction.load() > 0);

    QCoreApplication::processEvents();
  }

  // ---- Challenger 2 Adversarial Stress Test: Rapid Create-Run-Destroy Churn Under Concurrency ----
  void adversarialRapidDialogCreateRunDestroyUnderStress()
  {
    QgisProcessingService svc(nullptr);
    QString error;

    for (int iter = 0; iter < 5; ++iter)
    {
      QWidget *w = svc.createAlgorithmDialog(
          QStringLiteral("paleotest:stoppable_test"),
          QVariantMap(), nullptr, &error);
      QVERIFY2(w, qPrintable(error));

      auto *algWidget = qobject_cast<QgsProcessingAlgorithmWidgetBase *>(w);
      QVERIFY(algWidget);

      QVERIFY(algWidget->runButton());
      algWidget->runButton()->click();

      // 在 Worker 刚刚启动或调度时立即销毁窗口，极限测试 TaskPayload 绑定与断开时序
      QThread::msleep(15);

      QPointer<QWidget> topWindow = w->window();
      delete w;
      if (topWindow)
        delete topWindow;

      QCoreApplication::processEvents();
    }

    // 确认所有后台 Task 安全退出，无崩溃与悬挂
    QTest::qWait(500);
    QCoreApplication::processEvents();
  }

  // (P1-13 / RUNTIME-02): verify that destroying dialog disconnects task signals safely
  void widgetDestructionDisconnectsTaskSignalsCleanly()
  {
    QgisProcessingService svc(nullptr);
    QString error;
    QWidget *w = svc.createAlgorithmDialog(
        QStringLiteral("paleotest:persistent_worker"),
        QVariantMap(), nullptr, &error);
    QVERIFY2(w, qPrintable(error));

    auto *algWidget = qobject_cast<QgsProcessingAlgorithmWidgetBase *>(w);
    QVERIFY(algWidget);

    PersistentWorkerAlgorithm::s_started = false;
    PersistentWorkerAlgorithm::s_completed = false;

    QVERIFY(algWidget->runButton());
    algWidget->runButton()->click();

    QTRY_VERIFY_WITH_TIMEOUT(PersistentWorkerAlgorithm::s_started.load(), 3000);
    QVERIFY(algWidget->isRunning());

    // Destroy the widget while algorithm is actively running
    delete w;

    // After widget deletion, processing events must not trigger any callbacks or crashes
    QCoreApplication::processEvents();
    QTRY_VERIFY_WITH_TIMEOUT(PersistentWorkerAlgorithm::s_completed.load(), 5000);
    QCoreApplication::processEvents();
  }
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");

  // 每运行一次的临时目录（对齐 tst_seismic_sectionui 惯例）：既隔离直跑时
  // 的真实用户配置，也消除固定 /tmp 路径跨运行/跨用户的陈旧状态向量
  // （ctest 路径另有 add_paleo_test 的 XDG/HOME 沙箱兜底）。
  static QTemporaryDir settingsDir;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());

  QgsApplication app(argc, argv, true); // GUI-enabled: dialog widgets required
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true); // distro install
  app.initQgis();

  TestProcDialog tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QCoreApplication::processEvents(); // flush deferred dialog/widget deletes
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_procdialog.moc"
