// 层：测试壳
#include <QtTest>
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextBrowser>
#include <QTextEdit>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
#include <functional>

#include "../src/ai/chat/llmclient.h"
#include "../src/services/toolresultprotocol.h"
#include "../src/ui/ai/aiassistdock.h"
#include "../src/ui/ai/aiconversationcard.h"
#include "../src/ui/ai/aicontextbar.h"
#include "../src/ui/ai/aioperationdetailwidget.h"
#include "../src/ui/paleotheme.h"
#include "../src/workflow/aichatcontroller.h"
#include "../src/workflow/aioperationpresentation.h"
#include "../src/workflow/capabilityhost.h"
#include "../src/workflow/chatinvocationbinding.h"

// G5 W2/W7/W8 dock 级测试：历史工具卡恢复（零模型请求）、草稿/键盘生命
// 周期、有界卡窗口、上下文条。端点一律 127.0.0.1 假服务器（同 tst_aiassistdock
// 口径），历史恢复用例不发任何模型请求。
namespace {
constexpr int kAiTestCardTotal = 500; // 验收矩阵口径：500 历史工具结果 > 单窗口上限 64
const char kStreamDone[] =
  "data: {\"choices\":[{\"delta\":{\"content\":\"砂岩\"}}]}\n\n"
  "data: [DONE]\n\n";

QString envelopeContent(const QJsonObject &native, bool ok) {
  return QString::fromUtf8(paleo::chat::compactJson(
    paleo::chat::resultEnvelope(native, ok)));
}
ChatSession makeSessionWithToolHistory(int toolFrames, bool legacyTurnIds = false) {
  ChatSession session;
  session.id = ChatSessionStore::newSessionId();
  session.title = QStringLiteral("G5 历史会话");
  session.createdAt = session.updatedAt = QDateTime::currentDateTime();
  for (int i = 0; i < toolFrames; ++i) {
    ChatMessage user;
    user.role = ChatRole::User;
    user.content = QStringLiteral("第 %1 个问题").arg(i + 1);
    user.hostTurnId = legacyTurnIds ? QString() : QStringLiteral("turn-%1").arg(i);
    ChatMessage assistant;
    assistant.role = ChatRole::Assistant;
    assistant.content = QStringLiteral("查询中");
    ChatToolCall call;
    call.id = QStringLiteral("call-%1").arg(i);
    call.name = QStringLiteral("paleo.query_project");
    call.argumentsJson = QStringLiteral("{\"topic\":\"summary\"}");
    assistant.toolCalls.append(call);
    ChatMessage tool;
    tool.role = ChatRole::Tool;
    tool.toolCallId = call.id;
    tool.content = envelopeContent(
      QJsonObject{{"topic", "summary"}, {"entities", QJsonObject{{"well", i + 1}}}},
      true);
    session.messages << user << assistant << tool;
    if (!legacyTurnIds &&
        !paleo::chat::bindInvocations(session, user.hostTurnId, "doc-g5",
                                     session.messages[session.messages.size() - 2].toolCalls).isEmpty())
      qFatal("G5 history fixture could not bind its tool call");
  }
  return session;
}
} // namespace

class FakeLlmServer : public QObject {
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
  int requestCount() const { return m_requests; }
  QString lastUserMessage() const { return m_lastUserMessage; }

private:
  // 与 tst_aiassistdock 同口径：等完整请求体（Content-Length）再计一次数并
  // 应答——按头就回会让第二个请求的发送侧阻塞（服务端未读body即写回）。
  void serve(QTcpSocket *sock) {
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
      ++m_requests;
      const auto messages = QJsonDocument::fromJson(buf->mid(headerEnd + 4, contentLength))
                              .object().value("messages").toArray();
      for (const auto &entry : messages)
        if (entry.toObject().value("role") == QLatin1String("user"))
          m_lastUserMessage = entry.toObject().value("content").toString();
      sock->write("HTTP/1.1 200 OK\r\nConnection: close\r\n"
                  "Content-Type: text/event-stream\r\n\r\n");
      QTimer::singleShot(20, sock, [sock] {
        sock->write(kStreamDone);
        sock->disconnectFromHost();
      });
    });
  }
  QTcpServer m_server;
  int m_requests = 0;
  QString m_lastUserMessage;
};

class TestG5ConversationWorkbench : public QObject {
  Q_OBJECT
private slots:
  void initTestCase();
  void cleanup();
  void historySessionRestoresToolCardsWithoutModelRequests();
  void historyCardsShowAttributionAndLazyExpand();
  void boundedCardWindowKeepsOlderResultsAccessible();
  void draftsSurviveRejectionAndRoundTripAcrossSessions();
  void enterSendsShiftEnterBreaks();
  void contextBarShowsBriefAndUpdate();
  void sentQuestionsKeepTheirOwnWorkspace();
  void attachPresenterIdempotentAndNullPresenterKeepsReason();
  void selectedResultPortCanBeReadFromTheDetailView();

private:
  static bool spinUntil(const std::function<bool()> &done, int timeoutMs = 15000) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < timeoutMs) {
      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
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
};

void TestG5ConversationWorkbench::selectedResultPortCanBeReadFromTheDetailView() {
  AiOperationDetailWidget detail;
  const QString operation = QStringLiteral("op-result");
  const QString output = QStringLiteral("OUTPUT");
  detail.openOperation(operation);
  detail.onOperationResults(operation, {{"ok", true}, {"ports", QJsonArray{
    QJsonObject{{"portId", output}, {"encoding", "text/plain"}}
  }}});

  auto *ports = detail.findChild<QTableWidget *>(QStringLiteral("aiOperationDetailPorts"));
  auto *read = detail.findChild<QPushButton *>(QStringLiteral("aiOperationDetailPortRead"));
  auto *next = detail.findChild<QPushButton *>(QStringLiteral("aiOperationDetailPortNext"));
  auto *preview = detail.findChild<QLabel *>(QStringLiteral("aiOperationDetailPortPreview"));
  QVERIFY(ports && read && next && preview);
  QCOMPARE(ports->rowCount(), 1);
  QVERIFY(QMetaObject::invokeMethod(ports, "cellClicked", Q_ARG(int, 0), Q_ARG(int, 0)));
  QVERIFY(read->isEnabled());

  QSignalSpy requests(&detail, &AiOperationDetailWidget::portPageRequested);
  read->click();
  QCOMPARE(requests.size(), 1);
  QCOMPARE(requests.first().at(0).toString(), operation);
  QCOMPARE(requests.first().at(1).toString(), output);
  QCOMPARE(requests.first().at(2).toLongLong(), qint64(0));

  detail.onPortPage(operation, output, {{"ok", true}, {"offset", "3"}, {"nextOffset", "6"},
                                       {"hasMore", false}, {"data", "ZGVm"}});
  QVERIFY(preview->text().isEmpty());
  detail.onPortPage(operation, output, {{"ok", true}, {"offset", "0"}, {"nextOffset", "3"},
                                       {"hasMore", true}, {"data", "YWJj"}});
  QVERIFY(preview->text().startsWith(QStringLiteral("abc")));
  QVERIFY(next->isEnabled());
  next->click();
  QCOMPARE(requests.size(), 2);
  QCOMPARE(requests.last().at(1).toString(), output);
  QCOMPARE(requests.last().at(2).toLongLong(), qint64(3));

  auto *resume = detail.findChild<QPushButton *>(QStringLiteral("aiOperationDetailResume"));
  QVERIFY(resume);
  detail.onBusyChanged(operation, QStringLiteral("resume"), true);
  QVERIFY(!resume->isEnabled());
  detail.openOperation(operation);
  QVERIFY(!resume->isEnabled()); // Reopening the busy operation keeps its state.
  detail.openOperation(QStringLiteral("op-other"));
  QVERIFY(resume->isEnabled());
  detail.onBusyChanged(operation, QStringLiteral("resume"), true);
  QVERIFY(resume->isEnabled()); // The old operation can no longer affect this view.
}

void TestG5ConversationWorkbench::initTestCase() {
  PaleoTheme::pinRenderEnvironment();
  QStandardPaths::setTestModeEnabled(true);
  cleanup();
}

void TestG5ConversationWorkbench::cleanup() {
  for (const QString &id : ChatSessionStore::listIds()) {
    QString error;
    QVERIFY2(ChatSessionStore::remove(id, &error), qPrintable(error));
  }
}

void TestG5ConversationWorkbench::historySessionRestoresToolCardsWithoutModelRequests() {
  const ChatSession session = makeSessionWithToolHistory(2);
  QString error;
  QVERIFY2(ChatSessionStore::save(session, &error), qPrintable(error));
  AiChatController controller; // 未配置：不可能发模型请求
  controller.loadSession(session.id, &error);
  QCOMPARE(controller.messages().size(), session.messages.size()); // 不重写历史

  AiAssistDock dock(&controller);
  auto *cards = dock.findChild<QWidget *>(QStringLiteral("aiAssistantToolCards"));
  QVERIFY(cards);
  const auto cardList =
    cards->findChildren<AiConversationCard *>(QStringLiteral("aiConversationCard"));
  QCOMPARE(cardList.size(), 2); // 每个工具帧恢复一张卡（不再是字符计数行）
  // 卡内容：工具名 + envelope 数据（计数键值对来自 data，不是「N 字符」）。
  QString cardText;
  for (QLabel *label : cardList.first()->findChildren<QLabel *>())
    cardText += label->text() + QLatin1Char('\n');
  QVERIFY2(cardText.contains(QStringLiteral("paleo.query_project")),
           qPrintable(cardText));
  QVERIFY2(!cardText.contains(QStringLiteral("结果已回传模型")),
           "历史卡必须呈现结果，不再只有字符计数");
  cardList.first()->setExpanded(true);
  QVERIFY2(spinUntil([cardList] {
             return !cardList.first()
                       ->findChildren<QWidget *>(QStringLiteral("aiCardResultView"))
                       .isEmpty();
           }),
           "展开必须构建结构化结果视图");
}

void TestG5ConversationWorkbench::historyCardsShowAttributionAndLazyExpand() {
  const ChatSession modern = makeSessionWithToolHistory(1);
  QString error;
  QVERIFY(ChatSessionStore::save(modern, &error));
  AiChatController controller;
  controller.loadSession(modern.id);
  AiAssistDock dock(&controller);
  auto *cards = dock.findChild<QWidget *>(QStringLiteral("aiAssistantToolCards"));
  auto cardList =
    cards->findChildren<AiConversationCard *>(QStringLiteral("aiConversationCard"));
  QCOMPARE(cardList.size(), 1);
  QString text;
  for (QLabel *label : cardList.first()->findChildren<QLabel *>())
    text += label->text() + QLatin1Char('\n');
  QVERIFY2(text.contains(QStringLiteral("turn-0")), qPrintable(text));
  QVERIFY2(text.contains(QStringLiteral("宿主绑定已验证")), qPrintable(text));

  // 旧记录（无 hostTurnId/绑定）：归属未验证，不猜。
  const ChatSession legacy = makeSessionWithToolHistory(1, /*legacyTurnIds=*/true);
  QVERIFY(ChatSessionStore::save(legacy, &error));
  controller.loadSession(legacy.id);
  // 旧卡 deleteLater：等事件队列回收后再数（新会话只留 1 张卡）。
  QVERIFY2(spinUntil([&cards] {
             return cards->findChildren<AiConversationCard *>(
                      QStringLiteral("aiConversationCard"))
                      .size() == 1;
           }),
           "切会话后旧卡必须回收");
  cardList =
    cards->findChildren<AiConversationCard *>(QStringLiteral("aiConversationCard"));
  QCOMPARE(cardList.size(), 1);
  text.clear();
  for (QLabel *label : cardList.first()->findChildren<QLabel *>())
    text += label->text() + QLatin1Char('\n');
  QVERIFY2(text.contains(QStringLiteral("归属未验证")), qPrintable(text));
}

void TestG5ConversationWorkbench::boundedCardWindowKeepsOlderResultsAccessible() {
  const int total = kAiTestCardTotal; // 100 张：超过单窗口上限
  const ChatSession session = makeSessionWithToolHistory(total);
  QString error;
  QVERIFY(ChatSessionStore::save(session, &error));
  AiChatController controller;
  controller.loadSession(session.id);
  AiAssistDock dock(&controller);
  auto *cards = dock.findChild<QWidget *>(QStringLiteral("aiAssistantToolCards"));
  auto cardList =
    cards->findChildren<AiConversationCard *>(QStringLiteral("aiConversationCard"));
  QVERIFY2(cardList.size() <= 64,
           qPrintable(QStringLiteral("活跃卡必须 ≤64，实际 %1").arg(cardList.size())));
  auto *older =
    dock.findChild<QPushButton *>(QStringLiteral("aiAssistantHistoryOlder"));
  auto *newer =
    dock.findChild<QPushButton *>(QStringLiteral("aiAssistantHistoryNewer"));
  QVERIFY(older && older->isVisibleTo(&dock));
  QVERIFY(!newer->isVisibleTo(&dock)); // 最新一窗：无「更新」方向
  older->click(); // 翻到更早窗口：旧结果仍可达，消息总数不变。
  QVERIFY2(spinUntil([&cards] {
             return cards->findChildren<AiConversationCard *>(
                      QStringLiteral("aiConversationCard"))
                      .size() <= 64;
           }),
           "翻页后仍受窗口上限约束");
  cardList =
    cards->findChildren<AiConversationCard *>(QStringLiteral("aiConversationCard"));
  QVERIFY(newer->isVisibleTo(&dock));
  QCOMPARE(controller.messages().size(), session.messages.size()); // 不删消息
}

void TestG5ConversationWorkbench::draftsSurviveRejectionAndRoundTripAcrossSessions() {
  FakeLlmServer server;
  QVERIFY(server.start());
  AiChatController controller; // 先未配置
  AiAssistDock dock(&controller);
  auto *input = dock.findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"));
  auto *send = dock.findChild<QPushButton *>(QStringLiteral("aiAssistantSend"));
  QVERIFY(input && send);
  // 未配置：sendUserText 拒绝 → 文本保留（不吞草稿），错误上屏。
  input->setPlainText(QStringLiteral("这段岩性如何？"));
  send->click();
  QCOMPARE(input->toPlainText(), QStringLiteral("这段岩性如何？"));

  // 配置后：接受才清空。
  controller.setConfig(makeConfig(server.endpoint()));
  dock.refresh();
  QVERIFY(send->isEnabled());
  send->click();
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }),
           "一轮必须结束");
  QVERIFY(input->toPlainText().isEmpty()); // 已接受 → 草稿清理

  // 草稿 round-trip：会话 A 输入未发送文本 → 新会话为空 → 回 A 恢复。
  input->setPlainText(QStringLiteral("草稿保留测试"));
  // 宿主接线（生产在 paleomainwindow_attach_shell，测试同路径连接）。
  connect(&dock, &AiAssistDock::openSessionRequested, &controller,
          [&controller](const QString &id) { controller.loadSession(id); });
  dock.findChild<QPushButton *>(QStringLiteral("aiAssistantNew"))->click();
  QVERIFY(input->toPlainText().isEmpty()); // 新会话无草稿
  auto *combo = dock.findChild<QComboBox *>(QStringLiteral("aiAssistantSessions"));
  const int index = combo->findText(QStringLiteral("这段岩性如何？"));
  QVERIFY(index > 0);
  QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, index));
  QVERIFY2(spinUntil([input] {
             return input->toPlainText() == QStringLiteral("草稿保留测试");
           }),
           "切回会话必须恢复未发送草稿");
}

void TestG5ConversationWorkbench::enterSendsShiftEnterBreaks() {
  FakeLlmServer server;
  QVERIFY(server.start());
  AiChatController controller;
  controller.setConfig(makeConfig(server.endpoint()));
  AiAssistDock dock(&controller);
  auto *input = dock.findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"));
  QVERIFY(input);
  input->setPlainText(QStringLiteral("Enter"));
  QTest::keyClick(input, Qt::Key_Return, Qt::NoModifier);
  QVERIFY2(spinUntil([&controller, &server, input] {
             return !controller.streaming() && server.requestCount() == 1 &&
                    input->toPlainText().isEmpty();
           }),
           "Enter 必须发送一次并清空（接受后）");
  // Shift+Enter：换行不发送。
  input->setPlainText(QStringLiteral("第一行"));
  // 光标移到行尾（用户输入后的自然位置）再 Shift+Enter。
  QTextCursor cursor = input->textCursor();
  cursor.movePosition(QTextCursor::End);
  input->setTextCursor(cursor);
  QTest::keyClick(input, Qt::Key_Return, Qt::ShiftModifier);
  QCOMPARE(input->toPlainText(), QStringLiteral("第一行\n"));
  QCOMPARE(server.requestCount(), 1); // 换行不产生新请求
}

void TestG5ConversationWorkbench::contextBarShowsBriefAndUpdate() {
  AiChatController controller;
  AiAssistDock dock(&controller);
  auto *summary =
    dock.findChild<QLabel *>(QStringLiteral("aiContextSummary"));
  QVERIFY(summary);
  controller.setProjectBrief(QStringLiteral("工程：井 3 口 · 层位 2 套"));
  QVERIFY2(summary->text().contains(QStringLiteral("井 3 口")),
           qPrintable(summary->text()));
  // 无 adapter：详情里如实说明操作服务未接入。
  auto *toggle = dock.findChild<QToolButton *>(QStringLiteral("aiContextToggle"));
  auto *body = dock.findChild<QLabel *>(QStringLiteral("aiContextDetailBody"));
  QVERIFY(toggle && body);
  toggle->setChecked(true);
  QVERIFY2(body->text().contains(QStringLiteral("操作服务未接入")),
           qPrintable(body->text()));
  // 无 adapter 时工程状态未知：不冒称「工程未打开」，操作服务未接入单独陈述。
  QVERIFY2(!summary->text().contains(QStringLiteral("工程未打开")),
           "工程可能开着，不得冒称未打开");
  QVERIFY2(summary->text().contains(QStringLiteral("操作服务未接入")),
           qPrintable(summary->text()));
}

void TestG5ConversationWorkbench::sentQuestionsKeepTheirOwnWorkspace() {
  FakeLlmServer server;
  QVERIFY(server.start());
  AiChatController controller;
  controller.setConfig(makeConfig(server.endpoint()));
  AiAssistDock dock(&controller);
  paleo::capability::CapabilityHost host;
  paleo::capability::HostBinding binding;
  binding.context.documentId = QStringLiteral("project-A");
  binding.context.open = true;
  binding.context.readOnly = true;
  binding.context.horizon = QStringLiteral("D61");
  binding.context.unitId = QStringLiteral("D61-D62");
  host.setBindingProvider([&binding] { return binding; });
  paleo::chat::AiOperationPresentation presenter({&controller, &host, nullptr});
  dock.attachOperationPresenter(&presenter);
  QString version = QStringLiteral("主图 A · v3");
  dock.setWorkspaceLabels([&version] { return QStringList{version}; });
  auto *input = dock.findChild<QTextEdit *>(QStringLiteral("aiAssistantInput"));
  auto *send = dock.findChild<QPushButton *>(QStringLiteral("aiAssistantSend"));
  QVERIFY(input && send);
  const QString question = QStringLiteral("这个图斑的依据是什么？");
  input->setPlainText(question);
  send->click();
  QVERIFY(spinUntil([&] { return server.requestCount() == 1 && !controller.streaming(); }));
  const QString first = server.lastUserMessage();
  for (const auto &item : QStringList{question, version, QStringLiteral("D61-D62"), QStringLiteral("工程只读"),
                                     binding.context.documentId}) QVERIFY(first.contains(item));
  QCOMPARE(controller.session().title, question);
  binding.context.readOnly = false;
  binding.context.horizon = QStringLiteral("C6");
  binding.context.unitId.clear();
  version = QStringLiteral("主图 B · v1");
  input->setPlainText(question);
  send->click();
  QVERIFY(spinUntil([&] { return server.requestCount() == 2 && !controller.streaming(); }));
  const auto next = server.lastUserMessage();
  QVERIFY(next.contains(version) && next.contains(QStringLiteral("工程可写")) && next.contains(QStringLiteral("C6")));
  QVERIFY(!next.contains(QStringLiteral("D61")));
  QCOMPARE(controller.session().messages.first().content, first);
  const auto restored = ChatSession::fromJson(controller.session().toJson());
  QCOMPARE(restored.messages.first().content, first);
}

void TestG5ConversationWorkbench::attachPresenterIdempotentAndNullPresenterKeepsReason() {
  AiChatController controller;
  AiAssistDock dock(&controller);
  paleo::chat::AiOperationPresentation *presenter =
    new paleo::chat::AiOperationPresentation(
      paleo::chat::AiOperationPresentation::Deps{&controller, nullptr, nullptr}, this);
  dock.attachOperationPresenter(presenter);
  dock.attachOperationPresenter(presenter); // 幂等：同指针直接返回
  auto *body = dock.findChild<QLabel *>(QStringLiteral("aiContextDetailBody"));
  auto *toggle = dock.findChild<QToolButton *>(QStringLiteral("aiContextToggle"));
  toggle->setChecked(true);
  QVERIFY2(body->text().contains(QStringLiteral("operation_host_or_session_unavailable")),
           qPrintable(body->text()));
  dock.attachOperationPresenter(nullptr); // 解绑后回到「未接入」事实
  QVERIFY2(body->text().contains(QStringLiteral("操作服务未接入")),
           qPrintable(body->text()));
  delete presenter;
}

QTEST_MAIN(TestG5ConversationWorkbench)
#include "tst_g5_conversation_workbench.moc"
