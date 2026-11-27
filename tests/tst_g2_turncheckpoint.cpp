// 层：测试壳
// G2 回合执行闭环（controller 侧矩阵）：工具结果先落盘再续轮（W2）、保存
// 欠账薄门与切换/删除保护（W3）、取消单次结算（W4）、本轮冻结（W1）、受控
// Pi 工具调用与会话保存（W8）。真实可发布工具的恢复重试链在
// tst_pigeneratedoperations（受控 Python 环境存在时执行）。
#include "fixtures/chatsessionstoragebreaker.h"
#include "workflow/aichatcontroller.h"
#include "workflow/capabilityhost.h"
#include "catalog/datacatalog.h"
#include "services/paleotaskservice.h"
#include "services/piprotocol.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>
#include <algorithm>
#include <atomic>
#include <functional>

namespace {
QByteArray reply(bool tools, int request, const QString &reason, const QString &toolName) {
  QJsonObject message{{"role", "assistant"}, {"content", tools ? "" : "host model completed"}};
  if (tools) {
    QJsonArray calls;
    for (int i = 0; i < 2; ++i) calls.append(QJsonObject{{"id", QString("provider-%1-call-%2").arg(request).arg(i)}, {"type", "function"},
      {"function", QJsonObject{{"name", toolName}, {"arguments", "{\"topic\":\"wells\"}"}}}});
    message.insert("tool_calls", calls);
  }
  return QJsonDocument(QJsonObject{{"choices", QJsonArray{QJsonObject{{"message", message}, {"finish_reason", reason.isEmpty() ? (tools ? QStringLiteral("tool_calls") : QStringLiteral("stop")) : reason}}}}}).toJson(QJsonDocument::Compact);
}
class Model : public QObject {
public:
  QTcpServer server; int requests = 0; bool firstTools = true;
  QVector<QByteArray> bodies;
  QString firstReason;
  QString firstToolName = QStringLiteral("paleo.query_project");
  Model() {
    connect(&server, &QTcpServer::newConnection, this, [this] {
      auto *socket = server.nextPendingConnection(); auto bytes = std::make_shared<QByteArray>();
      auto served = std::make_shared<bool>(false);
      connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      connect(socket, &QTcpSocket::readyRead, socket, [this, socket, bytes, served] {
        if (*served) return;
        bytes->append(socket->read(128 * 1024)); const auto header = bytes->indexOf("\r\n\r\n"); if (header < 0) return;
        const auto length = QString::fromUtf8(bytes->left(header)).section("Content-Length:", 1, 1, QString::SectionCaseInsensitiveSeps).section('\r', 0, 0).trimmed().toInt();
        if (length < 0 || length > 128 * 1024 || bytes->size() < header + 4 + length) return;
        *served = true; bodies.append(bytes->mid(header + 4, length)); const auto index = requests++;
        const bool calls = firstTools && index == 0;
        const auto body = reply(calls, requests, index == 0 ? firstReason : QString(), firstToolName);
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
      });
    });
  }
  bool open() { return server.listen(QHostAddress::LocalHost, 0); }
  LlmConfig config() const { return LlmConfig::fromParts(QUrl(QString("http://127.0.0.1:%1/v1").arg(server.serverPort())), "controlled-fixture", "cpp-fixture-only-key", false); }
  LlmConfig switchedConfig() const { auto config = this->config(); config.model = QStringLiteral("switched-mid-turn"); return config; }
};
struct Host {
  QTemporaryDir project; DataCatalog catalog; PaleoTaskService tasks;
  paleo::capability::CapabilityHost capability{paleo::capability::NativeAdapterFactory{}, nullptr};
  ~Host() { tasks.shutdown(10000, true); }
  bool bind(AiChatController &controller) {
    if (!catalog.open(project.path())) return false; tasks.setMaxWorkerThreads(3);
    capability.setTaskService(&tasks); capability.setCallerScope(paleo::host::Caller::Agent, "agent:g2-test");
    capability.setBindingProvider([this] {
      paleo::capability::HostBinding binding; binding.catalog = &catalog; binding.projectDir = project.path();
      binding.context.documentId = "host-document"; binding.context.epoch = 1; binding.context.open = true;
      binding.context.persistentIdentity = true; binding.context.readOnly = true; binding.context.allowedEffects = paleo::host::Read; return binding;
    });
    controller.toolRunner()->setTaskService(&tasks); AiChatToolContext context;
    context.catalog = &catalog; context.projectDir = project.path(); context.documentId = "host-document"; context.documentEpoch = 1;
    context.capabilityHost = &capability; context.requireCapabilityHost = true; controller.toolRunner()->setContext(context); return true;
  }
};
int countRole(const QVector<ChatMessage> &messages, ChatRole role) {
  int total = 0;
  for (const auto &message : messages) total += message.role == role;
  return total;
}
int diskToolReplies(const QString &session) {
  ChatSession stored; QString error;
  if (!ChatSessionStore::load(session, &stored, &error)) return -1;
  return countRole(stored.messages, ChatRole::Tool);
}
// 受控 Node peer（与 tst_g2_piterminal 的 FakeNode 同语义，精简为两调用轮）：
// 在 worker 线程经线程安全 channel 说话，宿主工具与模型 HTTP 全真。
class ControlledPeer {
public:
  // gate：第一项 tool_result 之后、第二项 tool_request 之前调用（worker 线程，
  // 仅线程安全操作；返回 false 视为驱动超时）。
  std::function<bool()> gate;
  QString run(PaleoTask *task, const std::shared_ptr<paleo::pi::ConversationChannel> &channel,
              const std::function<void()> &notify) {
    QByteArray buffer; QElapsedTimer clock; clock.start();
    bool stopping = false, idleSent = false, closed = false;
    int round = 0, callsDone = 0; QStringList pendingCalls;
    quint64 peerSequence = 0;
    const auto peer = [&](const QString &type, const QJsonObject &payload) {
      bool wake = false;
      const auto &identity = channel->identity();
      const QJsonObject envelope{{"protocol", QLatin1String(paleo::pi::ConversationProtocol)}, {"type", type},
        {"sessionId", identity.sessionId}, {"generation", identity.generation}, {"turnId", identity.turnId},
        {"nonce", identity.nonce}, {"sequence", QString::number(++peerSequence)}, {"payload", payload}};
      if (!channel->acceptPeer(envelope, wake)) return false;
      if (wake && notify) notify();
      return true;
    };
    while (!closed) {
      if (clock.elapsed() > 60000) return QStringLiteral("test_driver_timeout");
      const auto channelError = channel->error();
      if (!channelError.isEmpty() && channelError != QLatin1String("pi_conversation_busy")) return channelError;
      if (!stopping && (task->cancelRequested() || channel->stopReason())) stopping = true;
      if (stopping && !idleSent) {
        idleSent = true;
        if (!peer("idle", {{"code", channel->stopReason() == 1 ? QStringLiteral("cancelled") : QStringLiteral("interrupted")}}))
          return channel->error();
      }
      const auto outgoing = channel->takeOutgoing();
      if (!outgoing.isEmpty()) buffer += outgoing;
      QJsonObject frame; bool complete = false; QString error;
      while (paleo::pi::takeFrame(buffer, frame, complete, error)) {
        if (!complete) break;
        const auto type = frame.value("type").toString();
        const auto payload = frame.value("payload").toObject();
        if (type == QLatin1String("open")) {
          if (!peer("opened", {{"upstreamCommit", "u"}, {"coreVersion", "c"}, {"nodeVersion", "n"},
                               {"toolExecution", "sequential"}, {"modelTransport", "host-only"}, {"sandboxed", false}}))
            return channel->error();
        } else if (type == QLatin1String("tool_definition")) {
          if (!peer("configured", {{"name", payload.value("name").toString()}})) return channel->error();
        } else if (type == QLatin1String("start")) {
          round = 1;
          if (!peer("model_request", {{"round", QStringLiteral("1")}})) return channel->error();
        } else if (type == QLatin1String("model_call")) {
          pendingCalls.append(payload.value("callId").toString());
        } else if (type == QLatin1String("model_end")) {
          callsDone = 0;
          if (!pendingCalls.isEmpty() &&
              !peer("tool_request", {{"round", QString::number(round)}, {"callId", pendingCalls.first()}}))
            return channel->error();
          if (pendingCalls.isEmpty() && !peer("idle", {{"code", QStringLiteral("ok")}})) return channel->error();
        } else if (type == QLatin1String("tool_result")) {
          ++callsDone;
          if (!stopping && callsDone < pendingCalls.size()) {
            if (gate && !gate()) return QStringLiteral("test_gate_timeout");
            if (!peer("tool_request", {{"round", QString::number(round)}, {"callId", pendingCalls.at(callsDone)}}))
              return channel->error();
          } else if (!stopping) {
            pendingCalls.clear(); ++round;
            if (!peer("model_request", {{"round", QString::number(round)}})) return channel->error();
          }
        } else if (type == QLatin1String("close")) {
          if (!peer("closed", {})) return channel->error();
          closed = true; break;
        }
      }
      QThread::msleep(2);
    }
    return {};
  }
};
} // namespace

class TestG2TurnCheckpoint : public QObject {
  Q_OBJECT
  QStringList m_sessions;
private slots:
  void initTestCase() {
    QStandardPaths::setTestModeEnabled(true);
    QDir(ChatSessionStore::directory()).removeRecursively();
  }
  void cleanupTestCase() { for (const auto &id : m_sessions) ChatSessionStore::remove(id, nullptr); }

  // G2-W2：第一项真实结果后保存失败——第二项工具与下一模型请求均未开始，
  // 已收到结果保留在内存且不被改判执行失败；磁盘停在最后可信 checkpoint；
  // 恢复存储后 retryLast 先清欠账再续轮（G2-W7）。
  void bundledPiFirstResultSaveFailureStopsQueueAndNextRequest() {
    ChatSessionStorageBreaker breaker; Host host; Model model; QVERIFY(model.open());
    AiChatController controller; QVERIFY(host.bind(controller));
    m_sessions.append(controller.session().id); controller.setConfig(model.config());
    QSignalSpy errors(&controller, &AiChatController::errorOccurred),
        started(&controller, &AiChatController::toolExecutionStarted),
        results(&controller, &AiChatController::toolResultReady);
    bool broken = false;
    connect(&controller, &AiChatController::toolExecutionStarted, this, [&](const auto &) {
      if (!broken) { broken = true; QVERIFY(breaker.engage(controller.session().id)); } // 首项执行中、结果未回：破坏落盘
    });
    const auto restore = qScopeGuard([&] { breaker.restore(); });
    Q_UNUSED(restore)
    controller.sendUserText("two admitted calls, storage dies after the first starts");
    QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 15000);
    QVERIFY(broken);
    QCOMPARE(model.requests, 1);                 // 没有第二次模型请求
    QCOMPARE(started.size(), 1);                 // 第二项从未启动
    QCOMPARE(results.size(), 0);                 // UI 结果卡不发布未结算结果
    QVERIFY(!errors.isEmpty());
    QVERIFY(errors.first().first().toString().contains(QStringLiteral("保存失败")));
    // 内存：第一项真实成功结果保留（不改判 tool_failed），第二项按 stale 配对。
    const auto memory = controller.messages();
    QCOMPARE(countRole(memory, ChatRole::Tool), 2);
    const auto firstTool = std::find_if(memory.cbegin(), memory.cend(), [](const ChatMessage &m) { return m.role == ChatRole::Tool; });
    QVERIFY(firstTool != memory.cend());
    QVERIFY(firstTool->content.contains(QStringLiteral("\"ok\"")));
    // 磁盘：最后可信 checkpoint = 已准入绑定，无任何结果。
    ChatSession stored; QString error;
    QVERIFY(breaker.loadCheckpoint(&stored));
    QCOMPARE(countRole(stored.messages, ChatRole::Tool), 0);
    QCOMPARE(stored.hostToolBindings.size(), 2);
    QVERIFY(breaker.restore());
    QTRY_VERIFY_WITH_TIMEOUT(controller.canRetry(), 15000);
    errors.clear();
    controller.retryLast();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 15000);
    QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.first().first().toString()));
    QCOMPARE(model.requests, 2);
    ChatSession recovered; QVERIFY2(ChatSessionStore::load(controller.session().id, &recovered, &error), qPrintable(error));
    QCOMPARE(countRole(recovered.messages, ChatRole::Tool), 0); // 重试轮按夹具回终答，历史完整落盘
    QCOMPARE(host.catalog.versions().size(), 0);
  }

  // G2-W3：最终回答写盘失败——回答保留内存、状态如实、切换/删除被拒、
  // 重试先清欠账；恢复后同一入口正常完成，不加新确认。
  void finalAnswerSaveFailureIsHonestAndBlocksSilentSwitch() {
    ChatSessionStorageBreaker breaker; Host host; Model model; model.firstTools = false; QVERIFY(model.open());
    AiChatController controller; QVERIFY(host.bind(controller));
    const auto session = controller.session().id; m_sessions.append(session);
    controller.setConfig(model.config());
    // 目标会话先在存储健康时落盘（切换目标必须真实存在）。
    ChatSession other; other.id = ChatSessionStore::newSessionId(); m_sessions.append(other.id);
    other.messages = {ChatMessage{}};
    QString otherError; QVERIFY2(ChatSessionStore::save(other, &otherError), qPrintable(otherError));
    connect(controller.piBridge(), &PiChatBridge::modelRequested, this, [&] {
      QVERIFY(breaker.engage(session)); // Fail the final save after the turn checkpoint has succeeded.
    }, Qt::SingleShotConnection);
    const auto restore = qScopeGuard([&] { breaker.restore(); });
    Q_UNUSED(restore)
    QSignalSpy errors(&controller, &AiChatController::errorOccurred),
        changed(&controller, &AiChatController::sessionChanged),
        listed(&controller, &AiChatController::sessionListChanged);
    controller.sendUserText("plain answer with broken storage");
    QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 15000);
    QCOMPARE(model.requests, 1);
    QVERIFY(!errors.isEmpty());
    QVERIFY(errors.first().first().toString().contains(QStringLiteral("保存失败")));
    QCOMPARE(controller.messages().last().content, QString("host model completed"));
    QVERIFY(controller.statusText().contains(QStringLiteral("保存失败")));
    const auto memoryBefore = controller.messages();
    const int changedBefore = changed.size();
    // 薄门：newSession 拒绝切换，内容与当前会话保持。
    controller.newSession();
    QCOMPARE(controller.session().id, session);
    QCOMPARE(controller.messages(), memoryBefore);
    // 薄门：loadSession 拒绝且不发成功 sessionChanged（草稿不被清）。
    QString error; QVERIFY(!controller.loadSession(other.id, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(controller.session().id, session);
    QCOMPARE(controller.messages(), memoryBefore);
    QCOMPARE(changed.size(), changedBefore);
    QVERIFY(listed.size() > 0); // Combo 恢复真实当前选择
    // 薄门：removeSession(currentId) 不删当前证据；其他会话不受影响。
    QVERIFY(!controller.removeSession(session, &error));
    QCOMPARE(controller.session().id, session);
    QVERIFY2(ChatSessionStore::load(other.id, &other, &otherError), qPrintable(otherError));
    controller.sendUserText(QStringLiteral("start another turn while storage is broken"));
    QCOMPARE(model.requests, 1);
    QCOMPARE(controller.messages(), memoryBefore);
    // retryLast：欠账未清 → 不截历史、不重发。
    controller.retryLast();
    QCOMPARE(model.requests, 1);
    QCOMPARE(controller.messages(), memoryBefore);
    QVERIFY(breaker.restore());
    // 恢复后：retryLast 先补保存再重发，一切照常。
    QTRY_VERIFY_WITH_TIMEOUT(controller.canRetry(), 15000);
    errors.clear();
    controller.retryLast();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 15000);
    QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.first().first().toString()));
    QCOMPARE(model.requests, 2);
    ChatSession recovered; QVERIFY2(ChatSessionStore::load(session, &recovered, &error), qPrintable(error));
    QVERIFY(!recovered.messages.isEmpty());
  }

  // G2-W1：回合内模型配置冻结——续轮请求沿用开场身份，中途 setConfig 不改
  // 写在途回合与记录的 model 字段；下一轮才生效。
  void midTurnConfigChangeDoesNotHijackTheRunningTurn() {
    Host host; Model model; QVERIFY(model.open());
    AiChatController controller; QVERIFY(host.bind(controller));
    m_sessions.append(controller.session().id); controller.setConfig(model.config());
    bool switched = false;
    controller.setProjectBrief(QStringLiteral("original-project-brief"));
    QStringList activeTurnIds;
    connect(&controller, &AiChatController::streamingChanged, this, [&](bool active) {
      if (!active) return;
      activeTurnIds.append(controller.hostTurnId());
      QCOMPARE(controller.turnContext(), controller.toolRunner()->hostContextIdentity());
    });
    connect(&controller, &AiChatController::toolExecutionStarted, this, [&](const auto &) {
      if (!switched) {
        switched = true;
        controller.setConfig(model.switchedConfig());
        controller.setProjectBrief(QStringLiteral("updated-project-brief"));
      }
    });
    controller.sendUserText("freeze me");
    QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 15000);
    QVERIFY(switched);
    QCOMPARE(model.requests, 2);
    QCOMPARE(model.bodies.size(), 2);
    QVERIFY(model.bodies.first().contains("\"model\":\"controlled-fixture\""));
    QVERIFY(model.bodies.at(1).contains("\"model\":\"controlled-fixture\"")); // 续轮 = 冻结身份
    QVERIFY(!model.bodies.at(1).contains("switched-mid-turn"));
    QVERIFY(model.bodies.first().contains("original-project-brief"));
    QVERIFY(model.bodies.at(1).contains("original-project-brief"));
    QVERIFY(!model.bodies.at(1).contains("updated-project-brief"));
    QCOMPARE(controller.config().model, QString("switched-mid-turn"));       // 配置本身已更新
    QCOMPARE(controller.session().model, QString("controlled-fixture"));     // 记录字段不悄悄改
    // 下一轮使用新配置。
    controller.sendUserText("new turn uses the new model");
    QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 15000);
    QCOMPARE(model.requests, 3);
    QVERIFY(model.bodies.at(2).contains("\"model\":\"switched-mid-turn\""));
    QCOMPARE(controller.session().model, QString("switched-mid-turn"));
    QVERIFY(model.bodies.at(2).contains("updated-project-brief"));
    QCOMPARE(activeTurnIds.size(), 2);
    QVERIFY(!activeTurnIds.first().isEmpty());
    QVERIFY(activeTurnIds.first() != activeTurnIds.last());
    controller.newSession();
    QVERIFY(controller.hostTurnId().isEmpty());
    QVERIFY(controller.turnContext().isEmpty());
  }

  void contextChangesCannotMoveLaterToolCallsToAnotherProject() {
    Host original, replacement;
    Model model;
    QVERIFY(model.open());
    AiChatController controller;
    QVERIFY(original.bind(controller));
    m_sessions.append(controller.session().id);
    controller.setConfig(model.config());
    QSignalSpy started(&controller, &AiChatController::toolExecutionStarted);
    QSignalSpy errors(&controller, &AiChatController::errorOccurred);
    connect(&controller, &AiChatController::toolExecutionStarted, this,
        [&](const ChatToolCall &) { if (started.size() == 1) QVERIFY(replacement.bind(controller)); });

    controller.sendUserText(QStringLiteral("query the original project twice"));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 15000);
    QCOMPARE(started.size(), 1);
    QCOMPARE(model.requests, 1);
    QCOMPARE(errors.size(), 1);
    QCOMPARE(errors.first().first().toString(), QStringLiteral("interrupted"));
    QCOMPARE(countRole(controller.messages(), ChatRole::Tool), 2);
    QCOMPARE(diskToolReplies(controller.session().id), 2);
  }

  void unsafeModelCompletionsCannotStartTools_data() {
    QTest::addColumn<QString>("finishReason");
    QTest::addColumn<QString>("toolName");
    QTest::addColumn<QString>("errorCode");
    QTest::newRow("truncated") << QStringLiteral("length") << QStringLiteral("paleo.query_project")
                               << QStringLiteral("model_output_truncated");
    QTest::newRow("filtered") << QStringLiteral("content_filter") << QStringLiteral("paleo.query_project")
                              << QStringLiteral("model_output_filtered");
    QTest::newRow("not-declared") << QStringLiteral("tool_calls") << QStringLiteral("paleo.unknown")
                                  << QStringLiteral("tool_not_declared");
  }

  void unsafeModelCompletionsCannotStartTools() {
    QFETCH(QString, finishReason);
    QFETCH(QString, toolName);
    QFETCH(QString, errorCode);
    Host host;
    Model model;
    model.firstReason = finishReason;
    model.firstToolName = toolName;
    QVERIFY(model.open());
    AiChatController controller;
    QVERIFY(host.bind(controller));
    m_sessions.append(controller.session().id);
    controller.setConfig(model.config());
    QSignalSpy started(&controller, &AiChatController::toolExecutionStarted);
    QSignalSpy errors(&controller, &AiChatController::errorOccurred);

    controller.sendUserText(QStringLiteral("attempt invalid tool completion"));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 15000);
    QCOMPARE(started.size(), 0);
    QCOMPARE(model.requests, 1);
    QCOMPARE(errors.size(), 1);
    QCOMPARE(errors.first().first().toString(), errorCode);
    QVERIFY(controller.session().hostToolBindings.isEmpty());
    QVERIFY(controller.messages().last().toolCalls.isEmpty());
  }

  // G2-W8 受控 Pi 链：真实 controller + 真实宿主查询 + 受控 peer。第一项结果
  // 先落盘（gate 在磁盘观察到后才放行第二项请求）→ 全链终态一致。
  void controlledPiChainPersistsEachResultBeforeContinuing() {
    Host host; Model model; QVERIFY(model.open());
    AiChatController controller; QVERIFY(host.bind(controller));
    m_sessions.append(controller.session().id); controller.setConfig(model.config());
    ControlledPeer peer;
    std::atomic<int> gateChecks{0};
    const auto sessionId = controller.session().id;
    peer.gate = [sessionId, &gateChecks] {
      // G2-W2 oracle：第二个 tool_request 派发前，第一项真实结果必须已在磁盘。
      for (int i = 0; i < 500; ++i) {
        ++gateChecks;
        if (diskToolReplies(sessionId) >= 1) return true;
        QThread::msleep(10);
      }
      return false;
    };
    controller.piBridge()->setWorkerDriver([&peer](PaleoTask *task,
        const std::shared_ptr<paleo::pi::ConversationChannel> &channel, const std::function<void()> &notify) {
      return peer.run(task, channel, notify);
    });
    QSignalSpy errors(&controller, &AiChatController::errorOccurred),
        results(&controller, &AiChatController::toolResultReady);
    controller.sendUserText("controlled pi chain with real host queries");
    QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 60000);
    QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.first().first().toString()));
    QCOMPARE(model.requests, 2);
    QCOMPARE(results.size(), 2);
    QVERIFY(results.first()[1].toBool());
    QVERIFY(gateChecks.load() >= 1);
    ChatSession stored; QString error;
    QVERIFY2(ChatSessionStore::load(sessionId, &stored, &error), qPrintable(error));
    QCOMPARE(stored.hostToolBindings.size(), 2);
    QCOMPARE(countRole(stored.messages, ChatRole::Tool), 2);
    QCOMPARE(stored.messages.last().content, QString("host model completed"));
    QCOMPARE(host.catalog.versions().size(), 0);
  }

  // G2-W2/W8：Pi 链第一项结果后保存失败：停止续轮与
  // Pi 请求、结果保留内存、磁盘停在准入 checkpoint；恢复后 retry 收口。
  void controlledPiChainStopsOnSaveFailure() {
    ChatSessionStorageBreaker breaker; Host host; Model model; QVERIFY(model.open());
    AiChatController controller; QVERIFY(host.bind(controller));
    m_sessions.append(controller.session().id); controller.setConfig(model.config());
    ControlledPeer peer;
    controller.piBridge()->setWorkerDriver([&peer](PaleoTask *task,
        const std::shared_ptr<paleo::pi::ConversationChannel> &channel, const std::function<void()> &notify) {
      return peer.run(task, channel, notify);
    });
    QSignalSpy errors(&controller, &AiChatController::errorOccurred),
        started(&controller, &AiChatController::toolExecutionStarted);
    bool broken = false;
    connect(&controller, &AiChatController::toolExecutionStarted, this, [&](const auto &) {
      if (!broken) { broken = true; QVERIFY(breaker.engage(controller.session().id)); }
    });
    const auto restore = qScopeGuard([&] { breaker.restore(); });
    Q_UNUSED(restore)
    controller.sendUserText("storage dies during the controlled pi tool round");
    QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 60000);
    QVERIFY(broken);
    QCOMPARE(model.requests, 1);
    QCOMPARE(started.size(), 1);
    QVERIFY(!errors.isEmpty());
    QVERIFY(errors.first().first().toString().contains(QStringLiteral("保存失败")));
    ChatSession stored; QString error;
    QVERIFY(breaker.loadCheckpoint(&stored));
    QCOMPARE(countRole(stored.messages, ChatRole::Tool), 0);
    QCOMPARE(stored.hostToolBindings.size(), 2); // 准入绑定仍是最可信 checkpoint
    QCOMPARE(countRole(controller.messages(), ChatRole::Tool), 2); // 内存配对完整（真实结果 + stale）
    QVERIFY(breaker.restore());
    QTRY_VERIFY_WITH_TIMEOUT(controller.canRetry(), 15000); // Pi worker 收尾完成后方可重试
    errors.clear();
    controller.retryLast();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 60000);
    QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.first().first().toString()));
    QCOMPARE(model.requests, 2);
    ChatSession recovered; QVERIFY2(ChatSessionStore::load(controller.session().id, &recovered, &error), qPrintable(error));
    QCOMPARE(countRole(recovered.messages, ChatRole::Tool), 0);
  }

  // G2-W4：保存失败窗口内用户取消——首个有效原因一次结算（取消），配对
  // 完整、无孤儿帧、无重复错误；欠账薄门仍然生效。
  void userCancelDuringBrokenStorageSettlesOnce() {
    ChatSessionStorageBreaker breaker; Host host; Model model; QVERIFY(model.open());
    AiChatController controller; QVERIFY(host.bind(controller));
    const auto session = controller.session().id; m_sessions.append(session);
    controller.setConfig(model.config());
    QSignalSpy errors(&controller, &AiChatController::errorOccurred);
    bool broken = false, cancelled = false;
    connect(&controller, &AiChatController::toolExecutionStarted, this, [&](const auto &) {
      if (!broken) { broken = true; QVERIFY(breaker.engage(controller.session().id)); }
    });
    const auto restore = qScopeGuard([&] { breaker.restore(); });
    Q_UNUSED(restore)
    connect(&controller, &AiChatController::messageAppended, this, [&](const ChatMessage &message) {
      if (cancelled || message.role != ChatRole::Tool) return; cancelled = true; controller.cancel();
    });
    controller.sendUserText("cancel lands inside the broken save window");
    QTRY_VERIFY_WITH_TIMEOUT(cancelled, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 15000);
    QCOMPARE(errors.size(), 1); // 取消是首个有效原因：仅一次结算，不叠保存失败弹窗
    QVERIFY(errors.first().first().toString().contains(QStringLiteral("取消")));
    const auto memory = controller.messages();
    QCOMPARE(countRole(memory, ChatRole::Tool), 2); // 两条调用都有应答，无孤儿
    QVERIFY(broken);
    // 欠账仍在（取消路径的保存也失败）→ 薄门拒绝无声切换。
    controller.newSession();
    QCOMPARE(controller.session().id, session);
    QCOMPARE(controller.messages(), memory);
    QVERIFY(breaker.restore());
    // 恢复存储后同一入口正常完成：切换前先落盘完整配对记录，内容不丢。
    controller.newSession();
    QVERIFY(controller.session().id != session);
    ChatSession saved; QString loadError;
    QVERIFY2(ChatSessionStore::load(session, &saved, &loadError), qPrintable(loadError));
    QCOMPARE(countRole(saved.messages, ChatRole::Tool), 2);
  }
};

QTEST_GUILESS_MAIN(TestG2TurnCheckpoint)
#include "tst_g2_turncheckpoint.moc"
