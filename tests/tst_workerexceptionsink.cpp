#include <QtTest>

#include <stdexcept>
#include <thread>
#include <vector>

#include "../src/services/workerexceptionsink.h"

// PW-014：std::thread 分片抛异常须转运到调用线程，而不是 std::terminate。
class TestWorkerExceptionSink : public QObject
{
  Q_OBJECT
private slots:
  void workerExceptionIsRethrownAfterJoin()
  {
    WorkerExceptionSink sink;
    auto body = [](int x) {
      if (x == 2)
        throw std::runtime_error("shard 2 failed");
    };
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t)
      workers.emplace_back(sink.wrap(body), t);
    for (auto &w : workers)
      w.join();
    QVERIFY(sink.failed());
    bool caught = false;
    try {
      sink.rethrowIfAny();
    } catch (const std::runtime_error &e) {
      caught = QByteArray(e.what()) == "shard 2 failed";
    }
    QVERIFY(caught);
  }

  void noExceptionIsNoop()
  {
    WorkerExceptionSink sink;
    std::thread t(sink.wrap([](int) {}), 1);
    t.join();
    QVERIFY(!sink.failed());
    sink.rethrowIfAny(); // 不抛
  }
};

QTEST_APPLESS_MAIN(TestWorkerExceptionSink)
#include "tst_workerexceptionsink.moc"
