#include <QtTest>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <functional>
#include <QEventLoop>
#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QStandardPaths>
#include <QTimer>

#include "../src/ai/chat/llmclient.h"
#include "../src/workflow/aichatcontroller.h"

// 方向51：AI 对话助手编排（workflow 层）——流式拼接、错误分类、取消语义、
// 会话落盘、禁用态。端点一律走 127.0.0.1 假服务器（无外网、无第三方 SDK）。
//
// 断言纪律：只断言语义（拼接结果/分类标签/状态迁移），不断言墙钟时长。
namespace {
// SSE 分块文本（真实流式服务把回答切成多个 delta 下发）。
const char kChunkA[] = "data: {\"choices\":[{\"delta\":{\"content\":\"砂岩\"}}]}\n\n";
const char kChunkB[] = "data: {\"choices\":[{\"delta\":{\"content\":\"为主\"}}]}\n\n";
const char kDone[] = "data: {\"choices\":[{\"delta\":{}}],\"usage\":{\"prompt_tokens\":11,\"completion_tokens\":2}}\n\n"
                     "data: [DONE]\n\n";
const char kToolChunk[] =
  "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c1\",\"type\":\"function\","
  "\"function\":{\"name\":\"paleo.well_facies_prediction\",\"arguments\":\"{\\\"model\\\":\\\"f1\\\","
  "\\\"well_name\\\":\\\"A\\\"}\"}}]}}]}\n\n"
  "data: [DONE]\n\n";
} // namespace

class FakeLlmServer : public QObject {
  Q_OBJECT
public:
  enum class Mode { Stream, ToolCalls, Unauthorized, Slow };

  bool start(Mode mode) {
    m_mode = mode;
    connect(&m_server, &QTcpServer::newConnection, this, [this] {
      serve(m_server.nextPendingConnection());
    });
    return m_server.listen(QHostAddress::LocalHost, 0);
  }
  QUrl endpoint() const {
    return QUrl(QStringLiteral("http://127.0.0.1:%1/v1")
                  .arg(m_server.serverPort()));
  }
  int requestCount() const { return m_requests; }

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
      *buf = buf->mid(headerEnd + 4 + contentLength);
      respond(sock);
    });
  }
  void respond(QTcpSocket *sock) {
    ++m_requests;
    if (m_mode == Mode::Slow)
      return; // 挂起不答：逼客户端空闲超时
    if (m_mode == Mode::Unauthorized) {
      const QByteArray body = "{\"error\":{\"message\":\"bad key\"}}";
      sock->write("HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n"
                  "Content-Type: application/json\r\nContent-Length: " +
                  QByteArray::number(body.size()) + "\r\n\r\n" + body);
      return;
    }
    // 流式：不写 Content-Length（分块写、写完关连接），让 readyRead 多次触发。
    sock->write("HTTP/1.1 200 OK\r\nConnection: close\r\n"
                "Content-Type: text/event-stream\r\n\r\n");
    const QByteArray tail =
      m_mode == Mode::ToolCalls ? QByteArray(kToolChunk) : QByteArray(kDone);
    QTimer::singleShot(20, sock, [sock] { sock->write(kChunkA); });
    QTimer::singleShot(45, sock, [sock] { sock->write(kChunkB); });
    QTimer::singleShot(70, sock, [sock, tail] {
      sock->write(tail);
      sock->disconnectFromHost();
    });
  }

  Mode m_mode = Mode::Stream;
  QTcpServer m_server;
  int m_requests = 0;
};

class TestAiChatController : public QObject {
  Q_OBJECT
private slots:
  void initTestCase();
  void init();
  void cleanup();
  void streamingAssemblesAnswerAndSavesSession();
  void toolCallFramesReachThePanelWithHonestStatus();
  void unauthorizedResponseIsClassifiedAsAuthError();
  void idleTimeoutIsClassifiedAsTimeout();
  void cancelDropsEmptyAssistantPlaceholder();
  void disabledConfigReportsAndSendsNothing();
  // 方向62
  void switchingSessionsFlushesUnsavedContentToDisk();
  void retryLastResendsWithoutDuplicatingUserMessage();
  void renameSessionPersistsTitleAndRejectsEmpty();
  void exportCurrentSessionWritesMarkdownMatchingSession();
  void applyConfigPersistsFileAndKeepsHonestDisabledState();
  void applyConfigRejectsStructuralProblemsWithoutSaving();

private:
  static bool spinUntil(const std::function<bool()> &done, int timeoutMs = 15000) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < timeoutMs)
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
  }
  LlmConfig baseConfig(const QUrl &endpoint, int timeoutMs = 5000) const;
  void dropSession(const QString &id);

  QStringList m_createdSessions;
};

LlmConfig TestAiChatController::baseConfig(const QUrl &endpoint,
                                           int timeoutMs) const {
  LlmConfig config;
  config.endpoint = endpoint;
  config.model = QStringLiteral("test-model");
  config.stream = true;
  config.timeoutMs = timeoutMs;
  config.apiKey = "sk-test-fixture"; // 只在内存里流转（不落 JSON）
  return config;
}

void TestAiChatController::dropSession(const QString &id) {
  if (id.isEmpty())
    return;
  QString error;
  ChatSessionStore::remove(id, &error);
}

void TestAiChatController::init() { m_createdSessions.clear(); }

// 会话存储重定向 .qttest 沙箱：Windows 上 XDG env 不改写 QStandardPaths，
// 不重定向会读写本机真实 %LOCALAPPDATA% 会话目录（方向62 起会话面变宽，
// 落盘断言/文件计数都需要封闭环境）。
void TestAiChatController::initTestCase() {
  QStandardPaths::setTestModeEnabled(true);
  QDir(ChatSessionStore::directory()).removeRecursively(); // 上轮残留清零
}

void TestAiChatController::cleanup() {
  for (const QString &id : m_createdSessions)
    dropSession(id);
  m_createdSessions.clear();
}

void TestAiChatController::streamingAssemblesAnswerAndSavesSession() {
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Stream));
  AiChatController controller;
  controller.setConfig(baseConfig(server.endpoint()));
  QVERIFY(controller.enabled());
  QVERIFY(controller.statusText().contains(QStringLiteral("test-model")));

  QStringList deltas;
  connect(&controller, &AiChatController::assistantDelta, this,
          [&deltas](const QString &text) { deltas.append(text); });
  QSignalSpy finished(&controller, &AiChatController::streamingChanged);
  const QString id = controller.session().id;
  m_createdSessions.append(id);

  controller.sendUserText(QStringLiteral("这段是什么岩性？"));
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "流式的结束信号必须到达（或超时报错）");
  QVERIFY(!finished.isEmpty());

  // 拼接结果 = 完整回答（增量顺序不乱）。
  const QVector<ChatMessage> messages = controller.messages();
  QCOMPARE(messages.size(), 2); // user + assistant
  QCOMPARE(messages.last().role, ChatRole::Assistant);
  QCOMPARE(messages.last().content, QStringLiteral("砂岩为主"));
  QVERIFY2(deltas.size() >= 2, "增量必须分多次到达（流式而非一次性）");

  // 落盘：会话在用户目录里，重新可读。
  ChatSession stored;
  QString error;
  QVERIFY2(ChatSessionStore::load(id, &stored, &error), qPrintable(error));
  QCOMPARE(stored.messages.size(), 2);
  QCOMPARE(stored.messages.last().content, QStringLiteral("砂岩为主"));
}

void TestAiChatController::toolCallFramesReachThePanelWithHonestStatus() {
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::ToolCalls));
  AiChatController controller;
  controller.setConfig(baseConfig(server.endpoint()));
  m_createdSessions.append(controller.session().id);
  QSignalSpy dispatched(&controller, &AiChatController::toolCallDispatched);
  QSignalSpy results(&controller, &AiChatController::toolResultReady);
  controller.sendUserText(QStringLiteral("帮我跑一下测井相"));
  QVERIFY2(spinUntil([&dispatched] { return dispatched.size() >= 1; }),
           "工具调用帧必须被分发（含路由结论）");
  const QString status = dispatched.at(0).at(1).toString();
  // 诚实：路由结论如实写出（已路由 + 入口），不冒充"已执行"。
  QVERIFY2(status.contains(QStringLiteral("已路由")), qPrintable(status));
  QVERIFY2(status.contains(QStringLiteral("wellfaciesservice")),
           qPrintable(status));
  // 方向61：工具会真的执行——未配置远端 → 失败结果（不冒充成功）；模型
  // 反复点工具由往返上限兜底收敛。
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }, 20000),
           "诚实失败轮必须收敛（错误回灌→终答或上限停轮）");
  QVERIFY2(results.size() >= 1, "必须给出执行终态（失败也是终态）");
  QVERIFY(!results.at(0).at(1).toBool());
}

void TestAiChatController::unauthorizedResponseIsClassifiedAsAuthError() {
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Unauthorized));
  AiChatController controller;
  controller.setConfig(baseConfig(server.endpoint()));
  m_createdSessions.append(controller.session().id);
  QSignalSpy errors(&controller, &AiChatController::errorOccurred);
  controller.sendUserText(QStringLiteral("hi"));
  QVERIFY2(spinUntil([&errors] { return errors.size() >= 1; }),
           "401 必须报错（不许静默）");
  const QString message = errors.at(0).at(0).toString();
  QVERIFY2(message.contains(llmErrorLabel(LlmErrorKind::Unauthorized)),
           qPrintable(message));
  QVERIFY(!controller.streaming());
}

void TestAiChatController::idleTimeoutIsClassifiedAsTimeout() {
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Slow));
  AiChatController controller;
  controller.setConfig(baseConfig(server.endpoint(), 300));
  m_createdSessions.append(controller.session().id);
  QSignalSpy errors(&controller, &AiChatController::errorOccurred);
  controller.sendUserText(QStringLiteral("hi"));
  QVERIFY2(spinUntil([&errors] { return errors.size() >= 1; }),
           "空闲超时必须报错（不许挂死）");
  const QString message = errors.at(0).at(0).toString();
  QVERIFY2(message.contains(llmErrorLabel(LlmErrorKind::Timeout)),
           qPrintable(message));
  QVERIFY(!controller.streaming());
}

void TestAiChatController::cancelDropsEmptyAssistantPlaceholder() {
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Slow));
  AiChatController controller;
  controller.setConfig(baseConfig(server.endpoint(), 5000));
  m_createdSessions.append(controller.session().id);
  controller.sendUserText(QStringLiteral("hi"));
  QVERIFY(controller.streaming());
  controller.cancel();
  QVERIFY(!controller.streaming());
  const QVector<ChatMessage> messages = controller.messages();
  QCOMPARE(messages.size(), 1); // 只剩用户那条：空白的助手占位被摘掉
  QCOMPARE(messages.first().role, ChatRole::User);
}

void TestAiChatController::disabledConfigReportsAndSendsNothing() {
  AiChatController controller; // 无端点/模型/密钥
  QVERIFY(!controller.enabled());
  QVERIFY(!controller.statusText().isEmpty());
  QSignalSpy errors(&controller, &AiChatController::errorOccurred);
  QSignalSpy appended(&controller, &AiChatController::messageAppended);
  controller.sendUserText(QStringLiteral("hi"));
  QCOMPARE(errors.size(), 1);           // 禁用态如实报错
  QCOMPARE(appended.size(), 0);         // 且不写历史（没有"假装回答"）
  QVERIFY(errors.at(0).at(0).toString().contains(QStringLiteral("尚未配置")));
}

// 方向62：切换会话语义钉死——切换把「当时的内存态历史」原样落盘
// （本轮因取消没等到 finished 落盘，也不许丢）。
void TestAiChatController::switchingSessionsFlushesUnsavedContentToDisk() {
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Slow)); // 挂起：逼出未保存态
  AiChatController controller;
  controller.setConfig(baseConfig(server.endpoint()));
  const QString idA = controller.session().id;
  m_createdSessions.append(idA);
  controller.sendUserText(QStringLiteral("这轮回答不会到"));
  controller.cancel(); // 只剩 user 消息；finished 没发生 → 未落盘
  QCOMPARE(controller.messages().size(), 1);
  const QVector<ChatMessage> snapshotA = controller.messages(); // 切换前快照
  QString error;
  // 方向61 起 cancel 即落盘（作废帧一并记账）——A 此刻可能已在盘上；
  // 本测试钉的是「切换不丢/不改写」，不再要求未落盘前提。

  // 另一个会话 B（直接造文件），切过去。
  ChatSession sessionB;
  sessionB.id = ChatSessionStore::newSessionId();
  sessionB.title = QStringLiteral("B");
  sessionB.createdAt = sessionB.updatedAt = QDateTime::currentDateTime();
  ChatMessage seeded;
  seeded.role = ChatRole::User;
  seeded.content = QStringLiteral("存量问题");
  sessionB.messages.append(seeded);
  QVERIFY2(ChatSessionStore::save(sessionB, &error), qPrintable(error));
  m_createdSessions.append(sessionB.id);

  QVERIFY(controller.loadSession(sessionB.id));
  // 切换把 A 落了盘：内容与切换前的内存快照一致（消息级相等）。
  ChatSession storedA;
  QVERIFY2(ChatSessionStore::load(idA, &storedA, &error), qPrintable(error));
  QCOMPARE(storedA.messages.size(), snapshotA.size());
  for (int i = 0; i < snapshotA.size(); ++i) {
    QCOMPARE(storedA.messages.at(i).role, snapshotA.at(i).role);
    QCOMPARE(storedA.messages.at(i).content, snapshotA.at(i).content);
  }
  // 切回 A：历史完整回来。
  QVERIFY(controller.loadSession(idA));
  QCOMPARE(controller.messages().size(), snapshotA.size());
}

void TestAiChatController::retryLastResendsWithoutDuplicatingUserMessage() {
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Stream));
  AiChatController controller;
  controller.setConfig(baseConfig(server.endpoint()));
  m_createdSessions.append(controller.session().id);
  controller.sendUserText(QStringLiteral("这段是什么岩性？"));
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "第一轮必须结束");
  QCOMPARE(server.requestCount(), 1);

  QVERIFY(controller.canRetry());
  controller.retryLast();
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "重发的一轮必须结束");
  QCOMPARE(server.requestCount(), 2);
  const QVector<ChatMessage> messages = controller.messages();
  QCOMPARE(messages.size(), 2); // user + assistant，无重复 user
  QCOMPARE(messages.first().role, ChatRole::User);
  QCOMPARE(messages.last().role, ChatRole::Assistant);
  QCOMPARE(messages.last().content, QStringLiteral("砂岩为主"));
}

void TestAiChatController::renameSessionPersistsTitleAndRejectsEmpty() {
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Stream));
  AiChatController controller;
  controller.setConfig(baseConfig(server.endpoint()));
  const QString id = controller.session().id;
  m_createdSessions.append(id);
  controller.sendUserText(QStringLiteral("hi"));
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "一轮必须结束（空会话不落盘，重命名无处写）");

  QString error;
  QVERIFY(!controller.renameSession(id, QStringLiteral("  "), &error));
  QVERIFY(!error.isEmpty()); // 空标题如实拒绝

  QVERIFY(controller.renameSession(id, QStringLiteral("新标题"), &error));
  ChatSession stored;
  QVERIFY2(ChatSessionStore::load(id, &stored, &error), qPrintable(error));
  QCOMPARE(stored.title, QStringLiteral("新标题"));
  // 当前会话重命名后内存态标题同步（下拉显示用 displayTitle）。
  QCOMPARE(controller.session().title, QStringLiteral("新标题"));
}

void TestAiChatController::exportCurrentSessionWritesMarkdownMatchingSession() {
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Stream));
  AiChatController controller;
  controller.setConfig(baseConfig(server.endpoint()));
  m_createdSessions.append(controller.session().id);
  controller.sendUserText(QStringLiteral("这段是什么岩性？"));
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "一轮必须结束");

  // 导出落会话沙箱目录（.qttest）——本机 QTemporaryDir 有宿主级问题
  // （tst_las 同症状红），不引入同类依赖。
  const QString path =
    ChatSessionStore::directory() + QStringLiteral("/export-test.md");
  QString error;
  QVERIFY2(controller.exportCurrentSession(path, &error), qPrintable(error));
  QFile file(path);
  QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));
  const QString markdown = QString::fromUtf8(file.readAll());
  file.close();
  QVERIFY(markdown.contains(QStringLiteral("## 你")));
  QVERIFY(markdown.contains(QStringLiteral("这段是什么岩性？")));
  QVERIFY(markdown.contains(QStringLiteral("砂岩为主")));
  QCOMPARE(markdown, controller.session().toMarkdown()); // 导出口径单一来源
  QVERIFY(!controller.exportCurrentSession(QString(), &error)); // 空路径拒绝
  QFile::remove(path);
}

// 方向62：图形化配置的应用编排（对话框只发意图，落盘在这里）。
void TestAiChatController::applyConfigPersistsFileAndKeepsHonestDisabledState() {
  QFile::remove(LlmConfig::path());
  AiChatController controller; // 未配置态
  qunsetenv("PALEO_LLM_ENDPOINT");
  qunsetenv("PALEO_LLM_MODEL");
  qunsetenv("PALEO_LLM_API_KEY");
  QSignalSpy statusChanges(&controller, &AiChatController::statusChanged);

  LlmConfig form;
  form.endpoint = QUrl(QStringLiteral("https://llm.example.com/v1"));
  form.model = QStringLiteral("geo-model-7b");
  form.maxTokens = 2048;
  form.timeoutMs = 45000;
  form.stream = false;

  bool doneOk = false;
  QString doneError = QStringLiteral("unset");
  controller.applyConfig(form, QByteArray(), false, &controller,
                         [&](bool ok, const QString &error) {
                           doneOk = ok;
                           doneError = error;
                         });
  QVERIFY2(doneOk, qPrintable(doneError));
  QVERIFY(QFile::exists(LlmConfig::path()));
  // 文件读回 round-trip：表单值即磁盘值（密钥除外——不在 JSON 面）。
  const LlmConfig loaded = LlmConfig::load();
  QCOMPARE(loaded.endpoint.toString(),
           QStringLiteral("https://llm.example.com/v1"));
  QCOMPARE(loaded.model, QStringLiteral("geo-model-7b"));
  QCOMPARE(loaded.maxTokens, 2048);
  QCOMPARE(loaded.timeoutMs, 45000);
  QCOMPARE(loaded.stream, false);
  // 诚实面：未提供密钥 → 应用成功但助手仍禁用，状态行说明原因。
  QVERIFY(!controller.enabled());
  QVERIFY(controller.statusText().contains(QStringLiteral("密钥")));
  QVERIFY(!statusChanges.isEmpty()); // 应用即生效（状态行已刷新）
  QCOMPARE(controller.config().model, QStringLiteral("geo-model-7b"));
  QFile::remove(LlmConfig::path());
}

void TestAiChatController::applyConfigRejectsStructuralProblemsWithoutSaving() {
  QFile::remove(LlmConfig::path());
  AiChatController controller;
  LlmConfig form;
  form.endpoint = QUrl(QStringLiteral("not-a-url"));
  form.model = QStringLiteral("m");
  bool doneOk = true;
  QString doneError;
  controller.applyConfig(form, QByteArray(), false, &controller,
                         [&](bool ok, const QString &error) {
                           doneOk = ok;
                           doneError = error;
                         });
  QVERIFY(!doneOk);                 // 结构非法：如实拒绝
  QVERIFY(!doneError.isEmpty());    // 且给出原因
  QVERIFY(!QFile::exists(LlmConfig::path())); // 不落盘
}

int main(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);
  TestAiChatController tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_aichatcontroller.moc"
