// 预测专项 P1：测井相远端服务的 mock-HTTP 合同测试（不连任何真实服务）。
// 补 tst_wellfacies 未覆盖的边：模型清单 schema 过滤、202 无任务 ID、轮询换
// 任务、结果模型不一致、未知状态、错误/进度回显密钥脱敏、重定向不跟随、
// 取消后迟到响应被忽略、取消停止轮询、无密钥不发请求。
#include "ai/wellfaciesservice.h"
#include "domain/welllogfacies.h"
#include "domain/wellfaciesrequest.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <functional>
#include <memory>

using namespace WellComposite;
namespace {
constexpr const char *kKey = "mock-key-0123456789";

struct Reply {
  int status = 200;
  QByteArray payload;
  QByteArray extraHeaders;
  bool hold = false; // 不回应，留待测试手动释放
};

// 每个请求按 handler 脚本应答；记录路径与请求头供断言。
class ScriptServer {
public:
  QTcpServer server;
  QList<QByteArray> paths;
  QList<QByteArray> raw;
  std::function<Reply(const QByteArray &path, const QJsonObject &body)> handler;
  QList<QPointer<QTcpSocket>> held;
  QList<Reply> heldReplies;
  ScriptServer() {
    server.listen(QHostAddress::LocalHost);
    QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
      while (server.hasPendingConnections()) {
        auto *s = server.nextPendingConnection();
        auto buffer = std::make_shared<QByteArray>();
        QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
        QObject::connect(s, &QTcpSocket::readyRead, s, [this, s, buffer] {
          *buffer += s->readAll();
          const auto end = buffer->indexOf("\r\n\r\n");
          if (end < 0) return;
          qsizetype length = 0;
          for (const auto &h : buffer->left(end).split('\n'))
            if (h.toLower().startsWith("content-length:"))
              length = h.mid(15).trimmed().toLongLong();
          if (buffer->size() < end + 4 + length) return;
          const QByteArray request = *buffer;
          buffer->clear();
          raw << request;
          const auto path = request.split(' ').value(1);
          paths << path;
          const auto body = QJsonDocument::fromJson(request.mid(end + 4)).object();
          const Reply reply = handler ? handler(path, body) : Reply{404, "{}"};
          if (reply.hold) {
            held << s;
            heldReplies << reply;
            return;
          }
          send(s, reply);
        });
      }
    });
  }
  static void send(QTcpSocket *s, const Reply &r) {
    s->write("HTTP/1.1 " + QByteArray::number(r.status) +
             " X\r\nContent-Type: application/json\r\nConnection: close\r\n" + r.extraHeaders +
             "Content-Length: " + QByteArray::number(r.payload.size()) + "\r\n\r\n" + r.payload);
    s->disconnectFromHost();
  }
  void release() {
    for (int i = 0; i < held.size(); ++i)
      if (held[i]) send(held[i], heldReplies[i]);
    held.clear();
    heldReplies.clear();
  }
  int count(const QByteArray &suffix) const {
    int n = 0;
    for (const auto &p : paths) n += p.endsWith(suffix) || (suffix.endsWith('/') && p.contains(suffix));
    return n;
  }
  WellFaciesConfig config(const QByteArray &key = kKey) const {
    WellFaciesConfig c;
    c.baseUrl = QUrl(QStringLiteral("http://127.0.0.1:%1/api/v1").arg(server.serverPort()));
    c.apiKey = key;
    return c;
  }
};

QByteArray json(const QJsonObject &o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }

WellFaciesModel model() {
  return {"model-1", QStringLiteral("测试微相"), "v1", QStringLiteral("恩平组"),
          {"GR"}, {QStringLiteral("河口坝")}, 128};
}
ComprehensiveWellData well() {
  ComprehensiveWellData d;
  d.wellName = "W-1";
  d.minDepth = 1000;
  d.maxDepth = 1016;
  CurveData c;
  c.name = "GR";
  c.unit = "API";
  for (int i = 0; i < 128; ++i) {
    c.depths.append(1000 + i * .125f);
    c.values.append(80 + i * .1f);
  }
  d.continuousCurves = {c};
  FormationInterval group;
  group.name = QStringLiteral("恩平组");
  group.unitType = QStringLiteral("组");
  group.topDepth = 1000;
  group.bottomDepth = 1016;
  auto member = group;
  member.name = QStringLiteral("恩平一段");
  member.unitType = QStringLiteral("段");
  d.formationIntervals = {group, member};
  LithologyInterval lith;
  lith.topDepth = 1000;
  lith.bottomDepth = 1016;
  lith.lithoName = QStringLiteral("细砂岩");
  d.lithologyIntervals = {lith};
  return d;
}
QJsonObject completed(const QJsonArray &rows, const QString &modelId = "model-1") {
  QJsonArray p;
  for (const auto &r : rows)
    p.append(QJsonObject{{"wellName", "W-1"},
                         {"depth", r.toObject().value(QStringLiteral("深度"))},
                         {"label", QStringLiteral("河口坝")},
                         {"confidence", .9}});
  return {{"jobId", "job-1"}, {"status", "completed"},
          {"model", QJsonObject{{"id", modelId}, {"name", QStringLiteral("测试微相")}, {"version", "v1"}}},
          {"predictions", p}};
}
QJsonObject accepted(const QString &jobId = "job-1", const QString &message = {}) {
  QJsonObject o{{"status", "predicting"}, {"pollAfterMs", 100}, {"progress", 30}};
  if (!jobId.isNull()) o.insert("jobId", jobId);
  if (!message.isEmpty()) o.insert("latestMessage", message);
  return o;
}
QJsonObject schemaModel(const QString &id, const QJsonArray &curves, int window,
                        const QJsonArray &categorical = {QStringLiteral("段")}) {
  return {{"id", id}, {"name", id}, {"version", "v1"},
          {"inputSchema", QJsonObject{{"formationGroup", QStringLiteral("恩平组")}, {"curves", curves},
                                      {"labels", QJsonArray{QStringLiteral("河口坝")}},
                                      {"categoricalFields", categorical}, {"window", window}}}};
}
} // namespace

class TestWellFaciesRemote : public QObject {
  Q_OBJECT
  QTemporaryDir m_home;

  // 发起一次预测并等待终态；返回 failed 文本（completed 时为空）。
  static QString runPredict(WellFaciesService &svc, int timeoutMs = 5000, QStringList *progress = nullptr) {
    QSignalSpy done(&svc, &WellFaciesService::completed), failed(&svc, &WellFaciesService::failed);
    if (progress)
      QObject::connect(&svc, &WellFaciesService::progress, &svc,
                       [progress](int, const QString &m) { *progress << m; });
    const auto input = prepareWellFaciesInput(well(), model());
    svc.predict(model(), "W-1", input);
    [&] { QTRY_VERIFY_WITH_TIMEOUT(done.size() + failed.size() > 0, timeoutMs); }();
    return failed.isEmpty() ? QString() : failed.first().first().toString();
  }

private slots:
  void initTestCase() { qputenv("XDG_DATA_HOME", m_home.path().toUtf8()); }

  void frozenModelRequiresActualUnitsBeforeNetwork() {
    ScriptServer server;WellFaciesService svc;svc.configure(server.config());
    QSignalSpy failed(&svc,&WellFaciesService::failed);
    auto input=prepareWellFaciesInput(well(),model());
    svc.predictFrozen(model(),"W-1",input,QString(64,'a'),"{}");
    QCOMPARE(failed.size(),1);QCOMPARE(failed.first().first().toString(),"model_feature_units_unavailable");
    QTest::qWait(30);QVERIFY(server.paths.isEmpty());
  }
  void contractInventoryRetainsUnavailableDeclarations() {
    ScriptServer server;server.handler=[](const QByteArray &,const QJsonObject &) {
      auto categorical=schemaModel("unsupported",{"GR"},2,{QStringLiteral("组")});
      auto missing=schemaModel("window-missing",{"GR"},2);auto input=missing.value("inputSchema").toObject();input.remove("window");missing.insert("inputSchema",input);
      auto version=schemaModel("version-missing",{"GR"},2);version.remove("version");
      return Reply{200,json({{"models",QJsonArray{categorical,missing,version}}})};
    };
    WellFaciesService service;service.configure(server.config());QSignalSpy ready(&service,&WellFaciesService::modelsReady);
    service.fetchModelContracts();QTRY_COMPARE(ready.size(),1);
    const auto models=qvariant_cast<QVector<WellFaciesModel>>(ready.front().first());QCOMPARE(models.size(),3);
    QCOMPARE(models[0].contractReadinessCode,"model_categorical_schema_unsupported");QCOMPARE(models[1].window,0);QVERIFY(models[2].version.isEmpty());
    for(const auto &model:models)QVERIFY(!paleo::wellfacies::modelReadiness(model,model.id,model.version).isEmpty());
  }
  void frozenVersionMustBeReturnedExactly_data() {
    QTest::addColumn<QString>("version");
    QTest::newRow("missing")<<QString();QTest::newRow("different")<<QString("v2");
  }
  void frozenVersionMustBeReturnedExactly() {
    QFETCH(QString,version);ScriptServer server;
    server.handler=[version](const QByteArray &,const QJsonObject &body){
      auto response=completed(body.value("wells").toArray().first().toObject().value("rows").toArray());
      auto m=response.value("model").toObject();m.insert("version",version);response.insert("model",m);
      return Reply{200,json(response)};
    };
    WellFaciesService svc;svc.configure(server.config());auto exact=model();exact.curveUnits={{"GR","API"}};
    auto input=prepareWellFaciesInput(well(),exact);QString error;QVERIFY(bindWellFaciesClasses(exact,{},&input,&error));
    const auto body=json({{"modelVersionId",exact.id},{"wells",QJsonArray{QJsonObject{{"wellName","W-1"},{"rows",input.rows}}}}});
    QSignalSpy failed(&svc,&WellFaciesService::failed),acceptedSpy(&svc,&WellFaciesService::jobAccepted);
    svc.predictFrozen(exact,"W-1",input,QString(64,'a'),body);
    QTRY_COMPARE(failed.size(),1);QCOMPARE(failed.first().first().toString(),"remote_model_version_mismatch");
    QCOMPARE(acceptedSpy.size(),1); // Preserve remote identity even when its result must be quarantined.
  }
  void frozenSendRequiresDurableObserverToContinue() {
    ScriptServer server;WellFaciesService svc;svc.configure(server.config());auto exact=model();exact.curveUnits={{"GR","API"}};
    auto input=prepareWellFaciesInput(well(),exact);QString error;QVERIFY(bindWellFaciesClasses(exact,{},&input,&error));
    connect(&svc,&WellFaciesService::requestSending,&svc,[&](const QString &){svc.cancel();});
    svc.predictFrozen(exact,"W-1",input,QString(64,'a'),"{}");
    QTest::qWait(50);QVERIFY(server.paths.isEmpty());
  }
  void frozenJobRecordingFailureStopsPolling() {
    ScriptServer server;server.handler=[](const QByteArray &,const QJsonObject &){return Reply{202,json(accepted("durable-job"))};};
    WellFaciesService svc;svc.configure(server.config());auto exact=model();exact.curveUnits={{"GR","API"}};
    auto input=prepareWellFaciesInput(well(),exact);QString error;QVERIFY(bindWellFaciesClasses(exact,{},&input,&error));
    QSignalSpy acceptedSpy(&svc,&WellFaciesService::jobAccepted);
    connect(&svc,&WellFaciesService::jobAccepted,&svc,[&](const QString &,const QString &){svc.cancel();});
    svc.predictFrozen(exact,"W-1",input,QString(64,'a'),"{}");
    QTRY_COMPARE(acceptedSpy.size(),1);QTest::qWait(180);QCOMPARE(server.paths.size(),1);
  }
  void frozenRecoveryQueriesOnlyOriginalJob() {
    ScriptServer server;server.handler=[](const QByteArray &,const QJsonObject &){return Reply{404,"{}"};};
    WellFaciesService svc;svc.configure(server.config());auto exact=model();exact.curveUnits={{"GR","API"}};
    auto input=prepareWellFaciesInput(well(),exact);QString error;QVERIFY(bindWellFaciesClasses(exact,{},&input,&error));
    QSignalSpy failed(&svc,&WellFaciesService::failed);
    svc.queryFrozen(exact,"W-1",input,QString(64,'a'),"old job/1");
    QTRY_COMPARE(failed.size(),1);QCOMPARE(server.paths.size(),1);
    QVERIFY(server.raw.front().startsWith("GET "));QVERIFY(server.paths.front().contains("old%20job%2F1"));
    QCOMPARE(server.count("/predict"),0);
  }

  void modelListDropsInvalidSchemas() {
    ScriptServer server;
    server.handler = [](const QByteArray &, const QJsonObject &) {
      return Reply{200, json({{"models", QJsonArray{
          schemaModel("ok", {"GR", "RT"}, 64),
          schemaModel("", {"GR"}, 64),                         // 无 id
          schemaModel("zero-window", {"GR"}, 0),               // 窗口非法
          schemaModel("no-curves", {}, 64),                    // 无曲线
          schemaModel("dup-curves", {"GR", "GR"}, 64),         // 曲线重复
          schemaModel("bad-cat", {"GR"}, 64, {QStringLiteral("沉积相")}), // 不支持的类别字段
      }}})};
    };
    WellFaciesService svc;
    svc.configure(server.config());
    QVector<WellFaciesModel> models;
    bool got = false;
    connect(&svc, &WellFaciesService::modelsReady, this,
            [&](const QVector<WellFaciesModel> &list) { models = list; got = true; });
    svc.fetchModels();
    QTRY_VERIFY(got);
    QCOMPARE(models.size(), 1);
    QCOMPARE(models.first().id, QStringLiteral("ok"));
    QCOMPARE(server.paths.size(), 1);
    QVERIFY(server.paths.first().endsWith("/api/v1/models"));
    QVERIFY(server.raw.first().toLower().contains(QByteArray("x-api-key: ") + kKey));
  }

  void modelListMissingArrayFails() {
    ScriptServer server;
    server.handler = [](const QByteArray &, const QJsonObject &) { return Reply{200, json({{"items", 1}})}; };
    WellFaciesService svc;
    svc.configure(server.config());
    bool got = false;
    connect(&svc, &WellFaciesService::modelsReady, this, [&] { got = true; });
    QSignalSpy failed(&svc, &WellFaciesService::failed);
    svc.fetchModels();
    QTRY_COMPARE(failed.size(), 1);
    QVERIFY(!got);
    QVERIFY(failed.first().first().toString().contains("models"));
  }

  void emptyKeySendsNothing() {
    ScriptServer server;
    WellFaciesService svc;
    svc.configure(server.config(""));
    QSignalSpy failed(&svc, &WellFaciesService::failed);
    svc.fetchModels();
    QCOMPARE(failed.size(), 1); // 同步拒绝
    QTest::qWait(100);
    QVERIFY(server.paths.isEmpty());
  }

  void acceptedWithoutJobIdFails() {
    ScriptServer server;
    server.handler = [](const QByteArray &, const QJsonObject &) {
      return Reply{202, json(accepted(QString()))};
    };
    WellFaciesService svc;
    svc.configure(server.config());
    QVERIFY(runPredict(svc).contains(QStringLiteral("任务 ID")));
    QCOMPARE(server.count("/predictions/"), 0); // 不盲目轮询
  }

  void pollSwitchingJobFails() {
    ScriptServer server;
    server.handler = [](const QByteArray &path, const QJsonObject &) {
      return path.endsWith("/predict") ? Reply{202, json(accepted("job-1"))}
                                       : Reply{200, json(accepted("job-OTHER"))};
    };
    WellFaciesService svc;
    svc.configure(server.config());
    QVERIFY(runPredict(svc).contains(QStringLiteral("不同的推理任务")));
    QCOMPARE(server.count("/predictions/job-1"), 1);
  }

  void pollThenCompleteUsesSameJob() {
    ScriptServer server;
    int polls = 0;
    server.handler = [&polls](const QByteArray &path, const QJsonObject &body) {
      static QJsonArray rows;
      if (path.endsWith("/predict")) {
        rows = body.value("wells").toArray().first().toObject().value("rows").toArray();
        return Reply{202, json(accepted("job-1"))};
      }
      return ++polls < 2 ? Reply{200, json(accepted("job-1"))} : Reply{200, json(completed(rows))};
    };
    WellFaciesService svc;
    svc.configure(server.config());
    const QString error = runPredict(svc);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(server.count("/predict"), 1);        // Submission endpoint only; polling is counted separately below.
    QCOMPARE(server.count("/predictions/job-1"), 2);
  }

  void completedWithForeignModelFails() {
    ScriptServer server;
    server.handler = [](const QByteArray &, const QJsonObject &body) {
      return Reply{200, json(completed(body.value("wells").toArray().first().toObject().value("rows").toArray(),
                                       "model-OTHER"))};
    };
    WellFaciesService svc;
    svc.configure(server.config());
    QSignalSpy done(&svc, &WellFaciesService::completed);
    QVERIFY(runPredict(svc).contains(QStringLiteral("不一致")));
    QVERIFY(done.isEmpty());
  }

  void unknownStatusFails() {
    ScriptServer server;
    server.handler = [](const QByteArray &, const QJsonObject &) {
      return Reply{200, json({{"jobId", "job-1"}, {"status", "exploded"}})};
    };
    WellFaciesService svc;
    svc.configure(server.config());
    QVERIFY(runPredict(svc).contains("exploded"));
  }

  void httpErrorRedactsEchoedKey() {
    ScriptServer server;
    server.handler = [](const QByteArray &, const QJsonObject &) {
      return Reply{401, json({{"code", "UNAUTHORIZED"},
                              {"error", QStringLiteral("invalid key %1").arg(kKey)}})};
    };
    WellFaciesService svc;
    svc.configure(server.config());
    const QString error = runPredict(svc);
    QVERIFY(error.contains("401"));
    QVERIFY(!error.contains(kKey));
    QVERIFY(error.contains(QStringLiteral("密钥已隐藏")));
  }

  void progressMessageRedactsEchoedKey() {
    ScriptServer server;
    server.handler = [](const QByteArray &path, const QJsonObject &) {
      return path.endsWith("/predict")
                 ? Reply{202, json(accepted("job-1", QStringLiteral("auth ok for %1").arg(kKey)))}
                 : Reply{200, json({{"jobId", "job-1"}, {"status", "failed"}, {"error", "boom"}})};
    };
    WellFaciesService svc;
    svc.configure(server.config());
    QStringList progress;
    runPredict(svc, 5000, &progress);
    QVERIFY(!progress.isEmpty());
    for (const auto &m : progress)
      QVERIFY2(!m.contains(kKey), "进度消息回显了 API 密钥");
  }

  void redirectIsNotFollowed() {
    ScriptServer server;
    server.handler = [](const QByteArray &, const QJsonObject &) {
      return Reply{302, "{}", "Location: http://127.0.0.1:1/steal\r\n"};
    };
    WellFaciesService svc;
    svc.configure(server.config());
    QVERIFY(runPredict(svc).contains("302"));
    QCOMPARE(server.paths.size(), 1);
  }

  void lateResponseAfterCancelIgnored() {
    ScriptServer server;
    server.handler = [](const QByteArray &, const QJsonObject &body) {
      Reply r{200, json(completed(body.value("wells").toArray().first().toObject().value("rows").toArray()))};
      r.hold = true;
      return r;
    };
    WellFaciesService svc;
    svc.configure(server.config());
    QSignalSpy done(&svc, &WellFaciesService::completed), failed(&svc, &WellFaciesService::failed);
    svc.predict(model(), "W-1", prepareWellFaciesInput(well(), model()));
    QTRY_COMPARE(server.held.size(), 1);
    svc.cancel();
    server.release();
    QTest::qWait(300);
    QVERIFY(done.isEmpty());
    QVERIFY(failed.isEmpty());
  }

  void cancelStopsPolling() {
    ScriptServer server;
    server.handler = [](const QByteArray &, const QJsonObject &) { return Reply{202, json(accepted("job-1"))}; };
    WellFaciesService svc;
    svc.configure(server.config());
    QSignalSpy progress(&svc, &WellFaciesService::progress);
    svc.predict(model(), "W-1", prepareWellFaciesInput(well(), model()));
    QTRY_VERIFY(server.count("/predictions/job-1") >= 1);
    svc.cancel();
    const int seen = server.paths.size();
    QTest::qWait(400); // pollAfterMs=100：未取消会再轮询数次
    QCOMPARE(server.paths.size(), seen);
  }

  void resumeAfterCancelDoesNotResubmit() {
    ScriptServer server;
    server.handler = [](const QByteArray &, const QJsonObject &) { return Reply{202, json(accepted("job-7"))}; };
    WellFaciesService svc;
    svc.configure(server.config());
    svc.predict(model(), "W-1", prepareWellFaciesInput(well(), model()));
    QTRY_COMPARE(server.count("/predictions/job-7"), 1);
    svc.cancel();
    svc.predict(model(), "W-1", prepareWellFaciesInput(well(), model()));
    QTRY_COMPARE(server.count("/predictions/job-7"), 2);
    svc.cancel();
    int submits = 0;
    for (const auto &p : server.paths) submits += p.endsWith("/predict");
    QCOMPARE(submits, 1); // 幂等：同输入续查已受理任务，不重复 POST
  }
};

QTEST_GUILESS_MAIN(TestWellFaciesRemote)
#include "tst_wellfacies_remote.moc"
