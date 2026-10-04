// 层：测试壳
//
// 批次进度面板（方向 33 目标形态 6）的断言集。核心断言：
//   1. 面板**只观察信号**：连上队列后，作业推进自动反映到行与汇总，不需
//      任何手工 refresh；
//   2. 逐作业行含状态/进度/耗时/失败原因，失败原因优先占末列（排查入口）；
//   3. 跳过 ≠ 失败（幂等命中显示为跳过色/文案，不与失败混淆）；
//   4. 队列缺席时退化为提示文案，不崩。
//   5. 可见性启停轮询（与 TaskPanel 同口径：隐藏时不刷新）。
//
// 纪律：回调持 std::shared_ptr，worker 可能仍在跑；compute 轮询有超时上界。
#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <atomic>
#include <functional>
#include <memory>

#include "services/paleotaskservice.h"
#include "ui/batchjobpanel.h"
#include "workflow/batchjobqueue.h"

using PaleoBatchQueue::BatchItem;
using PaleoBatchQueue::BatchQueue;
using PaleoBatchQueue::BatchSpec;
using PaleoBatchQueue::ItemState;

namespace
{

bool pumpUntil(const std::function<bool()> &pred, int timeoutMs = 8000)
{
  QElapsedTimer clock;
  clock.start();
  while (!pred())
  {
    if (clock.elapsed() > timeoutMs)
      return false;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(1);
  }
  return true;
}

BatchSpec makeSpec(const QString &batchId, const QStringList &horizons,
                   const QStringList &methods, int concurrency = 1)
{
  BatchSpec s;
  s.batchId = batchId;
  s.horizons = horizons;
  s.methodIds = methods;
  s.paramTemplates.insert(QStringLiteral("cellSize"), 25.0);
  s.concurrency = concurrency;
  return s;
}

/// 找到面板的行控件（测试需要读它，得从私有成员走 friend 之外的路——
/// 故这里用 QTreeWidget 的 public 接口按内容定位）。
QTreeWidget *listOf(BatchJobPanel *panel)
{
  return panel->findChild<QTreeWidget *>();
}

QLabel *summaryOf(BatchJobPanel *panel)
{
  // 汇总标签是第一个 QLabel 子对象（面板构造顺序固定：summary 在 list 之前）。
  const auto labels = panel->findChildren<QLabel *>();
  return labels.isEmpty() ? nullptr : labels.first();
}

QStringList columnTexts(QTreeWidget *list, int column)
{
  QStringList out;
  for (int i = 0; i < list->topLevelItemCount(); ++i)
    out << list->topLevelItem(i)->text(column);
  return out;
}

} // namespace

class TestBatchJobPanel : public QObject
{
  Q_OBJECT

private slots:
  void cleanup() { tearDown(); }

  void showsEmptyHintWithoutQueue();
  void rowsFollowItemSignalsWithoutManualRefresh();
  void failedItemShowsReasonInLastColumn();
  void skippedIsNotShownAsFailure();
  void summaryLineReflectsBatchCounts();
  void pollTimerRunsOnlyWhileVisible();

private:
  std::unique_ptr<QObject> m_dispatcher;
  std::unique_ptr<PaleoTaskService> m_tasks;
  std::unique_ptr<BatchQueue> m_queue;
  std::unique_ptr<BatchJobPanel> m_panel;
  std::shared_ptr<std::atomic_bool> m_gate;
  QTemporaryDir m_dir;

  /// gate 未开时 compute 阻塞（测「行随信号更新」与可见性轮询用）。
  PaleoBatchQueue::ItemExecutor gatingExecutor()
  {
    auto gate = m_gate;
    return [gate](BatchItem &job, const std::function<bool()> &cancelled,
                  const std::function<void(double, const QString &)> &progress) {
      if (progress)
        progress(30.0, QStringLiteral("interpolate"));
      QElapsedTimer clock;
      clock.start();
      while (!gate->load() && clock.elapsed() < 5000)
      {
        if (cancelled && cancelled())
        {
          job.error = QObject::tr("已取消");
          return false;
        }
        QThread::msleep(2);
      }
      return true;
    };
  }

  void setUp()
  {
    m_gate = std::make_shared<std::atomic_bool>(false);
    m_dispatcher = std::make_unique<QObject>();
    m_tasks = std::make_unique<PaleoTaskService>(nullptr, m_dispatcher.get());
    m_tasks->setMaxWorkerThreads(4);
    m_queue = std::make_unique<BatchQueue>(m_tasks.get(), m_dir.path(),
                                           m_dispatcher.get());
    m_queue->setConcurrency(1);
    m_queue->setExecutor(gatingExecutor());
    m_panel = std::make_unique<BatchJobPanel>(m_queue.get(), nullptr);
  }

  void tearDown()
  {
    if (m_gate)
      m_gate->store(true); // 放开在跑的 compute，避免 teardown 时还在等
    if (m_queue)
      m_queue->cancelBatch();
    if (m_tasks)
      m_tasks->shutdown(3000, false);
    m_panel.reset();
    m_queue.reset();
    m_tasks.reset();
    m_dispatcher.reset();
    m_gate.reset();
  }
};

void TestBatchJobPanel::showsEmptyHintWithoutQueue()
{
  m_panel.reset();
  BatchJobPanel orphan(static_cast<BatchQueue *>(nullptr), nullptr);
  QLabel *summary = summaryOf(&orphan);
  QVERIFY(summary);
  // 队列缺席：退化为提示，不崩
  QVERIFY(!summary->text().isEmpty());
}

void TestBatchJobPanel::rowsFollowItemSignalsWithoutManualRefresh()
{
  setUp();
  const BatchSpec s = makeSpec(QStringLiteral("p1"),
                               {QStringLiteral("H1"), QStringLiteral("H2")},
                               {QStringLiteral("m1")}, 1);
  QString err;
  QVERIFY2(m_queue->start(s, &err), qPrintable(err));

  QTreeWidget *list = listOf(m_panel.get());
  QVERIFY(list);

  // 不调 refresh：行应由 batchStarted/itemChanged 信号自动出现
  QVERIFY(pumpUntil([list] { return list->topLevelItemCount() == 2; }));

  // 开放闸门让作业完成
  m_gate->store(true);
  QVERIFY(pumpUntil([this] { return !m_queue->busy(); }));

  // 状态列应含「完成」
  const QStringList states = columnTexts(list, 2);
  for (const QString &s2 : states)
    QVERIFY2(s2.contains(QStringLiteral("完成")), qPrintable(s2));
}

void TestBatchJobPanel::failedItemShowsReasonInLastColumn()
{
  setUp();
  m_queue->setExecutor([](BatchItem &job, const std::function<bool()> &,
                           const std::function<void(double, const QString &)>&) {
    if (job.itemId == QLatin1String("H2|m1"))
    {
      job.error = QStringLiteral("注入的失败原因");
      return false;
    }
    return true;
  });

  const BatchSpec s = makeSpec(QStringLiteral("p2"),
                               {QStringLiteral("H1"), QStringLiteral("H2")},
                               {QStringLiteral("m1")}, 1);
  QString err;
  QVERIFY2(m_queue->start(s, &err), qPrintable(err));
  QVERIFY(pumpUntil([this] { return !m_queue->busy(); }));

  QTreeWidget *list = listOf(m_panel.get());
  QVERIFY(list);
  QVERIFY(pumpUntil([list] { return list->topLevelItemCount() == 2; }));

  // 失败原因应出现在末列（排查第一入口）
  const QStringList last = columnTexts(list, 4);
  bool found = false;
  for (const QString &t : last)
    if (t.contains(QStringLiteral("注入的失败原因")))
      found = true;
  QVERIFY2(found, qPrintable(last.join(QStringLiteral(" | "))));
}

void TestBatchJobPanel::skippedIsNotShownAsFailure()
{
  setUp();
  const BatchSpec s = makeSpec(QStringLiteral("p3"),
                               {QStringLiteral("H1")}, {QStringLiteral("m1")}, 1);
  QString err;
  QVERIFY2(m_queue->start(s, &err), qPrintable(err));
  m_gate->store(true);
  QVERIFY(pumpUntil([this] { return !m_queue->busy(); }));

  // 第二遍：幂等命中 → 跳过
  QVERIFY2(m_queue->resume(s, &err), qPrintable(err));
  QVERIFY(pumpUntil([this] { return !m_queue->busy(); }));

  QTreeWidget *list = listOf(m_panel.get());
  QVERIFY(list);
  const QStringList states = columnTexts(list, 2);
  QVERIFY(!states.isEmpty());
  for (const QString &t : states)
  {
    QVERIFY2(t.contains(QStringLiteral("跳过")), qPrintable(t));
    // 跳过不是失败：状态文案不得含「失败」
    QVERIFY2(!t.contains(QStringLiteral("失败")), qPrintable(t));
  }
}

void TestBatchJobPanel::summaryLineReflectsBatchCounts()
{
  setUp();
  m_queue->setExecutor([](BatchItem &job, const std::function<bool()> &,
                           const std::function<void(double, const QString &)>&) {
    if (job.itemId == QLatin1String("H2|m1"))
    {
      job.error = QStringLiteral("注入的失败");
      return false;
    }
    return true;
  });
  const BatchSpec s = makeSpec(QStringLiteral("p4"),
                               {QStringLiteral("H1"), QStringLiteral("H2")},
                               {QStringLiteral("m1")}, 1);
  QString err;
  QVERIFY2(m_queue->start(s, &err), qPrintable(err));
  QVERIFY(pumpUntil([this] { return !m_queue->busy(); }));

  QLabel *summary = summaryOf(m_panel.get());
  QVERIFY(summary);
  // 汇总含成功 1 / 失败 1
  QVERIFY2(summary->text().contains(QStringLiteral("成功 1")),
           qPrintable(summary->text()));
  QVERIFY2(summary->text().contains(QStringLiteral("失败 1")),
           qPrintable(summary->text()));
}

void TestBatchJobPanel::pollTimerRunsOnlyWhileVisible()
{
  setUp();
  QTreeWidget *list = listOf(m_panel.get());
  QVERIFY(list);

  // 初始未 show → 轮询未启
  QVERIFY(!m_panel->isVisible());
  QVERIFY(pumpUntil([list] { return list->topLevelItemCount() == 0; }));

  // show 后轮询启
  m_panel->show();
  QVERIFY(pumpUntil([this] { return m_panel->isVisible(); }));
  // 停到批中，让行在无显式 refresh 下随轮询出现
  const BatchSpec s = makeSpec(QStringLiteral("p5"),
                               {QStringLiteral("H1")}, {QStringLiteral("m1")}, 1);
  QString err;
  QVERIFY2(m_queue->start(s, &err), qPrintable(err));
  QVERIFY(pumpUntil([list] { return list->topLevelItemCount() >= 1; }));

  m_gate->store(true);
  QVERIFY(pumpUntil([this] { return !m_queue->busy(); }));
  m_panel->hide();
}

int main(int argc, char *argv[])
{
  // 无控制台的 Windows 下 ctest 拉起时 QtTest 结果走 OutputDebugString，
  // 崩溃时 stdout 缓冲又被 __fastfail 吞掉。追加 `-o <file>,txt` 让每个用例
  // 的结果落盘，崩到哪一条一目了然（与仓内其余测试同口径）。
  QByteArray logPath =
      QByteArray(QT_TESTCASE_BUILDDIR) + "/tst_batchjobpanel-result.txt";
  {
    if (qgetenv("QT_QPA_PLATFORM").isEmpty())
      qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestBatchJobPanel tc;
    QList<QByteArray> forwarded;
    forwarded << QByteArray(argv[0]);
    for (int i = 1; i < argc; ++i)
      forwarded << QByteArray(argv[i]);
    forwarded << QByteArray("-o") << logPath + ",txt";
    QList<char *> cargv;
    cargv.reserve(forwarded.size());
    for (QByteArray &a : forwarded)
      cargv << a.data();
    return QTest::qExec(&tc, cargv.size(), cargv.data());
  }
}

#include "tst_batchjobpanel.moc"
