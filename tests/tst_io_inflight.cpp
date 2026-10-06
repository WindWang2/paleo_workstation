#include <QtTest>
#include <QThread>
#include <QtConcurrent>
#include <QFuture>

#include <atomic>
#include <stdexcept>
#include <vector>

#include "../src/io/inflight.h"

class TestIoInflight : public QObject
{
  Q_OBJECT

private slots:
  void singleSubmission();
  void sequentialSubmissionsSameKey();
  void concurrentSubmissionsCoalesced();
  void distinctKeysDoNotCoalesce();
  void exceptionPropagation();
  void mutationDemonstration_executorIdentity();
};

void TestIoInflight::singleSubmission()
{
  InflightCoalescer<QString, int> coalescer;
  QCOMPARE(coalescer.inFlightCount(), 0);

  auto ticket = coalescer.submit(QStringLiteral("task1"), []() {
    return 100;
  });

  QVERIFY(ticket.executor);
  QCOMPARE(ticket.future.get(), 100);
  QCOMPARE(coalescer.inFlightCount(), 0);
}

void TestIoInflight::sequentialSubmissionsSameKey()
{
  InflightCoalescer<QString, QString> coalescer;

  auto t1 = coalescer.submit(QStringLiteral("keyA"), []() {
    return QStringLiteral("res1");
  });
  QVERIFY(t1.executor);
  QCOMPARE(t1.future.get(), QStringLiteral("res1"));

  // 第一次完成后再次提交同一键：不再在途，成为新的执行者
  auto t2 = coalescer.submit(QStringLiteral("keyA"), []() {
    return QStringLiteral("res2");
  });
  QVERIFY(t2.executor);
  QCOMPARE(t2.future.get(), QStringLiteral("res2"));
  QCOMPARE(coalescer.inFlightCount(), 0);
}

void TestIoInflight::concurrentSubmissionsCoalesced()
{
  InflightCoalescer<QString, int> coalescer;
  std::atomic<int> executionCount{0};
  const int numThreads = 6;

  std::vector<InflightCoalescer<QString, int>::Ticket> tickets(numThreads);
  QVector<QFuture<void>> futures;

  for (int i = 0; i < numThreads; ++i)
  {
    futures.append(QtConcurrent::run([&coalescer, &executionCount, &tickets, i]() {
      tickets[i] = coalescer.submit(QStringLiteral("shared_key"), [&executionCount]() {
        ++executionCount;
        QThread::msleep(30); // 维持短暂在途时间使并发发生合并
        return 999;
      });
    }));
  }

  for (auto &f : futures)
  {
    f.waitForFinished();
  }

  // 必须所有并发请求拿到相同的结果
  int executorCount = 0;
  for (int i = 0; i < numThreads; ++i)
  {
    QCOMPARE(tickets[i].future.get(), 999);
    if (tickets[i].executor)
    {
      ++executorCount;
    }
  }

  // 至少合并发生（执行次数远少于总请求数）
  QVERIFY(executionCount.load() >= 1 && executionCount.load() < numThreads);
  QCOMPARE(executorCount, executionCount.load());
  QCOMPARE(coalescer.inFlightCount(), 0);
}

void TestIoInflight::distinctKeysDoNotCoalesce()
{
  InflightCoalescer<QString, int> coalescer;
  auto t1 = coalescer.submit(QStringLiteral("key1"), []() { return 1; });
  auto t2 = coalescer.submit(QStringLiteral("key2"), []() { return 2; });

  QVERIFY(t1.executor);
  QVERIFY(t2.executor);
  QCOMPARE(t1.future.get(), 1);
  QCOMPARE(t2.future.get(), 2);
}

void TestIoInflight::exceptionPropagation()
{
  InflightCoalescer<QString, int> coalescer;
  auto ticket = coalescer.submit(QStringLiteral("failing_key"), []() -> int {
    throw std::runtime_error("simulated failure inside job");
  });

  QVERIFY(ticket.executor);
  QCOMPARE(coalescer.inFlightCount(), 0);

  bool caught = false;
  try
  {
    ticket.future.get();
  }
  catch (const std::runtime_error &e)
  {
    caught = true;
    QVERIFY(QString::fromLatin1(e.what()).contains(QStringLiteral("simulated failure")));
  }
  QVERIFY(caught);
}

void TestIoInflight::mutationDemonstration_executorIdentity()
{
  // 变异测试示范：对于串行单点请求，Ticket.executor 必须恒为 true
  InflightCoalescer<int, int> coalescer;
  for (int k = 0; k < 10; ++k)
  {
    auto ticket = coalescer.submit(k, [k]() { return k * 2; });
    QVERIFY(ticket.executor);
    QCOMPARE(ticket.future.get(), k * 2);
  }
}

QTEST_GUILESS_MAIN(TestIoInflight)
#include "tst_io_inflight.moc"
