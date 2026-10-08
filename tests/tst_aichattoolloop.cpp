#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

#include <functional>
#include <memory>

#include <qgsapplication.h>

#include <gdal.h>

#include "../src/ai/chat/llmclient.h"
#include "../src/ai/onnxfixture.h"
#include "../src/ai/onnxpredictionservice.h"
#include "../src/catalog/datacatalog.h"
#include "../src/metadata/layermanifest.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/services/paleotaskservice.h"
#include "../src/workflow/aichatcontroller.h"
#include "../src/workflow/aichattoolrunner.h"
#include "../src/workflow/aiassistworkflow.h"

// 方向61：AI 工具调用闭环（tools[] 上送 → tool_calls 真执行 → role=tool
// 回灌 → 终答）。全链路走 127.0.0.1 脚本化假端点（无外网、无第三方 SDK），
// tile 分类用夹具小模型真跑（PaleoTaskService 异步 + 协作取消）。
//
// 断言纪律：只断言语义（请求体形状/回灌对账/状态迁移/产品有无），不断言
// 墙钟时长；取消测试的取数注入延迟只为让取消稳定落在执行中，不是计时断言。
namespace {

QByteArray sseEvent(const QJsonObject &event) {
  return "data: " +
         QJsonDocument(event).toJson(QJsonDocument::Compact) + "\n\n";
}

QByteArray doneTail(int promptTokens, int completionTokens) {
  QJsonObject usage;
  usage.insert(QStringLiteral("prompt_tokens"), promptTokens);
  usage.insert(QStringLiteral("completion_tokens"), completionTokens);
  QJsonObject choice;
  choice.insert(QStringLiteral("delta"), QJsonObject());
  QJsonObject event;
  event.insert(QStringLiteral("choices"), QJsonArray{choice});
  event.insert(QStringLiteral("usage"), usage);
  return sseEvent(event) + QByteArrayLiteral("data: [DONE]\n\n");
}

// 一帧/多帧 tool_calls 的流式应答（SSE 按 index 分段由 parser 拼回）。
QByteArray toolCallsSse(const QVector<QPair<QString, QString>> &calls) {
  QByteArray out;
  for (int i = 0; i < calls.size(); ++i) {
    const QString id = calls[i].first;
    const QString name = calls[i].second.section(QLatin1Char('|'), 0, 0);
    const QString args = calls[i].second.section(QLatin1Char('|'), 1);
    QJsonObject function;
    function.insert(QStringLiteral("name"), name);
    function.insert(QStringLiteral("arguments"), args);
    QJsonObject call;
    call.insert(QStringLiteral("index"), i);
    call.insert(QStringLiteral("id"), id);
    call.insert(QStringLiteral("type"), QStringLiteral("function"));
    call.insert(QStringLiteral("function"), function);
    QJsonObject delta;
    delta.insert(QStringLiteral("tool_calls"), QJsonArray{call});
    QJsonObject choice;
    choice.insert(QStringLiteral("delta"), delta);
    QJsonObject event;
    event.insert(QStringLiteral("choices"), QJsonArray{choice});
    out += sseEvent(event);
  }
  return out + doneTail(50, 20);
}

QByteArray textAnswerSse(const QString &text) {
  const QStringList words = {text.left(text.size() / 2), text.mid(text.size() / 2)};
  QByteArray out;
  for (const QString &word : words) {
    QJsonObject delta;
    delta.insert(QStringLiteral("content"), word);
    QJsonObject choice;
    choice.insert(QStringLiteral("delta"), delta);
    QJsonObject event;
    event.insert(QStringLiteral("choices"), QJsonArray{choice});
    out += sseEvent(event);
  }
  return out + doneTail(60, 10);
}

// 脚本化假端点：逐请求弹脚本应答；捕获请求体 JSON；无脚本时挂起不答
// （供「取消后不得有后续请求」类断言用）。
class ScriptedLlmServer : public QObject {
  Q_OBJECT
public:
  bool start() {
    connect(&m_server, &QTcpServer::newConnection, this,
            [this] { serve(m_server.nextPendingConnection()); });
    return m_server.listen(QHostAddress::LocalHost, 0);
  }
  QUrl endpoint() const {
    return QUrl(QStringLiteral("http://127.0.0.1:%1/v1").arg(m_server.serverPort()));
  }
  void enqueue(const QByteArray &response) { m_script.append(response); }
  int requestCount() const { return m_bodies.size(); }
  QJsonObject body(int index) const { return m_bodies.value(index); }

private:
  void serve(QTcpSocket *sock) {
    if (!sock)
      return;
    connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
    auto buf = std::make_shared<QByteArray>();
    connect(sock, &QTcpSocket::readyRead, sock, [this, sock, buf] {
      buf->append(sock->readAll());
      const int headerEnd = buf->indexOf("\r\n\r\n");
      if (headerEnd < 0)
        return;
      const QString header = QString::fromUtf8(buf->left(headerEnd));
      const int contentLength =
        header.section("Content-Length:", 1, 1).section("\r", 0, 0).trimmed().toInt();
      if (buf->mid(headerEnd + 4).size() < contentLength)
        return;
      const QByteArray bodyBytes = buf->mid(headerEnd + 4, contentLength);
      *buf = buf->mid(headerEnd + 4 + contentLength);
      m_bodies.append(QJsonDocument::fromJson(bodyBytes).object());
      if (m_script.isEmpty())
        return; // 挂起不答
      const QByteArray response = m_script.takeFirst();
      sock->write("HTTP/1.1 200 OK\r\nConnection: close\r\n"
                  "Content-Type: text/event-stream\r\n\r\n");
      QTimer::singleShot(10, sock, [sock, response] {
        sock->write(response);
        sock->flush();
        sock->disconnectFromHost();
      });
    });
  }
  QTcpServer m_server;
  QVector<QJsonObject> m_bodies;
  QVector<QByteArray> m_script;
};

QString tileArgs() {
  return QStringLiteral(
           "paleo.tile_classification|{\"model\":\"seg3\",\"grid_rows\":32,"
           "\"grid_columns\":24,\"tile_rows\":16,\"tile_columns\":16,\"halo\":4}");
}

QString faciesArgs() {
  return QStringLiteral(
    "paleo.well_facies_prediction|{\"model\":\"f1\",\"well_name\":\"W-1\"}");
}

bool syntheticFetch(int row0, int col0, int rows, int cols, QVector<float> &out,
                    QString &) {
  out.resize(rows * cols);
  for (int r = 0; r < rows; ++r)
    for (int c = 0; c < cols; ++c)
      out[r * cols + c] = float((row0 + r) * 0.05 + (col0 + c) * 0.03) - 0.5f;
  return true;
}

bool hasDecl(QgisLayerService *layers, const QString &id) {
  const QVector<LayerDeclaration> all = layers->declared();
  for (const LayerDeclaration &d : all)
    if (d.layerId == id)
      return true;
  return false;
}
} // namespace

class TestAiChatToolLoop : public QObject {
  Q_OBJECT
private slots:
  void initTestCase();
  void cleanup();

  void fullTileLoopExecutesFeedsBackAndAnswers();
  void multiCallRoundAnswersEveryToolCallId();
  // #281：一轮首个调用同步失败时，派发循环不得被重入改容器（迭代器失效
  // UB / 漏派后续调用卡在「正在执行领域工具…」）。
  void syncFailingFirstCallStillDispatchesRestOfRound();
  void unboundFaciesIsHonestErrorNotFakeSuccess();
  void cancelDuringToolExecutionInvalidatesAndKeepsProtocolValid();
  void toolRoundCapStopsEndlessLooping();
  void sendWhileBusyIsRejected();
  void historyWindowTruncatesOldTurnsButKeepsSystemAndRecent();
  void toolResultClampAndTokenEstimateContract();
  // 方向77：工程上下文只读工具（query_project）的端到端闭环 + 预算协同。
  void queryProjectLoopAnswersWithWellNames();
  void projectBriefStaysInSystemPromptWithinBudget();

private:
  struct Fixture {
    QTemporaryDir dir;
    DataCatalog catalog;
    QgisProjectService projectSvc;
    LayerManifest manifest{dir.filePath(QStringLiteral("project.sqlite"))};
    QgisLayerService layers{&projectSvc, &manifest};
    PaleoOnnxService onnx;
    PaleoTaskService tasks;
    AiAssistWorkflow wf{&layers};
  };
  bool initFixture(Fixture &f);
  static bool spinUntil(const std::function<bool()> &done, int timeoutMs = 15000);
  LlmConfig baseConfig(const QUrl &endpoint) const;

  QStringList m_createdSessions;
};

void TestAiChatToolLoop::initTestCase() {
  QVERIFY(QgisRuntime::isInitialized());
  QVERIFY(GDALGetDriverByName("GTiff") != nullptr);
}

void TestAiChatToolLoop::cleanup() {
  for (const QString &id : m_createdSessions) {
    QString error;
    ChatSessionStore::remove(id, &error);
  }
  m_createdSessions.clear();
}

bool TestAiChatToolLoop::initFixture(Fixture &f) {
  if (!f.dir.isValid() || !f.catalog.open(f.dir.path()))
    return false;
  if (!f.projectSvc.createProject(f.dir.filePath(QStringLiteral("proj.qgz"))))
    return false;
  if (!f.manifest.open())
    return false;
  const QString models = f.dir.filePath(QStringLiteral("models"));
  if (!QDir().mkpath(models))
    return false;
  if (!OnnxFixtureWriter::writeSeg(models + QStringLiteral("/seg3.onnx"),
                                   {1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 0.1f}))
    return false;
  f.onnx.setModelRoot(models);
  f.wf.setOnnxService(&f.onnx);
  f.wf.setCatalog(&f.catalog, f.dir.path());
  f.wf.setTaskService(&f.tasks);
  return true;
}

bool TestAiChatToolLoop::spinUntil(const std::function<bool()> &done,
                                   int timeoutMs) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < timeoutMs)
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  return done();
}

LlmConfig TestAiChatToolLoop::baseConfig(const QUrl &endpoint) const {
  LlmConfig config;
  config.endpoint = endpoint;
  config.model = QStringLiteral("test-model");
  config.stream = true;
  config.timeoutMs = 8000;
  config.apiKey = "sk-test-fixture";
  return config;
}

// Oracle 1+2：tools[] 三工具 schema 上送 + tool_choice=auto；tool_calls 真跑
// tile 分类（夹具模型）→ role=tool 回灌（rows/layer_ids 对账）→ 终答落会话。
void TestAiChatToolLoop::fullTileLoopExecutesFeedsBackAndAnswers() {
  Fixture f;
  QVERIFY(initFixture(f));
  ScriptedLlmServer server;
  QVERIFY(server.start());
  server.enqueue(toolCallsSse({{QStringLiteral("call-1"), tileArgs()}}));
  const QString finalText = QStringLiteral("分类完成：三张产品已入草稿通道。");
  server.enqueue(textAnswerSse(finalText));

  AiChatController controller;
  controller.setConfig(baseConfig(server.endpoint()));
  m_createdSessions.append(controller.session().id);
  controller.toolRunner()->setWorkflow(&f.wf);
  AiChatToolContext context;
  context.horizon = QStringLiteral("T1");
  context.gridFetch = &syntheticFetch;
  controller.toolRunner()->setContext(context);

  QSignalSpy results(&controller, &AiChatController::toolResultReady);
  controller.sendUserText(QStringLiteral("帮我跑一下相分类"));
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "全链路（工具执行→回灌→终答）必须收敛");

  // 请求 1：tools[] 上送（五工具 schema——方向77 增 query_project/asset_lineage）
  // + tool_choice=auto。
  QCOMPARE(server.requestCount(), 2);
  const QJsonObject body1 = server.body(0);
  const QJsonArray tools = body1.value(QStringLiteral("tools")).toArray();
  QCOMPARE(tools.size(), 5);
  QStringList toolNames;
  for (const QJsonValue &value : tools) {
    const QJsonObject entry = value.toObject();
    QCOMPARE(entry.value(QStringLiteral("type")).toString(),
             QStringLiteral("function"));
    toolNames.append(entry.value(QStringLiteral("function"))
                      .toObject()
                      .value(QStringLiteral("name"))
                      .toString());
    QVERIFY(!entry.value(QStringLiteral("function"))
               .toObject()
               .value(QStringLiteral("parameters"))
               .toObject()
               .isEmpty());
  }
  QVERIFY(toolNames.contains(QStringLiteral("paleo.tile_classification")));
  QVERIFY(toolNames.contains(QStringLiteral("paleo.horizon_tracking_suggestion")));
  QVERIFY(toolNames.contains(QStringLiteral("paleo.well_facies_prediction")));
  QCOMPARE(body1.value(QStringLiteral("tool_choice")).toString(),
           QStringLiteral("auto"));
  QCOMPARE(body1.value(QStringLiteral("messages"))
             .toArray()
             .at(0)
             .toObject()
             .value(QStringLiteral("role"))
             .toString(),
           QStringLiteral("system"));

  // 请求 2：assistant 带 tool_calls + role=tool 应答（id 对账 + 结果摘要），
  // 且续轮仍带 tools（模型可继续点工具）。
  const QJsonObject body2 = server.body(1);
  QCOMPARE(body2.value(QStringLiteral("tool_choice")).toString(),
           QStringLiteral("auto"));
  const QJsonArray messages2 = body2.value(QStringLiteral("messages")).toArray();
  bool sawAssistantCall = false, sawToolReply = false;
  for (const QJsonValue &value : messages2) {
    const QJsonObject message = value.toObject();
    const QString role = message.value(QStringLiteral("role")).toString();
    if (role == QStringLiteral("assistant") && message.contains("tool_calls")) {
      const QJsonArray calls = message.value(QStringLiteral("tool_calls")).toArray();
      QCOMPARE(calls.size(), 1);
      QCOMPARE(calls.at(0).toObject().value(QStringLiteral("id")).toString(),
               QStringLiteral("call-1"));
      sawAssistantCall = true;
    }
    if (role == QStringLiteral("tool")) {
      QCOMPARE(message.value(QStringLiteral("tool_call_id")).toString(),
               QStringLiteral("call-1"));
      const QJsonObject payload = QJsonDocument::fromJson(
                                     message.value(QStringLiteral("content"))
                                       .toString()
                                       .toUtf8())
                                     .object();
      QCOMPARE(payload.value(QStringLiteral("rows")).toInt(), 32);
      QCOMPARE(payload.value(QStringLiteral("columns")).toInt(), 24);
      QCOMPARE(payload.value(QStringLiteral("layer_ids")).toArray().size(), 3);
      QVERIFY(payload.value(QStringLiteral("classified_pixels")).toInt() > 0);
      sawToolReply = true;
    }
  }
  QVERIFY(sawAssistantCall);
  QVERIFY(sawToolReply);

  // 执行面真跑了：产品三件套已声明（DERIVED 草稿通道）。
  QVERIFY(hasDecl(&f.layers, QStringLiteral("aifacies.T1.seg3")));
  QVERIFY(hasDecl(&f.layers, QStringLiteral("aifacies.T1.seg3.masked")));
  QVERIFY(hasDecl(&f.layers, QStringLiteral("confidence.T1.seg3")));
  QCOMPARE(f.wf.lastProductLayerIds().size(), 3);

  // 卡片终态：成功（ok=true）。
  QCOMPARE(results.size(), 1);
  QVERIFY(results.at(0).at(1).toBool());

  // 会话终态：user → assistant(calls) → tool → assistant(终答)。
  const QVector<ChatMessage> messages = controller.messages();
  QCOMPARE(messages.size(), 4);
  QCOMPARE(messages.at(1).toolCalls.size(), 1);
  QCOMPARE(messages.at(2).role, ChatRole::Tool);
  QCOMPARE(messages.at(2).toolCallId, QStringLiteral("call-1"));
  QCOMPARE(messages.at(3).content, finalText);
}

// 一帧多工具：tile（可执行）+ facies（未配置 → 诚实错误）都必须有 role=tool
// 应答，缺一个是协议违规（OpenAI 400）。
void TestAiChatToolLoop::multiCallRoundAnswersEveryToolCallId() {
  Fixture f;
  QVERIFY(initFixture(f));
  ScriptedLlmServer server;
  QVERIFY(server.start());
  server.enqueue(toolCallsSse({{QStringLiteral("call-a"), tileArgs()},
                               {QStringLiteral("call-b"), faciesArgs()}}));
  server.enqueue(textAnswerSse(QStringLiteral("两项都处理完了。")));

  AiChatController controller;
  controller.setConfig(baseConfig(server.endpoint()));
  m_createdSessions.append(controller.session().id);
  controller.toolRunner()->setWorkflow(&f.wf);
  AiChatToolContext context;
  context.horizon = QStringLiteral("T1");
  context.gridFetch = &syntheticFetch;
  controller.toolRunner()->setContext(context);

  controller.sendUserText(QStringLiteral("分类并预测测井相"));
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "多工具轮必须收敛");
  QCOMPARE(server.requestCount(), 2);
  const QJsonArray messages2 =
    server.body(1).value(QStringLiteral("messages")).toArray();
  QStringList answeredIds, erroredIds;
  for (const QJsonValue &value : messages2) {
    const QJsonObject message = value.toObject();
    if (message.value(QStringLiteral("role")).toString() != QStringLiteral("tool"))
      continue;
    answeredIds.append(message.value(QStringLiteral("tool_call_id")).toString());
    const QJsonObject payload = QJsonDocument::fromJson(
                                  message.value(QStringLiteral("content")).toString().toUtf8())
                                  .object();
    if (payload.contains("error"))
      erroredIds.append(message.value(QStringLiteral("tool_call_id")).toString());
  }
  answeredIds.sort(); // 应答顺序协议不要求（执行串行、到达序任意）：按集合比
  QCOMPARE(answeredIds,
           QStringList({QStringLiteral("call-a"), QStringLiteral("call-b")}));
  QCOMPARE(erroredIds, QStringList{QStringLiteral("call-b")}); // facies 如实报错
}

// #281（ABA-02 复现形状）：一轮两个调用，**第一个**同步失败（tile 缺必填
// 实参 → 执行器同步错误回报）。旧实现里 run() 在同一栈 emit toolFinished，
// 控制器 range-for 遍历 m_unansweredCalls 中被重入 removeAt——迭代器失效
// UB，第二个调用漏派、pending 永不归零卡死。断言：收敛 + 两个 id 都有
// role=tool 应答（失败如实带 error，成功带结果）。
void TestAiChatToolLoop::syncFailingFirstCallStillDispatchesRestOfRound() {
  Fixture f;
  QVERIFY(initFixture(f));
  ScriptedLlmServer server;
  QVERIFY(server.start());
  // 第一个：tile 实参缺 model（必填）→ 同步失败；第二个：正常 tile 调用。
  server.enqueue(toolCallsSse(
    {{QStringLiteral("call-first"),
      QStringLiteral("paleo.tile_classification|{\"grid_rows\":32,"
                     "\"grid_columns\":24}")},
     {QStringLiteral("call-second"), tileArgs()}}));
  server.enqueue(textAnswerSse(QStringLiteral("已完成能执行的那一项。")));

  AiChatController controller;
  controller.setConfig(baseConfig(server.endpoint()));
  m_createdSessions.append(controller.session().id);
  controller.toolRunner()->setWorkflow(&f.wf);
  AiChatToolContext context;
  context.horizon = QStringLiteral("T1");
  context.gridFetch = &syntheticFetch;
  controller.toolRunner()->setContext(context);

  controller.sendUserText(QStringLiteral("跑两项分类"));
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "首调同步失败的多工具轮必须收敛（不漏派、不卡死）");
  QCOMPARE(server.requestCount(), 2);
  const QJsonArray messages2 =
    server.body(1).value(QStringLiteral("messages")).toArray();
  QStringList answeredIds, erroredIds;
  for (const QJsonValue &value : messages2) {
    const QJsonObject message = value.toObject();
    if (message.value(QStringLiteral("role")).toString() != QStringLiteral("tool"))
      continue;
    answeredIds.append(message.value(QStringLiteral("tool_call_id")).toString());
    const QJsonObject payload = QJsonDocument::fromJson(
                                  message.value(QStringLiteral("content")).toString().toUtf8())
                                  .object();
    if (payload.contains("error"))
      erroredIds.append(message.value(QStringLiteral("tool_call_id")).toString());
  }
  answeredIds.sort();
  QCOMPARE(answeredIds,
           QStringList({QStringLiteral("call-first"), QStringLiteral("call-second")}));
  QCOMPARE(erroredIds, QStringList{QStringLiteral("call-first")}); // 缺实参如实报错
}

// Oracle 5：未配置的远端工具 → 错误结果回灌 + UI 失败态，不冒充成功；
// 模型拿到错误后仍能给终答。
void TestAiChatToolLoop::unboundFaciesIsHonestErrorNotFakeSuccess() {
  Fixture f;
  QVERIFY(initFixture(f));
  ScriptedLlmServer server;
  QVERIFY(server.start());
  server.enqueue(toolCallsSse({{QStringLiteral("call-f"), faciesArgs()}}));
  server.enqueue(textAnswerSse(QStringLiteral("测井相服务未配置，无法预测。")));

  AiChatController controller;
  controller.setConfig(baseConfig(server.endpoint()));
  m_createdSessions.append(controller.session().id);
  controller.toolRunner()->setWorkflow(&f.wf); // facies 未 setFaciesConfig

  QSignalSpy results(&controller, &AiChatController::toolResultReady);
  controller.sendUserText(QStringLiteral("预测 W-1 的测井相"));
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "诚实错误轮必须收敛（错误回灌→终答）");
  QCOMPARE(server.requestCount(), 2);
  QCOMPARE(results.size(), 1);
  QVERIFY(!results.at(0).at(1).toBool()); // ok=false：失败卡片
  // 诚实面以回灌原文为准（卡片摘要是它的截断）。
  const QVector<ChatMessage> messages = controller.messages();
  QCOMPARE(messages.at(2).role, ChatRole::Tool);
  const QJsonObject payload =
    QJsonDocument::fromJson(messages.at(2).content.toUtf8()).object();
  QVERIFY2(payload.value(QStringLiteral("error"))
             .toString()
             .contains(QStringLiteral("测井相服务未配置")),
           qPrintable(messages.at(2).content));
  // 请求 2 的 role=tool 消息同样携带该错误（模型看得到，才能如实转述）。
  const QJsonArray messages2 =
    server.body(1).value(QStringLiteral("messages")).toArray();
  bool fed = false;
  for (const QJsonValue &value : messages2) {
    const QJsonObject message = value.toObject();
    if (message.value(QStringLiteral("role")).toString() == QStringLiteral("tool") &&
        message.value(QStringLiteral("tool_call_id")).toString() ==
          QStringLiteral("call-f")) {
      QVERIFY(message.value(QStringLiteral("content"))
                .toString()
                .contains(QStringLiteral("测井相服务未配置")));
      fed = true;
    }
  }
  QVERIFY(fed);
}

// Oracle 3：在途工具执行时取消——无悬垂回调/无第二条请求；未应答帧补
// 「已取消」role=tool 应答（协议完整）；tile 任务协作取消不写产品。
void TestAiChatToolLoop::cancelDuringToolExecutionInvalidatesAndKeepsProtocolValid() {
  Fixture f;
  QVERIFY(initFixture(f));
  ScriptedLlmServer server;
  QVERIFY(server.start());
  server.enqueue(toolCallsSse({{QStringLiteral("call-x"), tileArgs()}}));

  AiChatController controller;
  controller.setConfig(baseConfig(server.endpoint()));
  m_createdSessions.append(controller.session().id);
  controller.toolRunner()->setWorkflow(&f.wf);
  AiChatToolContext context;
  context.horizon = QStringLiteral("T1");
  // 慢取数：每 tile 30ms——让取消稳定落进「执行中」（非计时断言，见文件头）。
  context.gridFetch = [](int row0, int col0, int rows, int cols,
                         QVector<float> &out, QString &) -> bool {
    QTest::qSleep(30);
    QString unused;
    return syntheticFetch(row0, col0, rows, cols, out, unused);
  };
  controller.toolRunner()->setContext(context);

  QSignalSpy started(&controller, &AiChatController::toolExecutionStarted);
  QSignalSpy results(&controller, &AiChatController::toolResultReady);
  controller.sendUserText(QStringLiteral("分类"));
  QVERIFY2(spinUntil([&started] { return started.size() >= 1; }),
           "工具必须先进入执行中");
  controller.cancel();
  QVERIFY(!controller.streaming());

  // 作废：无后续 LLM 请求、无结果卡片（迟到执行结果不回灌）。
  QTest::qWait(120);
  QCOMPARE(server.requestCount(), 1);
  QCOMPARE(results.size(), 0);

  // tile 任务收尾：协作取消 → 无产品（诚实无部分产品）。
  QVERIFY2(spinUntil([&f] { return f.tasks.runningCount() == 0; }, 10000),
           "任务必须终态（取消或完成），不许悬挂");
  QVERIFY(f.wf.lastProductLayerIds().isEmpty());

  // 会话：assistant(toolCalls) + 一条「已取消」的 role=tool 应答。
  const QVector<ChatMessage> messages = controller.messages();
  QCOMPARE(messages.size(), 3); // user + assistant(calls) + tool(取消)
  QCOMPARE(messages.at(1).toolCalls.size(), 1);
  QCOMPARE(messages.at(2).role, ChatRole::Tool);
  QCOMPARE(messages.at(2).toolCallId, QStringLiteral("call-x"));
  QVERIFY2(messages.at(2).content.contains(QStringLiteral("取消")),
           qPrintable(messages.at(2).content));
  // 控制器存活销毁（无 UAF）：本用例结束即析构 controller（先于 fixture）。
}

// 工具往返上限：模型无限点工具 → 如实报错停轮（防乒乓），不留半死状态。
void TestAiChatToolLoop::toolRoundCapStopsEndlessLooping() {
  Fixture f;
  QVERIFY(initFixture(f));
  ScriptedLlmServer server;
  QVERIFY(server.start());
  for (int i = 0; i < 10; ++i)
    server.enqueue(toolCallsSse({{QStringLiteral("call-%1").arg(i), faciesArgs()}}));

  AiChatController controller;
  controller.setConfig(baseConfig(server.endpoint()));
  m_createdSessions.append(controller.session().id);
  controller.toolRunner()->setWorkflow(&f.wf); // facies 未配置 → 快速错误轮

  QSignalSpy errors(&controller, &AiChatController::errorOccurred);
  controller.sendUserText(QStringLiteral("反复预测"));
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "上限停轮必须收敛");
  QVERIFY(!errors.isEmpty());
  const QString last = errors.last().at(0).toString();
  QVERIFY2(last.contains(QStringLiteral("上限")), qPrintable(last));
  QCOMPARE(server.requestCount(), AiChatController::kMaxToolRounds + 1);
}

// 流式进行中再发消息：如实拒绝，不并发改写会话状态。
void TestAiChatToolLoop::sendWhileBusyIsRejected() {
  ScriptedLlmServer server;
  QVERIFY(server.start()); // 无脚本：挂起不答，保持 streaming

  AiChatController controller;
  controller.setConfig(baseConfig(server.endpoint()));
  m_createdSessions.append(controller.session().id);
  QSignalSpy errors(&controller, &AiChatController::errorOccurred);
  controller.sendUserText(QStringLiteral("第一条"));
  QVERIFY(controller.streaming());
  // 第一条已上送（异步连接：等服务器真收到再谈拒绝语义）。
  QVERIFY2(spinUntil([&server] { return server.requestCount() >= 1; }),
           "第一条请求必须到达服务器");
  controller.sendUserText(QStringLiteral("第二条"));
  QVERIFY2(errors.last().at(0).toString().contains(QStringLiteral("仍在进行")),
           qPrintable(errors.last().at(0).toString()));
  controller.cancel();
  QVERIFY(!controller.streaming());
  QTest::qWait(50);
  QCOMPARE(server.requestCount(), 1); // 拒绝后无第二条请求
}

// Oracle 4：超长历史截断——system prompt 常驻 + 截断标记可见 + 近轮保留、
// 旧轮丢弃（请求体断言）。
void TestAiChatToolLoop::historyWindowTruncatesOldTurnsButKeepsSystemAndRecent() {
  ScriptedLlmServer server;
  QVERIFY(server.start());
  server.enqueue(textAnswerSse(QStringLiteral("好的。")));

  AiChatController controller;
  controller.setConfig(baseConfig(server.endpoint()));

  ChatSession stored;
  stored.id = ChatSessionStore::newSessionId();
  stored.createdAt = stored.updatedAt = QDateTime::currentDateTime();
  for (int i = 0; i < 15; ++i) {
    ChatMessage user;
    user.role = ChatRole::User;
    user.content = QStringLiteral("第%1问：这一段岩性如何？").arg(i);
    ChatMessage assistant;
    assistant.role = ChatRole::Assistant;
    assistant.content = QStringLiteral("第%1答：砂岩为主。").arg(i);
    stored.messages.append(user);
    stored.messages.append(assistant);
  }
  QString error;
  QVERIFY2(ChatSessionStore::save(stored, &error), qPrintable(error));
  m_createdSessions.append(stored.id);
  QVERIFY2(controller.loadSession(stored.id, &error), qPrintable(error));

  controller.sendUserText(QStringLiteral("新问题"));
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "截断轮必须收敛");
  QCOMPARE(server.requestCount(), 1);
  const QJsonArray messages =
    server.body(0).value(QStringLiteral("messages")).toArray();
  QVERIFY(messages.size() >= 2);
  QCOMPARE(messages.at(0).toObject().value(QStringLiteral("role")).toString(),
           QStringLiteral("system"));
  QVERIFY2(messages.at(0).toObject().value(QStringLiteral("content"))
             .toString()
             .contains(QStringLiteral("地质解释助手")),
           "system prompt 必须常驻首位");
  // 截断标记（第二条 system，紧跟常驻 system）。
  const QString second = messages.at(1).toObject().value(QStringLiteral("content")).toString();
  QVERIFY2(second.contains(QStringLiteral("截断")),
           qPrintable(QStringLiteral("第二条消息应为截断标记，实得：%1").arg(second)));

  int userCount = 0;
  QStringList userTexts;
  for (const QJsonValue &value : messages) {
    const QJsonObject message = value.toObject();
    if (message.value(QStringLiteral("role")).toString() == QStringLiteral("user")) {
      ++userCount;
      userTexts.append(message.value(QStringLiteral("content")).toString());
    }
  }
  // 窗口按「轮」计数，新问题这一轮也占一个窗位：15 旧轮 + 新轮 = 16 轮，
  // 保留最近 12 轮 = 第4问..第14问（11 旧轮）+ 新问题。
  QCOMPARE(userCount, AiChatController::kMaxHistoryTurns);
  QVERIFY(userTexts.contains(QStringLiteral("第4问：这一段岩性如何？")));
  QVERIFY(!userTexts.contains(QStringLiteral("第3问：这一段岩性如何？"))); // 更旧已丢
  QVERIFY(userTexts.contains(QStringLiteral("新问题")));
}

// 预算口径单测：工具结果截断（标记如实带原始长度）+ token 字符近似。
void TestAiChatToolLoop::toolResultClampAndTokenEstimateContract() {
  const QString huge(AiChatController::kToolResultCharLimit + 500, QLatin1Char('x'));
  const QString clamped = AiChatController::clampToolResult(huge);
  QVERIFY(clamped.size() > AiChatController::kToolResultCharLimit);
  QVERIFY(clamped.size() < huge.size());
  QVERIFY2(clamped.contains(QStringLiteral("截断")), "截断标记必须可见");
  QVERIFY(clamped.contains(QString::number(huge.size()))); // 原始长度如实

  const QString shortResult = QStringLiteral("{\"rows\":32}");
  QCOMPARE(AiChatController::clampToolResult(shortResult), shortResult);

  // 字符近似口径：ASCII ≈ 4 字符/token；CJK ≈ 1 字符/token（保守方向）。
  QCOMPARE(AiChatController::estimateTokens(QStringLiteral("abcd")), 1);
  QCOMPARE(AiChatController::estimateTokens(QStringLiteral("岩性")), 2);
}

// 方向77 Oracle 6：问「工程里有哪些井」→ 模型点 query_project → 真执行
// （catalog 只读枚举）→ 结果按 role=tool 回灌 → 终答含井名；工程摘要段
// 常驻 system prompt 首位。
void TestAiChatToolLoop::queryProjectLoopAnswersWithWellNames() {
  Fixture f;
  QVERIFY(initFixture(f));
  for (const QString &name : {QStringLiteral("W-1"), QStringLiteral("W-2")}) {
    CatalogEntity well;
    well.id = name.toLower();
    well.entityType = QStringLiteral("well");
    well.name = name;
    QVERIFY(f.catalog.addEntity(well));
  }
  ScriptedLlmServer server;
  QVERIFY(server.start());
  server.enqueue(toolCallsSse({{QStringLiteral("call-q"),
                                QStringLiteral(
                                  "paleo.query_project|{\"topic\":\"wells\"}")}}));
  const QString finalText =
    QStringLiteral("工程里现有两口井：W-1 与 W-2。");
  server.enqueue(textAnswerSse(finalText));

  AiChatController controller;
  controller.setConfig(baseConfig(server.endpoint()));
  m_createdSessions.append(controller.session().id);
  controller.setProjectBrief(
    AiChatToolRunner::projectBrief(&f.catalog, f.dir.path()));
  controller.toolRunner()->setWorkflow(&f.wf);
  AiChatToolContext context;
  context.horizon = QStringLiteral("T1");
  context.gridFetch = &syntheticFetch;
  context.catalog = &f.catalog;
  context.projectDir = f.dir.path();
  controller.toolRunner()->setContext(context);

  QSignalSpy results(&controller, &AiChatController::toolResultReady);
  controller.sendUserText(QStringLiteral("工程里有哪些井？"));
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "query 全链路（工具执行→回灌→终答）必须收敛");
  QCOMPARE(server.requestCount(), 2);

  // 请求 1：system prompt 首位且带工程概况常驻段（预算轻注入）。
  const QJsonArray messages1 =
    server.body(0).value(QStringLiteral("messages")).toArray();
  QCOMPARE(messages1.at(0).toObject().value(QStringLiteral("role")).toString(),
           QStringLiteral("system"));
  QVERIFY2(messages1.at(0).toObject().value(QStringLiteral("content"))
             .toString()
             .contains(QStringLiteral("当前工程概况")),
           "工程摘要段必须常驻 system prompt");
  // tools[] 五只（新工具在列）。
  const QJsonArray tools = server.body(0).value(QStringLiteral("tools")).toArray();
  QStringList toolNames;
  for (const QJsonValue &value : tools)
    toolNames.append(value.toObject().value(QStringLiteral("function"))
                       .toObject()
                       .value(QStringLiteral("name"))
                       .toString());
  QVERIFY(toolNames.contains(QStringLiteral("paleo.query_project")));
  QVERIFY(toolNames.contains(QStringLiteral("paleo.asset_lineage")));

  // 请求 2：role=tool 回灌携带井名（模型据此作答）。
  const QJsonArray messages2 =
    server.body(1).value(QStringLiteral("messages")).toArray();
  bool fedWellNames = false;
  for (const QJsonValue &value : messages2) {
    const QJsonObject message = value.toObject();
    if (message.value(QStringLiteral("role")).toString() != QStringLiteral("tool"))
      continue;
    QCOMPARE(message.value(QStringLiteral("tool_call_id")).toString(),
             QStringLiteral("call-q"));
    const QString content = message.value(QStringLiteral("content")).toString();
    if (content.contains(QStringLiteral("W-1")) &&
        content.contains(QStringLiteral("W-2")))
      fedWellNames = true;
  }
  QVERIFY(fedWellNames);

  // 卡片成功态 + 终答含井名。
  QCOMPARE(results.size(), 1);
  QVERIFY(results.at(0).at(1).toBool());
  const QVector<ChatMessage> messages = controller.messages();
  QCOMPARE(messages.size(), 4);
  QVERIFY(messages.at(3).content.contains(QStringLiteral("W-1")));
}

// 方向77 Oracle 5：工程摘要常驻段 + 12 轮历史开窗 + 4000 字符工具截断
// 共存时，请求体规模仍在预算口径内（system + 截断标记外，历史 ≤6000）。
void TestAiChatToolLoop::projectBriefStaysInSystemPromptWithinBudget() {
  Fixture f;
  QVERIFY(initFixture(f));
  for (int i = 0; i < 3; ++i) {
    CatalogEntity well;
    well.id = QStringLiteral("well-%1").arg(i);
    well.entityType = QStringLiteral("well");
    well.name = QStringLiteral("W-%1").arg(i);
    QVERIFY(f.catalog.addEntity(well));
  }
  ScriptedLlmServer server;
  QVERIFY(server.start());
  server.enqueue(textAnswerSse(QStringLiteral("好的。")));

  AiChatController controller;
  controller.setConfig(baseConfig(server.endpoint()));
  m_createdSessions.append(controller.session().id);
  controller.setProjectBrief(
    AiChatToolRunner::projectBrief(&f.catalog, f.dir.path()));

  // 15 轮历史（超 12 轮开窗）+ 一条贴近上限的工具结果（clamp 后 ~4000）。
  ChatSession stored;
  stored.id = ChatSessionStore::newSessionId();
  stored.createdAt = stored.updatedAt = QDateTime::currentDateTime();
  const QString hugeResult(AiChatController::kToolResultCharLimit + 500,
                           QLatin1Char('x'));
  for (int i = 0; i < 15; ++i) {
    ChatMessage user;
    user.role = ChatRole::User;
    user.content = QStringLiteral("第%1问：这一段岩性如何？").arg(i);
    ChatMessage assistant;
    assistant.role = ChatRole::Assistant;
    assistant.content =
      (i == 7) ? QString() : QStringLiteral("第%1答：砂岩为主。").arg(i);
    if (i == 7) {
      ChatToolCall call;
      call.id = QStringLiteral("call-old-%1").arg(i);
      call.name = QStringLiteral("paleo.tile_classification");
      call.argumentsJson = QStringLiteral("{\"model\":\"seg3\"}");
      assistant.toolCalls.append(call);
    }
    stored.messages.append(user);
    stored.messages.append(assistant);
    if (i == 7) {
      ChatMessage tool;
      tool.role = ChatRole::Tool;
      tool.toolCallId = QStringLiteral("call-old-%1").arg(i);
      tool.content = AiChatController::clampToolResult(hugeResult);
      stored.messages.append(tool);
    }
  }
  QString error;
  QVERIFY2(ChatSessionStore::save(stored, &error), qPrintable(error));
  m_createdSessions.append(stored.id);
  QVERIFY2(controller.loadSession(stored.id, &error), qPrintable(error));

  controller.sendUserText(QStringLiteral("新问题"));
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "预算轮必须收敛");
  QCOMPARE(server.requestCount(), 1);
  const QJsonArray messages =
    server.body(0).value(QStringLiteral("messages")).toArray();
  QVERIFY(messages.size() >= 2);
  // system 常驻首位且带摘要段。
  const QString system = messages.at(0).toObject().value(QStringLiteral("content"))
                           .toString();
  QVERIFY(system.contains(QStringLiteral("地质解释助手")));
  QVERIFY(system.contains(QStringLiteral("当前工程概况")));

  // 预算口径：system + 截断标记之外，全部消息内容估算 token ≤ 历史预算
  //（开窗纪律不因摘要段松动）。
  int historyTokens = 0;
  for (int i = 1; i < messages.size(); ++i)
    historyTokens += AiChatController::estimateTokens(
      messages.at(i).toObject().value(QStringLiteral("content")).toString());
  QVERIFY2(historyTokens <= AiChatController::kHistoryTokenBudget + 60,
           qPrintable(QStringLiteral("历史估算 token=%1 超预算")
                        .arg(historyTokens)));
  // 摘要段自身受限：system 总估算明显小于历史预算（轻注入不喧宾夺主）。
  QVERIFY(AiChatController::estimateTokens(system) <
          AiChatController::kHistoryTokenBudget / 3);
}

int main(int argc, char *argv[]) {
  if (!QgisRuntime::isInitialized())
    QgisRuntime::initialize(QStringLiteral("/usr"));
  QgsApplication::processingRegistry();
  GDALAllRegister();
  TestAiChatToolLoop tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_aichattoolloop.moc"
