// 层：测试壳
#include <QtTest>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QLabel>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextBrowser>
#include <QTextEdit>
#include <QTimer>
#include <functional>

#include "../src/ai/chat/llmclient.h"
#include "../src/ui/ai/aiassistdock.h"
#include "../src/ui/paleotheme.h"
#include "../src/workflow/aichatcontroller.h"

// 方向51：AI 助手 dock（视图层）——禁用态、流式渲染、工具调用占位卡片、
// 错误态。端点一律走 127.0.0.1 假服务器（本机），不发外网请求。
namespace {
const char kChunkA[] = "data: {\"choices\":[{\"delta\":{\"content\":\"砂岩\"}}]}\n\n";
const char kChunkB[] = "data: {\"choices\":[{\"delta\":{\"content\":\"为主\"}}]}\n\n";
const char kDone[] =
  "data: {\"choices\":[{\"delta\":{\"content\":\"已完成\"}}]}\n\n"
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
  enum class Mode { Stream, ToolCalls, ToolCallsOnce, Unauthorized };
  bool start(Mode mode) {
    m_mode = mode;
    connect(&m_server, &QTcpServer::newConnection, this,
            [this] { serve(m_server.nextPendingConnection()); });
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
    if (m_mode == Mode::Unauthorized) {
      const QByteArray body = "{\"error\":{\"message\":\"bad key\"}}";
      sock->write("HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n"
                  "Content-Length: " +
                  QByteArray::number(body.size()) + "\r\n\r\n" + body);
      return;
    }
    sock->write("HTTP/1.1 200 OK\r\nConnection: close\r\n"
                "Content-Type: text/event-stream\r\n\r\n");
    const bool toolRound =
      m_mode == Mode::ToolCalls ||
      (m_mode == Mode::ToolCallsOnce && m_requestCount++ == 0);
    const QByteArray tail = toolRound ? QByteArray(kToolChunk) : QByteArray(kDone);
    QTimer::singleShot(20, sock, [sock] { sock->write(kChunkA); });
    QTimer::singleShot(50, sock, [sock, tail] {
      sock->write(tail);
      sock->disconnectFromHost();
    });
  }
  Mode m_mode = Mode::Stream;
  int m_requestCount = 0;
  QTcpServer m_server;
};

class TestAiAssistDock : public QObject {
  Q_OBJECT
private slots:
  void initTestCase();
  void unconfiguredControllerShowsDisabledState();
  void streamingRendersIntoTranscript();
  void toolCallRendersPlaceholderCard();
  void errorStateIsRenderedWithReason();

private:
  static bool spinUntil(const std::function<bool()> &done, int timeoutMs = 15000) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < timeoutMs)
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
  }
  LlmConfig makeConfig(const QUrl &endpoint) const {
    LlmConfig config;
    config.endpoint = endpoint;
    config.model = QStringLiteral("test-model");
    config.stream = true;
    config.timeoutMs = 5000;
    config.apiKey = "sk-test-fixture";
    return config;
  }
  void dropSession(AiChatController *controller) {
    if (!controller)
      return;
    QString error;
    ChatSessionStore::remove(controller->session().id, &error);
  }
};

void TestAiAssistDock::initTestCase() {
  PaleoTheme::pinRenderEnvironment(); // 字体/主题钉死（渲染断言稳定）
}

void TestAiAssistDock::unconfiguredControllerShowsDisabledState() {
  AiChatController controller; // 未配置：端点/模型/密钥都没有
  AiAssistDock dock(&controller);
  auto *status = dock.findChild<QLabel *>(QStringLiteral("aiAssistantStatus"));
  auto *send = dock.findChild<QPushButton *>(QStringLiteral("aiAssistantSend"));
  auto *input = dock.findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"));
  auto *stop = dock.findChild<QPushButton *>(QStringLiteral("aiAssistantStop"));
  QVERIFY(status && send && input && stop);
  // 诚实禁用态：状态行写明原因，输入与发送都不可用——不冒充能用。
  QVERIFY2(status->text().contains(QStringLiteral("尚未配置")),
           qPrintable(status->text()));
  QVERIFY(!send->isEnabled());
  QVERIFY(!input->isEnabled());
  QVERIFY(!stop->isEnabled());
}

void TestAiAssistDock::streamingRendersIntoTranscript() {
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Stream));
  AiChatController controller;
  controller.setConfig(makeConfig(server.endpoint()));
  AiAssistDock dock(&controller);
  auto *input = dock.findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"));
  auto *send = dock.findChild<QPushButton *>(QStringLiteral("aiAssistantSend"));
  auto *transcript =
    dock.findChild<QTextBrowser *>(QStringLiteral("aiAssistantTranscript"));
  QVERIFY(input && send && transcript);
  QVERIFY(send->isEnabled()); // 配置齐全 → 可用

  input->setPlainText(QStringLiteral("这段是什么岩性？"));
  send->click();
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "流式必须结束（或超时报错）");
  const QString text = transcript->toPlainText();
  QVERIFY2(text.contains(QStringLiteral("砂岩")), qPrintable(text));
  QVERIFY2(text.contains(QStringLiteral("这段是什么岩性？")),
           qPrintable(text)); // 用户那条也要上屏
  dropSession(&controller);
}

void TestAiAssistDock::toolCallRendersPlaceholderCard() {
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::ToolCallsOnce));
  AiChatController controller;
  controller.setConfig(makeConfig(server.endpoint()));
  AiAssistDock dock(&controller);
  auto *cards = dock.findChild<QWidget *>(QStringLiteral("aiAssistantToolCards"));
  QVERIFY(cards);
  dock.findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"))
    ->setPlainText(QStringLiteral("帮我跑一下测井相"));
  dock.findChild<QPushButton *>(QStringLiteral("aiAssistantSend"))->click();
  QVERIFY2(spinUntil([&cards] {
             return !cards->findChildren<QFrame *>(
                       QStringLiteral("aiAssistantToolCard")).isEmpty();
           }),
           "工具调用必须出占位卡片");
  // 方向61：卡片两态翻面——未配置远端 → 终态「失败」+ 如实摘要，然后模型
  // 拿到错误结果给出终答（ToolCallsOnce：工具一轮、文字一轮）。
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }, 20000),
           "诚实失败轮必须收敛（错误回灌→终答）");
  const auto cardList =
    cards->findChildren<QFrame *>(QStringLiteral("aiAssistantToolCard"));
  QCOMPARE(cardList.size(), 1); // 同 id 帧不叠卡
  QString cardText;
  for (QLabel *label : cardList.first()->findChildren<QLabel *>())
    cardText += label->text() + QLatin1Char('\n');
  QVERIFY2(cardText.contains(QStringLiteral("失败")), qPrintable(cardText));
  QVERIFY2(cardText.contains(QStringLiteral("未配置")), qPrintable(cardText));
  dropSession(&controller);
}

void TestAiAssistDock::errorStateIsRenderedWithReason() {
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Unauthorized));
  AiChatController controller;
  controller.setConfig(makeConfig(server.endpoint()));
  AiAssistDock dock(&controller);
  auto *transcript =
    dock.findChild<QTextBrowser *>(QStringLiteral("aiAssistantTranscript"));
  dock.findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"))
    ->setPlainText(QStringLiteral("hi"));
  dock.findChild<QPushButton *>(QStringLiteral("aiAssistantSend"))->click();
  QVERIFY2(spinUntil([transcript] {
             return transcript->toPlainText().contains(QStringLiteral("密钥"));
           }),
           qPrintable(transcript->toPlainText()));
  QVERIFY(!controller.streaming());
  dropSession(&controller);
}

QTEST_MAIN(TestAiAssistDock)
#include "tst_aiassistdock.moc"
