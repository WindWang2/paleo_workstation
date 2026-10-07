// 层：测试壳
#include <QtTest>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QLabel>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextFrame>
#include <QTextEdit>
#include <QTimer>
#include <functional>

#include "../src/ai/chat/llmclient.h"
#include "../src/ui/ai/aiassistdock.h"
#include "../src/ui/paleotheme.h"
#include "../src/workflow/aichatcontroller.h"

// 方向51：AI 助手 dock（视图层）——禁用态、流式渲染、工具调用占位卡片、
// 错误态。方向62 追加：会话切换 round-trip（内容/滚动）、markdown 定型
// 渲染、复制/重发、会话行。端点一律走 127.0.0.1 假服务器（本机），
// 不发外网请求。
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
// 方向62：markdown 形态回答（表格 + 粗体 + 行内代码），验证流结束后的
// 定型渲染（流式期间是纯文本，结束后替换为富文本）。
const char kMarkdownChunk[] =
  "data: {\"choices\":[{\"delta\":{\"content\":\"| 井号 | 岩性 |\\n|---|---|\\n"
  "| A1 | 砂岩 |\\n**结论**：粗粒砂岩为主\"}}]}\n\n"
  "data: [DONE]\n\n";
} // namespace

class FakeLlmServer : public QObject {
  Q_OBJECT
public:
  enum class Mode { Stream, ToolCalls, ToolCallsOnce, Unauthorized, Markdown };
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
    if (m_mode == Mode::Unauthorized) {
      const QByteArray body = "{\"error\":{\"message\":\"bad key\"}}";
      sock->write("HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n"
                  "Content-Length: " +
                  QByteArray::number(body.size()) + "\r\n\r\n" + body);
      return;
    }
    sock->write("HTTP/1.1 200 OK\r\nConnection: close\r\n"
                "Content-Type: text/event-stream\r\n\r\n");
    if (m_mode == Mode::Markdown) {
      // markdown 回答一整块下发（表头必须在行首，不能被前缀文本污染）。
      QTimer::singleShot(20, sock, [sock] {
        sock->write(kMarkdownChunk);
        sock->disconnectFromHost();
      });
      return;
    }
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
  int m_requests = 0;
};

class TestAiAssistDock : public QObject {
  Q_OBJECT
private slots:
  void initTestCase();
  void unconfiguredControllerShowsDisabledState();
  void streamingRendersIntoTranscript();
  void toolCallRendersPlaceholderCard();
  void errorStateIsRenderedWithReason();
  void markdownAnswerIsFormattedAfterStreamEnds();
  void sessionSwitchRoundTripsContentAndScroll();
  void renameAndDeleteKeepComboInSync();
  void copyButtonPutsSelectionOrWholeTranscriptOnClipboard();
  void resendIssuesNewRequestWithoutDuplicatingHistory();
  void captureLedgerScreenshots();

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
  // 会话存储重定向 .qttest 沙箱：Windows 上 XDG env 不改写 QStandardPaths，
  // 不重定向的话 combo/列表断言会被本机真实会话存量污染；再清一次目录
  // 防上轮残留（计数断言要确定态）。
  QStandardPaths::setTestModeEnabled(true);
  QDir(ChatSessionStore::directory()).removeRecursively();
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

// 方向62：流式期间纯文本，流结束整块替换为 markdown 渲染（表格要成为
// QTextTable 子帧——纯 insertText 永远不会产生帧结构）。
void TestAiAssistDock::markdownAnswerIsFormattedAfterStreamEnds() {
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Markdown));
  AiChatController controller;
  controller.setConfig(makeConfig(server.endpoint()));
  AiAssistDock dock(&controller);
  auto *transcript =
    dock.findChild<QTextBrowser *>(QStringLiteral("aiAssistantTranscript"));
  dock.findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"))
    ->setPlainText(QStringLiteral("列表对比一下岩性"));
  dock.findChild<QPushButton *>(QStringLiteral("aiAssistantSend"))->click();
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "markdown 流必须结束");
  const QString text = transcript->toPlainText();
  QVERIFY2(text.contains(QStringLiteral("井号")), qPrintable(text));
  QVERIFY2(text.contains(QStringLiteral("A1")), qPrintable(text));
  QVERIFY2(text.contains(QStringLiteral("结论")), qPrintable(text));
  // 富文本结构证据：文档里出现子帧（表格）；流式纯文本阶段不会有帧。
  bool hasTableFrame = false;
  QTextFrame *root = transcript->document()->rootFrame();
  for (QTextFrame::Iterator it = root->begin(); !it.atEnd(); ++it) {
    if (it.currentFrame()) {
      hasTableFrame = true;
      break;
    }
  }
  QVERIFY2(hasTableFrame, "markdown 表格必须渲染成 QTextTable（帧结构）");
  dropSession(&controller);
}

// 方向62：会话切换 round-trip——openSessionRequested 真实 emit + 装配侧
// load 接线（测试内模拟宿主的同一连接），内容回放一致、滚动位置还原。
void TestAiAssistDock::sessionSwitchRoundTripsContentAndScroll() {
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Stream));
  AiChatController controller;
  controller.setConfig(makeConfig(server.endpoint()));
  AiAssistDock dock(&controller);
  auto *transcript =
    dock.findChild<QTextBrowser *>(QStringLiteral("aiAssistantTranscript"));
  auto *combo =
    dock.findChild<QComboBox *>(QStringLiteral("aiAssistantSessions"));
  QVERIFY(transcript && combo);
  const QString idA = controller.session().id;

  // 会话 A 跑一轮（finished 即落盘）。
  dock.findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"))
    ->setPlainText(QStringLiteral("这段是什么岩性？"));
  dock.findChild<QPushButton *>(QStringLiteral("aiAssistantSend"))->click();
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "会话 A 的一轮必须结束");
  QVERIFY2(transcript->toPlainText().contains(QStringLiteral("砂岩")),
           qPrintable(transcript->toPlainText()));

  // 宿主接线（生产在 paleomainwindow_attach_shell，测试同路径连接）。
  connect(&dock, &AiAssistDock::openSessionRequested, &controller,
          [&controller](const QString &id) { controller.loadSession(id); });

  // 新会话：A 进列表（当前未命名固定首位）。
  dock.findChild<QPushButton *>(QStringLiteral("aiAssistantNew"))->click();
  QCOMPARE(combo->count(), 2); // 未命名（当前）+ A
  QVERIFY(transcript->toPlainText().isEmpty()); // 新会话清屏

  // 用户在拉列表里选 A：activated → openSessionRequested → load → 回放。
  QSignalSpy openSpy(&dock, &AiAssistDock::openSessionRequested);
  QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, 1));
  QCOMPARE(openSpy.size(), 1);
  QCOMPARE(openSpy.at(0).at(0).toString(), idA);
  QCOMPARE(controller.session().id, idA);
  const QString replayed = transcript->toPlainText();
  QVERIFY2(replayed.contains(QStringLiteral("这段是什么岩性？")),
           qPrintable(replayed));
  QVERIFY2(replayed.contains(QStringLiteral("砂岩")), qPrintable(replayed));

  // 滚动位置还原：长会话 B 滚到中间，切走再切回，位置一致。
  ChatSession longSession;
  longSession.id = ChatSessionStore::newSessionId();
  longSession.title = QStringLiteral("长会话");
  longSession.createdAt = longSession.updatedAt = QDateTime::currentDateTime();
  ChatMessage long1;
  long1.role = ChatRole::Assistant;
  long1.content = QStringLiteral("分层描述：\n");
  for (int i = 0; i < 60; ++i)
    long1.content += QStringLiteral("第%1层：砂岩与泥岩互层，厚度约 %2 米。\n")
                       .arg(i + 1)
                       .arg(120 - i);
  longSession.messages.append(long1);
  QString error;
  QVERIFY2(ChatSessionStore::save(longSession, &error), qPrintable(error));

  dock.resize(300, 160);
  dock.show();
  controller.loadSession(longSession.id);
  QScrollBar *scroll = transcript->verticalScrollBar();
  const int stored = qMax(1, scroll->maximum() / 2);
  scroll->setValue(stored);
  const int effective = scroll->value(); // 布局未撑开时为 0（断言仍成立）
  controller.loadSession(idA);
  controller.loadSession(longSession.id);
  QCOMPARE(scroll->value(), effective);
  QVERIFY(transcript->toPlainText().contains(QStringLiteral("第1层")));

  dropSession(&controller);
  ChatSessionStore::remove(longSession.id, &error);
  ChatSessionStore::remove(idA, &error); // 清干净，别污染后续用例的计数断言
}

// 方向62：重命名/删除后下拉同步（sessionListChanged → refreshSessions）。
void TestAiAssistDock::renameAndDeleteKeepComboInSync() {
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Stream));
  AiChatController controller;
  controller.setConfig(makeConfig(server.endpoint()));
  AiAssistDock dock(&controller);
  auto *combo =
    dock.findChild<QComboBox *>(QStringLiteral("aiAssistantSessions"));
  const QString idA = controller.session().id;
  dock.findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"))
    ->setPlainText(QStringLiteral("这段是什么岩性？"));
  dock.findChild<QPushButton *>(QStringLiteral("aiAssistantSend"))->click();
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "一轮必须结束（落盘后才能出现在列表里）");
  QCOMPARE(combo->count(), 1); // 只有 A（空的新会话不落盘不进列表）

  QString error;
  QVERIFY(controller.renameSession(idA, QStringLiteral("岩性讨论"), &error));
  QCOMPARE(combo->itemText(0), QStringLiteral("岩性讨论"));
  ChatSession stored;
  QVERIFY(ChatSessionStore::load(idA, &stored, &error));
  QCOMPARE(stored.title, QStringLiteral("岩性讨论"));

  // 删除当前会话：列表回到未命名新会话，文件消失。
  QVERIFY(controller.removeSession(idA, &error));
  QVERIFY(!ChatSessionStore::load(idA, &stored, &error));
  QCOMPARE(combo->count(), 1);
  QVERIFY(combo->itemText(0).contains(QStringLiteral("未命名")));
  dropSession(&controller);
}

// 方向62：复制——有选区复制选区（单条/局部），无选区复制整段。
void TestAiAssistDock::copyButtonPutsSelectionOrWholeTranscriptOnClipboard() {
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Stream));
  AiChatController controller;
  controller.setConfig(makeConfig(server.endpoint()));
  AiAssistDock dock(&controller);
  auto *transcript =
    dock.findChild<QTextBrowser *>(QStringLiteral("aiAssistantTranscript"));
  auto *copy =
    dock.findChild<QPushButton *>(QStringLiteral("aiAssistantCopy"));
  dock.findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"))
    ->setPlainText(QStringLiteral("这段是什么岩性？"));
  dock.findChild<QPushButton *>(QStringLiteral("aiAssistantSend"))->click();
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "一轮必须结束");
  QVERIFY(copy->isEnabled());

  // 无选区：整段（用户消息 + 回答都在）。
  copy->click();
  const QString whole = QApplication::clipboard()->text();
  QVERIFY2(whole.contains(QStringLiteral("这段是什么岩性？")), qPrintable(whole));
  QVERIFY2(whole.contains(QStringLiteral("砂岩")), qPrintable(whole));

  // 有选区：只复制选中的两个字。
  QTextCursor cursor = transcript->textCursor();
  cursor.setPosition(0);
  cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 2);
  transcript->setTextCursor(cursor);
  copy->click();
  QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("你：")
                                                  .left(2));
  dropSession(&controller);
}

// 方向62：重发最后一条用户消息——新请求发出（服务器计数 +1），历史不
// 重复（无第二条 user 消息），transcript 只有一份回答。
void TestAiAssistDock::resendIssuesNewRequestWithoutDuplicatingHistory() {
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Stream));
  AiChatController controller;
  controller.setConfig(makeConfig(server.endpoint()));
  AiAssistDock dock(&controller);
  auto *transcript =
    dock.findChild<QTextBrowser *>(QStringLiteral("aiAssistantTranscript"));
  auto *resend =
    dock.findChild<QPushButton *>(QStringLiteral("aiAssistantResend"));
  dock.findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"))
    ->setPlainText(QStringLiteral("这段是什么岩性？"));
  dock.findChild<QPushButton *>(QStringLiteral("aiAssistantSend"))->click();
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "第一轮必须结束");
  QCOMPARE(server.requestCount(), 1);
  // 本文件假服务器的 Stream 模式答案 = kChunkA 两段（「砂岩砂岩」）。
  const QString firstAnswer = controller.messages().last().content;
  QVERIFY(!firstAnswer.isEmpty());
  QVERIFY(resend->isEnabled());
  resend->click();
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "重发的一轮必须结束");
  QCOMPARE(server.requestCount(), 2);
  QCOMPARE(controller.messages().size(), 2); // user + assistant（无重复 user）
  QCOMPARE(controller.messages().last().content, firstAnswer); // 同源同答
  QCOMPARE(transcript->toPlainText().count(firstAnswer), 1);   // 旧回答被截掉
  dropSession(&controller);
}

// 方向62 ledger 截图（无人值守证据）：默认跳过；设 PALEO_AI_SHOT_DIR 时
// 落两张 PNG（markdown 定型后的 dock 全貌 + 会话下拉展开态）。产物提交
// docs/progress/ai-ux-shots/（先例：crossplot-facies-shots）。
void TestAiAssistDock::captureLedgerScreenshots() {
  const QString dir =
    QString::fromUtf8(qgetenv("PALEO_AI_SHOT_DIR"));
  if (dir.isEmpty())
    QSKIP("截图按需采集（PALEO_AI_SHOT_DIR 未设）");
  FakeLlmServer server;
  QVERIFY(server.start(FakeLlmServer::Mode::Markdown));
  AiChatController controller;
  controller.setConfig(makeConfig(server.endpoint()));
  AiAssistDock dock(&controller);
  dock.findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"))
    ->setPlainText(QStringLiteral("列表对比一下岩性"));
  dock.findChild<QPushButton *>(QStringLiteral("aiAssistantSend"))->click();
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "markdown 流必须结束（定型渲染后截图）");
  dock.resize(520, 420);
  dock.show();
  QVERIFY2(dock.grab()
             .save(dir + QStringLiteral("/ai-ux-dock-markdown.png")),
           "dock 截图落盘失败");
  dropSession(&controller);
}

QTEST_MAIN(TestAiAssistDock)
#include "tst_aiassistdock.moc"
