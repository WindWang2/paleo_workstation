// 层：测试壳
#include <QtTest>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <functional>
#include <QObject>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>

#include "../src/ai/chat/chatmessage.h"
#include "../src/ai/remotepredictconfig.h"
#include "../src/ai/remotepredictrouter.h"
#include "../src/app/aiwiring.h"
#include "../src/catalog/datacatalog.h"
#include "../src/workflow/aichatcontroller.h"
#include "../src/workflow/aichattoolrunner.h"
#include "fixtures/mkprojectfixture.h" // 方向95：mini 夹具扩散（重绑段）

// 方向51 Oracle①：产品装配（app/aiwiring.cpp）与测试走同一个装配函数，
// 因此「装配出来的是 router 而不是替身 Mock」是可执行断言。
//
// 为什么不是 tst_mappingworkbench：那份是伞式注册（paleo_core），在本机以
// 0xc0000139 死于 QTest 之前（master 上 tst_ui/tst_mappingworkbench 同样红，
// 与本方向无关；见 ledger 的「宿主红项」）。这里用 paleo_app 的最小链接集
// 单独覆盖装配语义，MappingWorkbench 侧的两个用例仍留在 tst_mappingworkbench。
//
// 方向77：bindChatToolRunner 的 catalog 注入断言（行为面——跑一只读工具
// 验证绑定生效；assist/layers 传空 = 无 ORT 构建同路径可用）。
class TestAiWiring : public QObject {
  Q_OBJECT
private slots:
  void unconfiguredEndpointStillYieldsRouterWithoutTransport();
  void configuredEndpointBindsHttpTransport();
  void routerWithoutTransportFailsHonestlyNotSilently();
  void statusHintIsHonestInEveryMode();
  void chatToolCatalogBindsWithoutAssistWorkflow();
  void chatToolCatalogRebindsOnProjectSwitch();
  void chatToolRebindsOntoMkprojectFixtureProject();

private:
  static bool spinUntil(const std::function<bool()> &done, int timeoutMs = 10000) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < timeoutMs)
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
  }
  // 经 chat->toolRunner() 跑一次 paleo.query_project（topic 参数化），取回首帧
  // 结果 payload；成功位随返回值透出。
  static bool queryProject(AiChatController &chat, const QString &topic,
                           QJsonObject *payload);
  // 兼容旧面：topic=wells（既有两槽的断言路径零改动）。
  static bool queryWells(AiChatController &chat, QJsonObject *payload);
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

bool TestAiWiring::queryProject(AiChatController &chat, const QString &topic,
                                QJsonObject *payload) {
  AiChatToolRunner *runner = chat.toolRunner();
  QSignalSpy results(runner, &AiChatToolRunner::toolFinished);
  ChatToolCall call;
  call.id = QStringLiteral("wiring-query");
  call.name = QStringLiteral("paleo.query_project");
  call.argumentsJson = QStringLiteral("{\"topic\":\"%1\"}").arg(topic);
  runner->run(call);
  if (!spinUntil([&results] { return results.size() >= 1; }))
    return false;
  *payload = QJsonDocument::fromJson(results.at(0).at(2).toString().toUtf8())
               .object();
  return results.at(0).at(1).toBool();
}

bool TestAiWiring::queryWells(AiChatController &chat, QJsonObject *payload) {
  return queryProject(chat, QStringLiteral("wells"), payload);
}

// 方向77：catalog 经装配根注入 chat 工具上下文——assist 为空（无 ORT 构建
// 的等价路径）时只读工具照样可用；tile/horizon 维持执行期如实报错。
void TestAiWiring::chatToolCatalogBindsWithoutAssistWorkflow() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog catalog;
  QVERIFY(catalog.open(dir.path()));
  CatalogEntity well;
  well.id = QStringLiteral("well-a");
  well.entityType = QStringLiteral("well");
  well.name = QStringLiteral("WA-1");
  QVERIFY(catalog.addEntity(well));

  AiChatController chat;
  bindChatToolRunner(&chat, nullptr, nullptr, &catalog, dir.path());
  // 装配出来的 system prompt 带工程概况常驻段（摘要注入走同一绑定）。
  QVERIFY2(chat.systemPrompt().contains(QStringLiteral("当前工程概况")),
           qPrintable(chat.systemPrompt()));
  QJsonObject payload;
  QVERIFY2(queryWells(chat, &payload),
           qPrintable(payload.value(QStringLiteral("error")).toString()));
  QCOMPARE(payload.value(QStringLiteral("count")).toInt(), 1);
  QVERIFY(payload.value(QStringLiteral("wells")).toArray().at(0).toObject()
            .value(QStringLiteral("name")).toString() == QStringLiteral("WA-1"));
}

// 方向77 Oracle③：换工程 → 重绑后工具看到的是新 catalog（井名实证）。
void TestAiWiring::chatToolCatalogRebindsOnProjectSwitch() {
  QTemporaryDir dirA, dirB;
  QVERIFY(dirA.isValid() && dirB.isValid());
  DataCatalog catalogA, catalogB;
  QVERIFY(catalogA.open(dirA.path()));
  QVERIFY(catalogB.open(dirB.path()));
  CatalogEntity a;
  a.id = QStringLiteral("well-a");
  a.entityType = QStringLiteral("well");
  a.name = QStringLiteral("WA-1");
  QVERIFY(catalogA.addEntity(a));
  CatalogEntity b;
  b.id = QStringLiteral("well-b");
  b.entityType = QStringLiteral("well");
  b.name = QStringLiteral("WB-9");
  QVERIFY(catalogB.addEntity(b));
  CatalogEntity b2;
  b2.id = QStringLiteral("well-b2");
  b2.entityType = QStringLiteral("well");
  b2.name = QStringLiteral("WB-10");
  QVERIFY(catalogB.addEntity(b2));

  AiChatController chat;
  bindChatToolRunner(&chat, nullptr, nullptr, &catalogA, dirA.path());
  QJsonObject first;
  QVERIFY2(queryWells(chat, &first),
           qPrintable(first.value(QStringLiteral("error")).toString()));
  QVERIFY(first.value(QStringLiteral("wells")).toArray().at(0).toObject()
            .value(QStringLiteral("name")).toString() == QStringLiteral("WA-1"));

  // 换工程：同一 controller、新 catalog —— 重绑生效（陈旧上下文不得残留）。
  bindChatToolRunner(&chat, nullptr, nullptr, &catalogB, dirB.path());
  QJsonObject second;
  QVERIFY2(queryWells(chat, &second),
           qPrintable(second.value(QStringLiteral("error")).toString()));
  const QJsonArray wellsB = second.value(QStringLiteral("wells")).toArray();
  QStringList namesB;
  for (const QJsonValue &value : wellsB)
    namesB.append(value.toObject().value(QStringLiteral("name")).toString());
  QVERIFY(namesB.contains(QStringLiteral("WB-9")));
  QVERIFY(!namesB.contains(QStringLiteral("WA-1"))); // 旧工程实体不得残留
  QCOMPARE(second.value(QStringLiteral("count")).toInt(), 2);
  // 摘要常驻段同刷：B 工程两井 → 「井 2 口」（A 工程是 1 口，可区分）。
  QVERIFY2(chat.systemPrompt().contains(QStringLiteral("井 2 口")),
           qPrintable(chat.systemPrompt()));
}

// 方向95：换工程重绑吃 mkproject mini 夹具——B 工程从 synthetic 自建换成
// 生产导入全链产物（paleo_mkproject --manifest mini：io 解析 → catalog 登记
// → 工程文件落盘，QProcess 驱动）。重绑后工具上下文快照以生产产物为据：
// wells 井名/summary 实体计数/systemPrompt 摘要常驻段（A 工程 1 井可区分）。
// 上槽 synthetic A→B 语义保留（双轨：fixture 是增广不是替换）。
void TestAiWiring::chatToolRebindsOntoMkprojectFixtureProject() {
  QTemporaryDir dirA;
  QVERIFY(dirA.isValid());
  DataCatalog catalogA;
  QVERIFY(catalogA.open(dirA.path()));
  CatalogEntity a;
  a.id = QStringLiteral("well-a");
  a.entityType = QStringLiteral("well");
  a.name = QStringLiteral("WA-1");
  QVERIFY(catalogA.addEntity(a));

  QTemporaryDir projB;
  QVERIFY(projB.isValid());
  const MkProjectFixture::RunResult runB =
      MkProjectFixture::buildMiniProject(projB.path());
  QVERIFY2(runB.ran && runB.exitCode == 0,
           qPrintable(runB.errorText.isEmpty() ? runB.standardOutput
                                              : runB.errorText));
  DataCatalog catalogB;
  QString openErr;
  QVERIFY2(catalogB.open(runB.projectDir, &openErr), qPrintable(openErr));

  AiChatController chat;
  bindChatToolRunner(&chat, nullptr, nullptr, &catalogA, dirA.path());
  QJsonObject first;
  QVERIFY2(queryWells(chat, &first),
           qPrintable(first.value(QStringLiteral("error")).toString()));
  QCOMPARE(first.value(QStringLiteral("count")).toInt(), 1);

  // 换工程：synthetic A（1 井）→ mini 生产工程 B（3 井）——同一 controller
  // 重绑，工具看到的是生产 catalog 读回面（陈旧上下文不得残留）。
  bindChatToolRunner(&chat, nullptr, nullptr, &catalogB, runB.projectDir);
  QJsonObject wells;
  QVERIFY2(queryWells(chat, &wells),
           qPrintable(wells.value(QStringLiteral("error")).toString()));
  QCOMPARE(wells.value(QStringLiteral("count")).toInt(), 3);
  QStringList namesB;
  for (const QJsonValue &value : wells.value(QStringLiteral("wells")).toArray())
    namesB.append(value.toObject().value(QStringLiteral("name")).toString());
  for (const QString &expected :
       {QStringLiteral("A1"), QStringLiteral("A2"), QStringLiteral("A3")})
    QVERIFY2(namesB.contains(expected),
             qPrintable(namesB.join(QLatin1Char(','))));
  QVERIFY(!namesB.contains(QStringLiteral("WA-1"))); // 旧工程实体不得残留

  // summary 快照：生产工程实体面（manifest expect 同源：well 3 /
  // seismic_survey 1 / auxiliary 2——经工具上下文而非测试直读 catalog）。
  QJsonObject summary;
  QVERIFY2(queryProject(chat, QStringLiteral("summary"), &summary),
           qPrintable(summary.value(QStringLiteral("error")).toString()));
  const QJsonObject entities = summary.value(QStringLiteral("entities")).toObject();
  QCOMPARE(entities.value(QStringLiteral("well")).toInt(), 3);
  QCOMPARE(entities.value(QStringLiteral("seismic_survey")).toInt(), 1);
  QCOMPARE(entities.value(QStringLiteral("auxiliary")).toInt(), 2);

  // 摘要常驻段同刷：B 工程 3 井（A 工程是 1 口，可区分）。
  QVERIFY2(chat.systemPrompt().contains(QStringLiteral("井 3 口")),
           qPrintable(chat.systemPrompt()));
}

int main(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);
  TestAiWiring tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_aiwiring.moc"
