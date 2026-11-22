// 层：测试壳
// H1 装配验收（aichatcontroller sendRound 接缝）：驱动真实
// sendUserText/retry/工具续轮，捕获**实际 HTTP 请求体**——单一宿主 System、
// 空占位不上送、连续后缀、legacy 终形 envelope、当前轮拒绝零 HTTP、
// 原档案不改动，以及一条受控 Pi peer 编排链与原生链同一投影 oracle。
// 组件语义矩阵在 tst_h1_contextprojection，此处只验收生产装配路径。
#include "workflow/aichatcontroller.h"
#include "workflow/aichatcontextprojection.h"
#include "workflow/capabilityhost.h"
#include "catalog/datacatalog.h"
#include "services/paleotaskservice.h"
#include "services/piconversation.h"
#include "services/piprotocol.h"
#include "services/toolresultprotocol.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>
#include <algorithm>
#include <functional>
#include <memory>

namespace {

// 逐请求弹脚本应答的假端点（同 tst_aichattoolloop 的口径；捕获完整请求体）。
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
  QByteArray rawBody(int index) const { return m_bodies.value(index); }
  QJsonObject body(int index) const {
    return QJsonDocument::fromJson(rawBody(index)).object();
  }

private:
  void serve(QTcpSocket *socket) {
    if (!socket)
      return;
    connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    auto buffer = std::make_shared<QByteArray>();
    connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
      buffer->append(socket->readAll());
      const int headerEnd = buffer->indexOf("\r\n\r\n");
      if (headerEnd < 0)
        return;
      const QString header = QString::fromUtf8(buffer->left(headerEnd));
      const int contentLength =
        header.section("Content-Length:", 1, 1, QString::SectionCaseInsensitiveSeps)
          .section("\r", 0, 0)
          .trimmed()
          .toInt();
      if (buffer->mid(headerEnd + 4).size() < contentLength)
        return;
      const QByteArray bodyBytes = buffer->mid(headerEnd + 4, contentLength);
      *buffer = buffer->mid(headerEnd + 4 + contentLength);
      m_bodies.append(bodyBytes);
      if (m_script.isEmpty())
        return; // 挂起不答（「拒绝后零请求」断言用）
      const QByteArray response = m_script.takeFirst();
      socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\n"
                    "Content-Type: text/event-stream\r\n\r\n");
      QTimer::singleShot(10, socket, [socket, response] {
        socket->write(response);
        socket->flush();
        socket->disconnectFromHost();
      });
    });
  }
  QTcpServer m_server;
  QVector<QByteArray> m_bodies;
  QVector<QByteArray> m_script;
};

QByteArray sseEvent(const QJsonObject &event) {
  return "data: " + QJsonDocument(event).toJson(QJsonDocument::Compact) + "\n\n";
}

QByteArray doneTail() {
  QJsonObject choice;
  choice.insert(QStringLiteral("delta"), QJsonObject());
  QJsonObject event;
  event.insert(QStringLiteral("choices"), QJsonArray{choice});
  QByteArray out = sseEvent(event);
  QJsonObject usageDone;
  usageDone.insert(QStringLiteral("prompt_tokens"), 60);
  usageDone.insert(QStringLiteral("completion_tokens"), 10);
  QJsonObject choiceDone;
  choiceDone.insert(QStringLiteral("delta"), QJsonObject());
  QJsonObject eventDone;
  eventDone.insert(QStringLiteral("choices"), QJsonArray{choiceDone});
  eventDone.insert(QStringLiteral("usage"), usageDone);
  out += sseEvent(eventDone) + QByteArrayLiteral("data: [DONE]\n\n");
  return out;
}

QByteArray textAnswerSse(const QString &text) {
  const QStringList halves = {text.left(text.size() / 2), text.mid(text.size() / 2)};
  QByteArray out;
  for (const QString &half : halves) {
    QJsonObject delta;
    delta.insert(QStringLiteral("content"), half);
    QJsonObject choice;
    choice.insert(QStringLiteral("delta"), delta);
    QJsonObject event;
    event.insert(QStringLiteral("choices"), QJsonArray{choice});
    out += sseEvent(event);
  }
  return out + doneTail();
}

ChatMessage message(ChatRole role, const QString &content) {
  ChatMessage out;
  out.role = role;
  out.content = content;
  return out;
}

void appendPlainTurn(ChatSession &session, const QString &tag) {
  session.messages.append(message(ChatRole::User, QStringLiteral("%1问").arg(tag)));
  session.messages.append(message(ChatRole::Assistant, QStringLiteral("%1答").arg(tag)));
}

int countRole(const QVector<ChatMessage> &messages, ChatRole role) {
  int total = 0;
  for (const ChatMessage &item : messages)
    if (item.role == role)
      ++total;
  return total;
}

// G2 受控 peer 同款（见 tst_g2_turncheckpoint）：paleo.pi-conversation.v1 的
// peer 侧，驱动一条「模型轮 → 终答 → idle ok」的最短编排链。
class ControlledPeer {
public:
  QString run(PaleoTask *task, const std::shared_ptr<paleo::pi::ConversationChannel> &channel,
              const std::function<void()> &notify) {
    QByteArray buffer;
    QElapsedTimer clock;
    clock.start();
    bool stopping = false, idleSent = false, closed = false;
    quint64 peerSequence = 0;
    const auto peer = [&](const QString &type, const QJsonObject &payload) {
      bool wake = false;
      const auto &identity = channel->identity();
      const QJsonObject envelope{{"protocol", QLatin1String(paleo::pi::ConversationProtocol)},
                                 {"type", type},
                                 {"sessionId", identity.sessionId},
                                 {"generation", identity.generation},
                                 {"turnId", identity.turnId},
                                 {"nonce", identity.nonce},
                                 {"sequence", QString::number(++peerSequence)},
                                 {"payload", payload}};
      if (!channel->acceptPeer(envelope, wake))
        return false;
      if (wake && notify)
        notify();
      return true;
    };
    while (!closed) {
      if (clock.elapsed() > 60000)
        return QStringLiteral("test_driver_timeout");
      const auto channelError = channel->error();
      if (!channelError.isEmpty() && channelError != QLatin1String("pi_conversation_busy"))
        return channelError;
      if (!stopping && (task->cancelRequested() || channel->stopReason()))
        stopping = true;
      if (stopping && !idleSent) {
        idleSent = true;
        if (!peer("idle", {{ "code", channel->stopReason() == 1
                                          ? QStringLiteral("cancelled")
                                          : QStringLiteral("interrupted") }}))
          return channel->error();
      }
      const auto outgoing = channel->takeOutgoing();
      if (!outgoing.isEmpty())
        buffer += outgoing;
      QJsonObject frame;
      bool complete = false;
      QString error;
      while (paleo::pi::takeFrame(buffer, frame, complete, error)) {
        if (!complete)
          break;
        const auto type = frame.value("type").toString();
        const auto payload = frame.value("payload").toObject();
        if (type == QLatin1String("open")) {
          if (!peer("opened", {{ "upstreamCommit", "u" }, { "coreVersion", "c" },
                               { "nodeVersion", "n" }, { "toolExecution", "sequential" },
                               { "modelTransport", "host-only" }, { "sandboxed", false }}))
            return channel->error();
        } else if (type == QLatin1String("tool_definition")) {
          if (!peer("configured", {{ "name", payload.value("name").toString() }}))
            return channel->error();
        } else if (type == QLatin1String("start")) {
          if (!peer("model_request", {{ "round", QStringLiteral("1") }}))
            return channel->error();
        } else if (type == QLatin1String("model_end")) {
          if (!peer("idle", {{ "code", QStringLiteral("ok") }}))
            return channel->error();
        } else if (type == QLatin1String("close")) {
          if (!peer("closed", {}))
            return channel->error();
          closed = true;
          break;
        }
      }
      QThread::msleep(2);
    }
    return {};
  }
};

struct Host {
  QTemporaryDir project;
  DataCatalog catalog;
  PaleoTaskService tasks;
  paleo::capability::CapabilityHost capability{paleo::capability::NativeAdapterFactory{}, nullptr};
  ~Host() { tasks.shutdown(10000, true); }
  QString bindError;
  bool bind(AiChatController &controller) {
    if (!catalog.open(project.path(), &bindError))
      return false;
    tasks.setMaxWorkerThreads(3);
    capability.setTaskService(&tasks);
    capability.setCallerScope(paleo::host::Caller::Agent, "agent:h1-test");
    capability.setBindingProvider([this] {
      paleo::capability::HostBinding binding;
      binding.catalog = &catalog;
      binding.projectDir = project.path();
      binding.context.documentId = "host-document";
      binding.context.epoch = 1;
      binding.context.open = true;
      binding.context.persistentIdentity = true;
      binding.context.readOnly = true;
      binding.context.allowedEffects = paleo::host::Read;
      return binding;
    });
    controller.toolRunner()->setTaskService(&tasks);
    AiChatToolContext context;
    context.catalog = &catalog;
    context.projectDir = project.path();
    context.documentId = "host-document";
    context.documentEpoch = 1;
    context.capabilityHost = &capability;
    context.requireCapabilityHost = true;
    controller.toolRunner()->setContext(context);
    return true;
  }
};

} // namespace

class TestH1RequestAssembly : public QObject {
  Q_OBJECT
private slots:
  void initTestCase();
  void cleanup();

  void basicRoundSendsSingleSystemWithoutPlaceholder();
  void legacyHistorySanitizedOnWireAndArchiveUntouched();
  void damagedOlderTurnExcludedOnWireWithContiguousSuffix();
  void conversionPricedTurnDropsOnWire();
  void currentTurnOverBudgetStopsRoundWithZeroRequests();
  void overBudgetMiddleTurnNotSkippedForOlderTurn();
  void retryLastReprojectsCleanSuffix();
  void controlledPiPeerChainMatchesProjectionOracle();

private:
  static bool spinUntil(const std::function<bool()> &done, int timeoutMs = 15000) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < timeoutMs)
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
  }
  LlmConfig baseConfig(const QUrl &endpoint) const {
    LlmConfig config;
    config.endpoint = endpoint;
    config.model = QStringLiteral("h1-assembly-model");
    config.stream = true;
    config.timeoutMs = 5000;
    config.apiKey = "sk-h1-fixture";
    return config;
  }
  void dropSession(const QString &id) {
    if (id.isEmpty())
      return;
    QString error;
    ChatSessionStore::remove(id, &error);
  }
  QStringList m_createdSessions;
};

void TestH1RequestAssembly::initTestCase() {
  QStandardPaths::setTestModeEnabled(true);
  QDir(ChatSessionStore::directory()).removeRecursively();
}

void TestH1RequestAssembly::cleanup() {
  for (const QString &id : m_createdSessions)
    dropSession(id);
  m_createdSessions.clear();
}

// 真实 HTTP 链 oracle 1：一轮普通对话的请求体——唯一宿主 System、末条是
// 用户问题（空 assistant 占位不作为协议消息上送）。
void TestH1RequestAssembly::basicRoundSendsSingleSystemWithoutPlaceholder() {
  ScriptedLlmServer server;
  QVERIFY(server.start());
  server.enqueue(textAnswerSse(QStringLiteral("好的。")));
  AiChatController controller;
  controller.setConfig(baseConfig(server.endpoint()));
  m_createdSessions.append(controller.session().id);
  ChatSession stored;
  stored.id = ChatSessionStore::newSessionId();
  stored.createdAt = stored.updatedAt = QDateTime::currentDateTime();
  appendPlainTurn(stored, QStringLiteral("一"));
  appendPlainTurn(stored, QStringLiteral("二"));
  QString error;
  QVERIFY2(ChatSessionStore::save(stored, &error), qPrintable(error));
  QVERIFY2(controller.loadSession(stored.id, &error), qPrintable(error));
  controller.sendUserText(QStringLiteral("新问题"));
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }), "必须收敛");
  QCOMPARE(server.requestCount(), 1);
  const QJsonArray messages =
    server.body(0).value(QStringLiteral("messages")).toArray();
  int systemCount = 0, assistantCount = 0;
  for (const QJsonValue &value : messages) {
    const QJsonObject entry = value.toObject();
    if (entry.value(QStringLiteral("role")).toString() == QStringLiteral("system"))
      ++systemCount;
    if (entry.value(QStringLiteral("role")).toString() == QStringLiteral("assistant"))
      ++assistantCount;
  }
  QCOMPARE(systemCount, 1);
  QVERIFY(messages.first().toObject().value(QStringLiteral("content"))
            .toString()
            .contains(QStringLiteral("地质解释助手")));
  // 上送历史的 assistant 只有存储里的「一答/二答」；空占位不上送。
  QCOMPARE(assistantCount, 2);
  QCOMPARE(messages.last().toObject().value(QStringLiteral("role")).toString(),
           QStringLiteral("user"));
  QCOMPARE(messages.last().toObject().value(QStringLiteral("content")).toString(),
           QStringLiteral("新问题"));
}

// oracle 2：旧档案（存储 System + legacy 工具结果 + 中段损坏轮）——线上唯一
// System、legacy 已按终形 envelope 上送、损坏轮整轮排除且后缀连续；原档案
// 字节与内存消息不被投影改写（新增消息只按既有追加语义出现）。
void TestH1RequestAssembly::legacyHistorySanitizedOnWireAndArchiveUntouched() {
  ScriptedLlmServer server;
  QVERIFY(server.start());
  server.enqueue(textAnswerSse(QStringLiteral("好的。")));
  AiChatController controller;
  controller.setConfig(baseConfig(server.endpoint()));
  ChatSession stored;
  stored.id = ChatSessionStore::newSessionId();
  stored.createdAt = stored.updatedAt = QDateTime::currentDateTime();
  stored.messages.append(message(ChatRole::System,
                                 QStringLiteral("旧档案里的高位 System 指令")));
  // 损坏轮：孤儿应答（整轮排除；其后的完好轮照常进后缀）。
  stored.messages.append(message(ChatRole::User, QStringLiteral("坏轮")));
  stored.messages.append(message(ChatRole::Tool, QStringLiteral("{}")));
  stored.messages.back().toolCallId = QStringLiteral("ghost");
  // 完好 legacy 轮：合法 JSON 工具结果（非 envelope），位于保留后缀内。
  stored.messages.append(message(ChatRole::User, QStringLiteral("旧问")));
  ChatMessage callFrame;
  callFrame.role = ChatRole::Assistant;
  callFrame.toolCalls.append({"old-call", "paleo.query_project", "{\"topic\":\"wells\"}"});
  stored.messages.append(callFrame);
  stored.messages.append(message(ChatRole::Tool, QStringLiteral("{\"wells\":[\"W-1\"]}")));
  stored.messages.back().toolCallId = QStringLiteral("old-call");
  appendPlainTurn(stored, QStringLiteral("近"));
  QString error;
  QVERIFY2(ChatSessionStore::save(stored, &error), qPrintable(error));
  m_createdSessions.append(stored.id);
  QByteArray onDiskBefore;
  {
    QFile file(ChatSessionStore::pathFor(stored.id));
    QVERIFY(file.open(QIODevice::ReadOnly));
    onDiskBefore = file.readAll();
  }
  QVERIFY2(controller.loadSession(stored.id, &error), qPrintable(error));
  controller.sendUserText(QStringLiteral("新问题"));
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }), "必须收敛");
  QCOMPARE(server.requestCount(), 1);
  const QJsonArray messages =
    server.body(0).value(QStringLiteral("messages")).toArray();
  int systemCount = 0;
  bool sawEnvelope = false, sawGhost = false;
  QStringList users;
  for (const QJsonValue &value : messages) {
    const QJsonObject entry = value.toObject();
    const QString role = entry.value(QStringLiteral("role")).toString();
    if (role == QStringLiteral("system")) {
      ++systemCount;
      QVERIFY(!entry.value(QStringLiteral("content"))
                .toString()
                .contains(QStringLiteral("高位 System")));
    }
    if (role == QStringLiteral("user"))
      users.append(entry.value(QStringLiteral("content")).toString());
    if (entry.value(QStringLiteral("tool_call_id")).toString() ==
        QStringLiteral("old-call")) {
      const auto payload = QJsonDocument::fromJson(
                             entry.value(QStringLiteral("content")).toString().toUtf8()).object();
      QVERIFY(paleo::chat::isResultEnvelope(payload));
      sawEnvelope = true;
    }
    if (entry.value(QStringLiteral("tool_call_id")).toString() == QStringLiteral("ghost"))
      sawGhost = true;
  }
  QCOMPARE(systemCount, 1);
  QVERIFY(sawEnvelope);
  QVERIFY(!sawGhost);
  // 连续后缀：坏轮排除（及其之前的 System 引导段），旧 legacy 轮、近轮、
  // 新问题照常进后缀。
  const QStringList expected{QStringLiteral("旧问"), QStringLiteral("近问"),
                             QStringLiteral("新问题")};
  QCOMPARE(users, expected);
  // 内存/磁盘原史未改写：存储侧原消息仍在（追加只来自本回合新问答）。
  ChatSession reloaded;
  QVERIFY2(ChatSessionStore::load(stored.id, &reloaded, &error), qPrintable(error));
  QVERIFY(reloaded.messages.size() >= stored.messages.size());
  for (int index = 0; index < stored.messages.size(); ++index) {
    QCOMPARE(reloaded.messages.at(index).role, stored.messages.at(index).role);
    QCOMPARE(reloaded.messages.at(index).content, stored.messages.at(index).content);
    QCOMPARE(reloaded.messages.at(index).toolCallId, stored.messages.at(index).toolCallId);
  }
}

// oracle 3：损坏中轮 + 短旧轮——线上只发连续后缀（不跳过损坏轮捞旧轮）。
void TestH1RequestAssembly::damagedOlderTurnExcludedOnWireWithContiguousSuffix() {
  ScriptedLlmServer server;
  QVERIFY(server.start());
  server.enqueue(textAnswerSse(QStringLiteral("好的。")));
  AiChatController controller;
  controller.setConfig(baseConfig(server.endpoint()));
  ChatSession stored;
  stored.id = ChatSessionStore::newSessionId();
  stored.createdAt = stored.updatedAt = QDateTime::currentDateTime();
  appendPlainTurn(stored, QStringLiteral("最旧"));
  stored.messages.append(message(ChatRole::User, QStringLiteral("坏轮")));
  stored.messages.append(message(ChatRole::Tool, QStringLiteral("{}")));
  stored.messages.back().toolCallId = QStringLiteral("ghost");
  appendPlainTurn(stored, QStringLiteral("近"));
  QString error;
  QVERIFY2(ChatSessionStore::save(stored, &error), qPrintable(error));
  m_createdSessions.append(stored.id);
  QVERIFY2(controller.loadSession(stored.id, &error), qPrintable(error));
  controller.sendUserText(QStringLiteral("新问题"));
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }), "必须收敛");
  QCOMPARE(server.requestCount(), 1);
  QStringList users;
  const auto messages = server.body(0).value(QStringLiteral("messages")).toArray();
  for (const QJsonValue &value : messages) {
    const auto entry = value.toObject();
    if (entry.value(QStringLiteral("role")).toString() == QStringLiteral("user"))
      users.append(entry.value(QStringLiteral("content")).toString());
  }
  const QStringList expected{QStringLiteral("近问"), QStringLiteral("新问题")};
  QCOMPARE(users, expected);
}

// oracle 4：legacy 轮按原 content 计价可纳入、终形 envelope 计价超限——线上
// 整轮截断（预算按实际发送形态收费）。
void TestH1RequestAssembly::conversionPricedTurnDropsOnWire() {
  ScriptedLlmServer server;
  QVERIFY(server.start());
  server.enqueue(textAnswerSse(QStringLiteral("好的。")));
  AiChatController controller;
  controller.setConfig(baseConfig(server.endpoint()));
  // 12 组调用 × legacy 结果：原 content 计价（每组 ~473 估算 token）合计
  // ~5700 进得了 6000 窗；终形 envelope（协议结构 + ok/data/transport 包装
  // + 引号转义）合计 ~6300 超窗——转换后计价必须整轮截断。
  ChatSession stored;
  stored.id = ChatSessionStore::newSessionId();
  stored.createdAt = stored.updatedAt = QDateTime::currentDateTime();
  stored.messages.append(message(ChatRole::User, QStringLiteral("多结果轮")));
  ChatMessage callFrame;
  const QString payload = QStringLiteral("{\"logs\":\"%1\"}").arg(QString(470, QChar(0x5ca9)));
  for (int index = 0; index < 12; ++index)
    callFrame.toolCalls.append({QStringLiteral("batch-%1").arg(index),
                                QStringLiteral("paleo.query_project"),
                                QStringLiteral("{\"topic\":\"wells\"}")});
  stored.messages.append(callFrame);
  for (int index = 0; index < 12; ++index) {
    stored.messages.append(message(ChatRole::Tool, payload));
    stored.messages.back().toolCallId = QStringLiteral("batch-%1").arg(index);
  }
  appendPlainTurn(stored, QStringLiteral("近"));
  QString error;
  QVERIFY2(ChatSessionStore::save(stored, &error), qPrintable(error));
  m_createdSessions.append(stored.id);
  QVERIFY2(controller.loadSession(stored.id, &error), qPrintable(error));
  controller.sendUserText(QStringLiteral("新问题"));
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }), "必须收敛");
  QCOMPARE(server.requestCount(), 1);
  QStringList users;
  bool sawBatchTool = false;
  const auto messages = server.body(0).value(QStringLiteral("messages")).toArray();
  for (const QJsonValue &value : messages) {
    const auto entry = value.toObject();
    if (entry.value(QStringLiteral("role")).toString() == QStringLiteral("user"))
      users.append(entry.value(QStringLiteral("content")).toString());
    if (entry.value(QStringLiteral("tool_call_id")).toString().startsWith(
          QStringLiteral("batch-")))
      sawBatchTool = true;
  }
  QVERIFY(!sawBatchTool);
  const QStringList expected{QStringLiteral("近问"), QStringLiteral("新问题")};
  QCOMPARE(users, expected);
}

// oracle 5：当前必需轮超历史预算——零 HTTP、既有停轮路径收口（无残留假
// 成功回答、用户问题保留在会话、状态回非流式）。
void TestH1RequestAssembly::currentTurnOverBudgetStopsRoundWithZeroRequests() {
  ScriptedLlmServer server;
  QVERIFY(server.start());
  AiChatController controller;
  controller.setConfig(baseConfig(server.endpoint()));
  m_createdSessions.append(controller.session().id);
  QSignalSpy errors(&controller, &AiChatController::errorOccurred);
  controller.sendUserText(QString(AiChatController::kHistoryTokenBudget + 80, QChar(0x5ca9)));
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }), "必须停轮");
  QCOMPARE(server.requestCount(), 0);
  QVERIFY2(!errors.isEmpty(), "超限必须如实报错，不冒充回答");
  QVERIFY2(errors.first().first().toString().contains(QStringLiteral("请求预算")) &&
               errors.first().first().toString().contains(
                 QStringLiteral("current_turn_exceeds_history_budget")),
           qPrintable(errors.first().first().toString()));
  // 用户问题保留；无假成功回答（assistant 数为 0——占位已撤）。
  QCOMPARE(countRole(controller.messages(), ChatRole::User), 1);
  QCOMPARE(countRole(controller.messages(), ChatRole::Assistant), 0);
  QVERIFY(controller.canRetry());
}

// oracle 6：短旧轮—巨大中间轮—短当前轮——巨大轮截断后不得跳过去捞短旧轮。
void TestH1RequestAssembly::overBudgetMiddleTurnNotSkippedForOlderTurn() {
  ScriptedLlmServer server;
  QVERIFY(server.start());
  server.enqueue(textAnswerSse(QStringLiteral("好的。")));
  AiChatController controller;
  controller.setConfig(baseConfig(server.endpoint()));
  ChatSession stored;
  stored.id = ChatSessionStore::newSessionId();
  stored.createdAt = stored.updatedAt = QDateTime::currentDateTime();
  appendPlainTurn(stored, QStringLiteral("短旧"));
  stored.messages.append(
    message(ChatRole::User, QString(AiChatController::kHistoryTokenBudget + 50, QChar(0x5ca9))));
  stored.messages.append(message(ChatRole::Assistant, QStringLiteral("巨答")));
  appendPlainTurn(stored, QStringLiteral("短近"));
  QString error;
  QVERIFY2(ChatSessionStore::save(stored, &error), qPrintable(error));
  m_createdSessions.append(stored.id);
  QVERIFY2(controller.loadSession(stored.id, &error), qPrintable(error));
  controller.sendUserText(QStringLiteral("新问题"));
  QVERIFY2(spinUntil([&controller] { return !controller.streaming(); }), "必须收敛");
  QCOMPARE(server.requestCount(), 1);
  QStringList users;
  const auto messages = server.body(0).value(QStringLiteral("messages")).toArray();
  for (const QJsonValue &value : messages) {
    const auto entry = value.toObject();
    if (entry.value(QStringLiteral("role")).toString() == QStringLiteral("user"))
      users.append(entry.value(QStringLiteral("content")).toString());
  }
  const QStringList expected{QStringLiteral("短近问"), QStringLiteral("新问题")};
  QCOMPARE(users, expected);
}

// oracle 7：retryLast 截掉旧回答后重发——投影看到的是截断后的净后缀，
// 请求体不包含被截回答。
void TestH1RequestAssembly::retryLastReprojectsCleanSuffix() {
  ScriptedLlmServer server;
  QVERIFY(server.start());
  server.enqueue(textAnswerSse(QStringLiteral("第一次答。")));
  server.enqueue(textAnswerSse(QStringLiteral("重试答。")));
  AiChatController controller;
  controller.setConfig(baseConfig(server.endpoint()));
  m_createdSessions.append(controller.session().id);
  controller.sendUserText(QStringLiteral("问题甲"));
  QVERIFY2(spinUntil([&] { return server.requestCount() == 1 && !controller.streaming(); }),
           "第一轮必须完成");
  controller.retryLast();
  QVERIFY2(spinUntil([&] { return server.requestCount() == 2 && !controller.streaming(); }),
           "重试轮必须完成");
  const auto messages =
    server.body(1).value(QStringLiteral("messages")).toArray();
  QStringList users, assistants;
  for (const QJsonValue &value : messages) {
    const auto entry = value.toObject();
    const QString role = entry.value(QStringLiteral("role")).toString();
    if (role == QStringLiteral("user"))
      users.append(entry.value(QStringLiteral("content")).toString());
    if (role == QStringLiteral("assistant"))
      assistants.append(entry.value(QStringLiteral("content")).toString());
  }
  QCOMPARE(users, QStringList{QStringLiteral("问题甲")});
  QVERIFY(assistants.isEmpty()); // 被截回答不再上送，空占位也不上送
}

// oracle 8：受控 Pi peer 编排链（真实模型 HTTP 仍经 LlmClient）与原生链同
// 一投影：单一宿主 System、无空占位、连续后缀。真实 Node runtime 不在测试
// 面（明确区分受控 peer 与未运行的真进程）。
void TestH1RequestAssembly::controlledPiPeerChainMatchesProjectionOracle() {
  Host host;
  ScriptedLlmServer server;
  QVERIFY(server.start());
  server.enqueue(textAnswerSse(QStringLiteral("pi 链终答。")));
  AiChatController controller;
  QVERIFY2(host.bind(controller), qPrintable(host.bindError));
  controller.setConfig(baseConfig(server.endpoint()));
  m_createdSessions.append(controller.session().id);
  controller.setPiRuntime({true, {}, {}});
  ControlledPeer peer;
  controller.piBridge()->setWorkerDriver([&peer](PaleoTask *task,
      const std::shared_ptr<paleo::pi::ConversationChannel> &channel,
      const std::function<void()> &notify) { return peer.run(task, channel, notify); });
  QSignalSpy errors(&controller, &AiChatController::errorOccurred);
  controller.sendUserText(QStringLiteral("pi 链问题"));
  QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 60000);
  QVERIFY2(errors.isEmpty(),
           errors.isEmpty() ? "" : qPrintable(errors.first().first().toString()));
  QCOMPARE(server.requestCount(), 1);
  const auto messages =
    server.body(0).value(QStringLiteral("messages")).toArray();
  int systemCount = 0, assistantCount = 0;
  for (const QJsonValue &value : messages) {
    const auto entry = value.toObject();
    const QString role = entry.value(QStringLiteral("role")).toString();
    if (role == QStringLiteral("system"))
      ++systemCount;
    if (role == QStringLiteral("assistant"))
      ++assistantCount;
  }
  QCOMPARE(systemCount, 1);
  QCOMPARE(assistantCount, 0); // 无空占位、无残留 assistant
  QCOMPARE(messages.last().toObject().value(QStringLiteral("role")).toString(),
           QStringLiteral("user"));
  // 线上体与纯投影序列化逐字节一致（byte oracle：issueRequest 与
  // serializeChatRequestBody 同形）。
  ChatSession view;
  view.messages = controller.messages();
  for (int index = 0; index < view.messages.size(); ++index)
    if (view.messages.at(index).role == ChatRole::Assistant &&
        view.messages.at(index).content == QStringLiteral("pi 链终答。"))
      view.messages.resize(index); // 投影输入 = 发送时刻会话（不含本轮终答）
  const auto projection = paleo::chat::projectChatRequest(
    view, controller.systemPrompt(), controller.tools(), controller.config());
  QVERIFY(projection.diagnostics.sendable);
  QCOMPARE(QJsonDocument::fromJson(server.rawBody(0)).object(),
           QJsonDocument::fromJson(paleo::chat::serializeChatRequestBody(
             projection.messages, controller.tools(), controller.config())).object());
}

QTEST_GUILESS_MAIN(TestH1RequestAssembly)
#include "tst_h1_requestassembly.moc"
