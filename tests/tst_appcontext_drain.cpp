// tst_appcontext_drain — review M1 回归：drainTasks 超时（worker 不轮询取消）
// 时 ~AppContext 不得析构服务（worker 闭包持其裸指针）；泄漏优于 UAF。
#include <QtTest>
#include <QPointer>
#include <QSemaphore>
#include <memory>

#include "../src/app/appcontext.h"
#include "../src/services/paleotaskservice.h"

class TestAppContextDrain : public QObject
{
  Q_OBJECT
private slots:
  void stuckWorkerKeepsServicesAlive();
};

void TestAppContextDrain::stuckWorkerKeepsServicesAlive()
{
  auto *ctx = new AppContext(QStringLiteral("/usr"));
  QVERIFY2(ctx->ready(), "AppContext failed to bring up QgisRuntime");
  QPointer<PaleoTaskService> svc = ctx->taskSvc();
  QVERIFY(svc);
  auto gate = std::make_shared<QSemaphore>(0);
  auto entered = std::make_shared<QSemaphore>(0);
  svc->start(QStringLiteral("uncooperative"), [gate, entered](PaleoTask *) {
    entered->release();
    gate->acquire(); // 模拟不轮询取消的长 GDAL 调用
    return QString();
  });
  QVERIFY(entered->tryAcquire(1, 5000));
  QThreadPool *pool = svc->workerPool();
  delete ctx; // drainTasks 10 s 超时 → 必须不拆除服务
  QVERIFY2(svc, "task service destroyed under a running worker (UAF)");
  QCOMPARE(pool->activeThreadCount(), 1);
  gate->release();
  QVERIFY(pool->waitForDone(5000));
}

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");
  TestAppContextDrain tc;
  return QTest::qExec(&tc, argc, argv);
}
#include "tst_appcontext_drain.moc"
