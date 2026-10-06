#include <QtTest>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <functional>
#include <QEventLoop>
#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
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
};

class TestAiChatController : public QObject {
  Q_OBJECT
private slots:
  void init();
  void cleanup();
  void streamingAssemblesAnswerAndSavesSession();
  void toolCallFramesReachThePanelWithHonestStatus();
  void unauthorizedResponseIsClassifiedAsAuthError();
  void idleTimeoutIsClassifiedAsTimeout();
  void cancelDropsEmptyAssistantPlaceholder();
  void disabledConfigReportsAndSendsNothing();

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

int main(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);
  TestAiChatController tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_aichatcontroller.moc"
