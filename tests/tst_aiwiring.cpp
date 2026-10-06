// 层：测试壳
#include <QtTest>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <functional>
#include <QObject>
#include <QUrl>

#include "../src/ai/remotepredictconfig.h"
#include "../src/ai/remotepredictrouter.h"
#include "../src/app/aiwiring.h"

// 方向51 Oracle①：产品装配（app/aiwiring.cpp）与测试走同一个装配函数，
// 因此「装配出来的是 router 而不是替身 Mock」是可执行断言。
//
// 为什么不是 tst_mappingworkbench：那份是伞式注册（paleo_core），在本机以
// 0xc0000139 死于 QTest 之前（master 上 tst_ui/tst_mappingworkbench 同样红，
// 与本方向无关；见 ledger 的「宿主红项」条）。这里用 paleo_app 的最小链接集
// 单独覆盖装配语义，MappingWorkbench 侧的两个用例仍留在 tst_mappingworkbench。
class TestAiWiring : public QObject {
  Q_OBJECT
private slots:
  void unconfiguredEndpointStillYieldsRouterWithoutTransport();
  void configuredEndpointBindsHttpTransport();
  void routerWithoutTransportFailsHonestlyNotSilently();
  void statusHintIsHonestInEveryMode();

private:
  static bool spinUntil(const std::function<bool()> &done, int timeoutMs = 10000) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < timeoutMs)
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
  }
};

void TestAiWiring::unconfiguredEndpointStillYieldsRouterWithoutTransport() {
  QObject owner;
  const RemotePredictionAssembly assembled =
    assembleRemotePrediction(RemotePredictConfig::fromParts(QUrl(), true,
                                                             QString()),
                             nullptr, QString(), &owner);
  QVERIFY(assembled.router);
  QVERIFY(assembled.service == assembled.router);
  QVERIFY(!assembled.transportBound);
  // 关键断言：产品装配面出来的是 router，className 不会是替身。
  QCOMPARE(QString(assembled.service->metaObject()->className()),
           QStringLiteral("RemotePredictionRouter"));
  QVERIFY2(assembled.statusHint.contains(QStringLiteral("未配置")),
           qPrintable(assembled.statusHint));
}

void TestAiWiring::configuredEndpointBindsHttpTransport() {
  QObject owner;
  const RemotePredictionAssembly assembled = assembleRemotePrediction(
    RemotePredictConfig::fromParts(
      QUrl(QStringLiteral("http://127.0.0.1:65501")), true, QString()),
    nullptr, QString(), &owner);
  QVERIFY(assembled.transportBound);
  QCOMPARE(QString(assembled.service->metaObject()->className()),
           QStringLiteral("RemotePredictionRouter"));
  QVERIFY(!assembled.statusHint.contains(QStringLiteral("未配置")));
  // 禁用开关：地址在但 enabled=false → 不挂传输，状态明说已禁用。
  const RemotePredictionAssembly disabled = assembleRemotePrediction(
    RemotePredictConfig::fromParts(
      QUrl(QStringLiteral("https://ai.example.com")), false, QString()),
    nullptr, QString(), &owner);
  QVERIFY(!disabled.transportBound);
  QVERIFY2(disabled.statusHint.contains(QStringLiteral("禁用")),
           qPrintable(disabled.statusHint));
  // 非法地址（明文 http 非 loopback）：同样不挂传输，状态给出原因。
  const RemotePredictionAssembly insecure = assembleRemotePrediction(
    RemotePredictConfig::fromParts(
      QUrl(QStringLiteral("http://ai.example.com")), true, QString()),
    nullptr, QString(), &owner);
  QVERIFY(!insecure.transportBound);
  QVERIFY2(insecure.statusHint.contains(QStringLiteral("https://")),
           qPrintable(insecure.statusHint));
}

void TestAiWiring::routerWithoutTransportFailsHonestlyNotSilently() {
  QObject owner;
  const RemotePredictionAssembly assembled =
    assembleRemotePrediction(RemotePredictConfig::fromParts(QUrl(), true,
                                                             QString()),
                             nullptr, QString(), &owner);
  RemotePredictionRequest request;
  request.id = QStringLiteral("wiring-1");
  request.horizon = QStringLiteral("D61");
  request.kind = QStringLiteral("seismic");
  request.facies = {QVariantMap{{QStringLiteral("code"), 1}}};
  bool failedSeen = false;
  bool completedSeen = false;
  QString reason;
  QObject::connect(assembled.router, &RemotePredictionService::failed,
                   &owner,
                   [&](const QString &id, const QString &text) {
                     if (id == request.id) {
                       failedSeen = true;
                       reason = text;
                     }
                   });
  QObject::connect(assembled.router, &RemotePredictionService::completed,
                   &owner, [&](const RemotePredictionResult &) {
                     completedSeen = true;
                   });
  assembled.router->start(request);
  QVERIFY2(spinUntil([&] { return failedSeen || completedSeen; }),
           "未配置远端时必须出终态（失败），不能沉默");
  QVERIFY(!completedSeen); // 绝不能凭空给一份"结果"
  QVERIFY2(reason.contains(QStringLiteral("未绑定远端传输")),
           qPrintable(reason));
}

void TestAiWiring::statusHintIsHonestInEveryMode() {
  const RemotePredictConfig none;
  QVERIFY2(none.statusHint().contains(QStringLiteral("未配置")),
           qPrintable(none.statusHint()));
  QVERIFY(!none.usable());
  const RemotePredictConfig local = RemotePredictConfig::fromParts(
    QUrl(QStringLiteral("http://127.0.0.1:8080")), true, QString());
  QVERIFY(local.usable());
  QVERIFY2(local.statusHint().contains(QStringLiteral("127.0.0.1")),
           qPrintable(local.statusHint()));
}

int main(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);
  TestAiWiring tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_aiwiring.moc"
