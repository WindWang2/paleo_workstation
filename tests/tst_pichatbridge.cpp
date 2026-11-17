// 层：测试壳
#include "workflow/aichatcontroller.h"
#include "workflow/capabilityhost.h"
#include "catalog/datacatalog.h"
#include "services/paleotaskservice.h"
#include <QtTest>
#include <QHostAddress>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

namespace {
QByteArray reply(bool tools, int request = 0) {
  QJsonObject message{{"role", "assistant"}, {"content", tools ? "" : "host model completed"}};
  if (tools) {
    QJsonArray calls;
    for (int i = 0; i < 2; ++i) calls.append(QJsonObject{{"id", QString("provider-%1-call-%2").arg(request).arg(i)}, {"type", "function"},
      {"function", QJsonObject{{"name", "paleo.query_project"}, {"arguments", "{\"topic\":\"wells\"}"}}}});
    message.insert("tool_calls", calls);
  }
  return QJsonDocument(QJsonObject{{"choices", QJsonArray{QJsonObject{{"message", message}, {"finish_reason", tools ? "tool_calls" : "stop"}}}}}).toJson(QJsonDocument::Compact);
}
class Model : public QObject {
public:
  QTcpServer server; int requests = 0, fragments = 1; bool firstTools = true, toolsEveryTurn = false, sse = false;
  QString fragment = "host model completed";
  QVector<QByteArray> bodies;
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
        const bool calls = firstTools && (toolsEveryTurn ? index % 2 == 0 : index == 0);
        const auto body = reply(calls, requests); QByteArray payload = body, type = "application/json";
        if (sse) {
          const auto message = QJsonDocument::fromJson(body).object().value("choices").toArray().first().toObject().value("message").toObject();
          QJsonObject delta;
          if (calls) { auto list = message.value("tool_calls").toArray(); for (int i = 0; i < list.size(); ++i) { auto call = list[i].toObject(); call["index"] = i; list[i] = call; } delta["tool_calls"] = list; }
          else delta["content"] = fragment;
          payload.clear();
          for (int i = 0; i < (calls ? 1 : fragments); ++i)
            payload += "data: " + QJsonDocument(QJsonObject{{"choices", QJsonArray{QJsonObject{{"delta", delta}}}}}).toJson(QJsonDocument::Compact) + "\n\n";
          payload += "data: [DONE]\n\n";
          type = "text/event-stream";
        }
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nContent-Length: " + QByteArray::number(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload);
        socket->disconnectFromHost();
      });
    });
  }
  bool open() { return server.listen(QHostAddress::LocalHost, 0); }
  LlmConfig config() const { return LlmConfig::fromParts(QUrl(QString("http://127.0.0.1:%1/v1").arg(server.serverPort())), "controlled-fixture", "cpp-fixture-only-key", sse); }
};
struct Host {
  QTemporaryDir project; DataCatalog catalog; PaleoTaskService tasks;
  paleo::capability::CapabilityHost capability{paleo::capability::NativeAdapterFactory{}, nullptr};
  ~Host() { tasks.shutdown(10000, true); }
  bool bind(AiChatController &controller) {
    if (!catalog.open(project.path())) return false; tasks.setMaxWorkerThreads(3);
    capability.setTaskService(&tasks); capability.setCallerScope(paleo::host::Caller::Agent, "agent:pi-test");
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
}
class TestPiChatBridge : public QObject {
  Q_OBJECT
  QStringList m_sessions;
private slots:
  void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
  void cleanupTestCase() { for (const auto &id : m_sessions) { QString error; ChatSessionStore::remove(id, &error); } }
  void nativeHttpStillUsesTheExistingModelAndRealHostQuery() {
    Host host; Model model; QVERIFY(model.open()); AiChatController controller; QVERIFY(host.bind(controller));
    m_sessions.append(controller.session().id); controller.setConfig(model.config()); QSignalSpy results(&controller, &AiChatController::toolResultReady);
    controller.sendUserText("query the current project"); QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 10000);
    QCOMPARE(model.requests, 2); QCOMPARE(results.size(), 2); QVERIFY(results.first()[1].toBool());
    QCOMPARE(controller.messages().last().content, QString("host model completed")); QCOMPARE(host.catalog.versions().size(), 0);
  }
  void nativeRetryUsesTheSameHostBindingsDespiteNewProviderIds() {
    Host host; Model model; model.toolsEveryTurn = true; QVERIFY(model.open()); AiChatController controller; QVERIFY(host.bind(controller));
    m_sessions.append(controller.session().id); controller.setConfig(model.config()); QSignalSpy errors(&controller, &AiChatController::errorOccurred);
    controller.sendUserText("query with a stable host-owned turn"); QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 10000);
    QCOMPARE(controller.session().hostToolBindings.size(), 2); const auto bindings = controller.session().hostToolBindings;
    controller.retryLast(); QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 10000); QVERIFY(errors.isEmpty());
    QCOMPARE(model.requests, 4); QCOMPARE(controller.session().hostToolBindings, bindings); QCOMPARE(host.catalog.versions().size(), 0);
  }
  void legacyChatFilesRemainReadableButUnboundWritesCannotBeRetried_data() {
    QTest::addColumn<QString>("tool"); QTest::addColumn<bool>("hasTurn"); QTest::addColumn<bool>("allowed");
    QTest::newRow("old-write-no-turn") << QString("paleo.python_compute") << false << false;
    QTest::newRow("old-write-no-binding") << QString("paleo.python_compute") << true << false;
    QTest::newRow("unknown-tool") << QString("paleo.unknown_write") << false << false;
    QTest::newRow("old-read-only") << QString("paleo.query_project") << false << true;
    QTest::newRow("original-operation-read") << QString("paleo.operation_get") << false << true;
    QTest::newRow("legacy-resume-needs-original-facts") << QString("paleo.operation_resume") << false << false;
    QTest::newRow("old-text-only") << QString() << false << true;
  }
  void legacyChatFilesRemainReadableButUnboundWritesCannotBeRetried() {
    QFETCH(QString, tool); QFETCH(bool, hasTurn); QFETCH(bool, allowed);
    ChatSession legacy; legacy.id = ChatSessionStore::newSessionId(); m_sessions.append(legacy.id);
    ChatMessage user; user.role = ChatRole::User; user.content = "legacy user intent"; if (hasTurn) user.hostTurnId = "old-host-turn";
    ChatMessage assistant; assistant.role = ChatRole::Assistant; assistant.content = "legacy known answer";
    if (!tool.isEmpty()) assistant.toolCalls = {{"legacy-provider-id", tool, "{\"client_request_id\":\"previous-explicit-id\"}"}};
    legacy.messages = {user, assistant};
    if (!tool.isEmpty()) { ChatMessage result; result.role = ChatRole::Tool; result.toolCallId = "legacy-provider-id";
      result.content = "{\"ok\":true,\"operationId\":\"original-operation\"}"; legacy.messages.append(result); }
    QString error; QVERIFY2(ChatSessionStore::save(legacy, &error), qPrintable(error)); ChatSession read;
    QVERIFY2(ChatSessionStore::load(legacy.id, &read, &error), qPrintable(error)); QCOMPARE(read.messages, legacy.messages);
    QVERIFY(read.hostToolBindings.isEmpty()); Host host; Model model; model.firstTools = !tool.isEmpty(); QVERIFY(model.open());
    AiChatController controller; m_sessions.append(controller.session().id); QVERIFY(host.bind(controller));
    QVERIFY(controller.loadSession(legacy.id, &error)); controller.setConfig(model.config());
    QSignalSpy errors(&controller, &AiChatController::errorOccurred), dispatched(&controller, &AiChatController::toolExecutionStarted);
    controller.retryLast(); QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 10000);
    if (allowed) { QVERIFY(errors.isEmpty()); QCOMPARE(model.requests, tool.isEmpty() ? 1 : 2); }
    else { QCOMPARE(model.requests, 0); QCOMPARE(dispatched.size(), 0); QVERIFY(!errors.isEmpty());
      QCOMPARE(errors.first().first().toString(), QString("chat_legacy_retry_requires_operation_recovery")); QCOMPARE(controller.messages(), legacy.messages); }
    QCOMPARE(host.catalog.versions().size(), 0);
  }
  void selectedMissingPiDoesNotFallBackToNativeOrSendProviderKeys() {
    Host host; Model model; QVERIFY(model.open()); AiChatController controller; QVERIFY(host.bind(controller));
    m_sessions.append(controller.session().id); controller.setConfig(model.config()); controller.setPiRuntime({true, {}, {}});
    QSignalSpy errors(&controller, &AiChatController::errorOccurred); controller.sendUserText("requires selected optional engine");
    QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 10000); QVERIFY(!errors.isEmpty()); QCOMPARE(model.requests, 0);
    QCOMPARE(host.catalog.versions().size(), 0);
  }
  void hostSessionSaveFailurePrecedesAnyOptionalProcessOrToolDispatch_data() {
    QTest::addColumn<bool>("pi"); QTest::newRow("pi-before-node") << true; QTest::newRow("native-before-tools") << false;
  }
  void hostSessionSaveFailurePrecedesAnyOptionalProcessOrToolDispatch() {
    QFETCH(bool, pi);
    Host host; Model model; QVERIFY(model.open()); AiChatController controller; QVERIFY(host.bind(controller));
    m_sessions.append(controller.session().id); controller.setConfig(model.config()); controller.setPiRuntime({pi, {}, {}});
    const auto path = ChatSessionStore::pathFor(controller.session().id); QVERIFY(QDir().mkpath(path));
    const auto cleanup = qScopeGuard([&] { QDir().rmdir(path); });
    QSignalSpy errors(&controller, &AiChatController::errorOccurred), calls(&controller, &AiChatController::toolExecutionStarted);
    controller.sendUserText("a save error cannot dispatch side effects"); QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 10000);
    QVERIFY(!errors.isEmpty()); QVERIFY(errors.first().first().toString().contains(pi ? "pi_session_save_failed" : "pi_invocation_save_failed"));
    QCOMPARE(model.requests, pi ? 0 : 1); QCOMPARE(calls.size(), 0); QCOMPARE(host.catalog.versions().size(), 0);
  }
  void destroyedTaskServiceCannotLeaveAQueuedConversationPermanentlyBusy() {
    auto tasks = std::make_unique<PaleoTaskService>(); tasks->setMaxWorkerThreads(2); auto running = std::make_shared<std::atomic_int>(0);
    for (int i = 0; i < 2; ++i) tasks->start("occupy existing worker", [running](PaleoTask *task) {
      ++*running; while (!task->cancelRequested()) QThread::msleep(1); return QString();
    });
    QTRY_COMPARE_WITH_TIMEOUT(running->load(), 2, 10000); PiChatBridge bridge;
    QSignalSpy finished(&bridge, &PiChatBridge::finished), failed(&bridge, &PiChatBridge::failed);
    QCOMPARE(bridge.start({true, {}, {}}, tasks.get(), "host-session", "host-turn", {}, [] { return true; }), QString());
    QVERIFY(bridge.busy()); tasks.reset(); QTRY_VERIFY_WITH_TIMEOUT(!bridge.busy(), 10000);
    QCOMPARE(finished.size(), 1); QVERIFY(!failed.isEmpty()); QCOMPARE(failed.first().first().toString(), QString("pi_task_unavailable"));
  }
  void cancelPairingPrecedesSessionSwitchOrOwnerDeletion_data() {
    QTest::addColumn<bool>("deleteOwner"); QTest::newRow("switch-session") << false; QTest::newRow("delete-owner") << true;
  }
  void cancelPairingPrecedesSessionSwitchOrOwnerDeletion() {
    QFETCH(bool, deleteOwner); Host host; Model model; QVERIFY(model.open()); auto *controller = new AiChatController;
    QPointer<AiChatController> owner(controller); const auto cleanup = qScopeGuard([&] { delete owner.data(); });
    QVERIFY(host.bind(*controller)); const auto original = controller->session().id; m_sessions.append(original); controller->setConfig(model.config());
    bool changed = false;
    connect(controller, &AiChatController::toolExecutionStarted, this, [owner](const auto &) { if (owner) owner->cancel(); });
    connect(controller, &AiChatController::messageAppended, this, [&](const ChatMessage &message) {
      if (message.role != ChatRole::Tool || changed) return; changed = true;
      if (deleteOwner) delete owner.data(); else { owner->newSession(); m_sessions.append(owner->session().id); }
    });
    controller->sendUserText("cancel the two pending real query calls"); QTRY_VERIFY_WITH_TIMEOUT(changed, 10000);
    ChatSession stored; QString error; QVERIFY2(ChatSessionStore::load(original, &stored, &error), qPrintable(error));
    int calls = 0, replies = 0; for (const auto &message : stored.messages) { calls += message.toolCalls.size(); replies += message.role == ChatRole::Tool; }
    QCOMPARE(calls, 2); QCOMPARE(replies, calls); QCOMPARE(model.requests, 1); QCOMPARE(host.catalog.versions().size(), 0);
    if (!deleteOwner) { QVERIFY(owner); QVERIFY(owner->messages().isEmpty()); }
  }
  void reentrantSendAndCancelBeforeRoundDoNotStartAnOldModelRequest() {
    Host host; Model model; model.firstTools = false; QVERIFY(model.open()); AiChatController controller; QVERIFY(host.bind(controller));
    m_sessions.append(controller.session().id); controller.setConfig(model.config()); bool once = false;
    connect(&controller, &AiChatController::messageAppended, this, [&](const ChatMessage &message) {
      if (message.role == ChatRole::User && !once) { once = true; controller.sendUserText("newer owner turn"); }
    });
    controller.sendUserText("old owner turn"); QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 10000); QCOMPARE(model.requests, 1);
    bool cancelOnce = false; connect(&controller, &AiChatController::streamingChanged, this, [&](bool streaming) {
      if (streaming && !cancelOnce) { cancelOnce = true; controller.cancel(); }
    });
    controller.sendUserText("cancel before model admission"); QVERIFY(!controller.streaming()); QTest::qWait(30); QCOMPARE(model.requests, 1);
  }
  void completedToolSubscriberCanDeleteOwnerAndPersistAllPairings() {
    Host host; Model model; QVERIFY(model.open()); auto *controller = new AiChatController;
    QPointer<AiChatController> owner(controller); const auto cleanup = qScopeGuard([&] { delete owner.data(); });
    QVERIFY(host.bind(*controller)); const auto original = controller->session().id; m_sessions.append(original); controller->setConfig(model.config());
    connect(controller, &AiChatController::messageAppended, this, [owner](const ChatMessage &message) {
      if (message.role == ChatRole::Tool) delete owner.data();
    });
    controller->sendUserText("delete after actual host query completion"); QTRY_VERIFY_WITH_TIMEOUT(!owner, 10000);
    ChatSession stored; QString error; QVERIFY2(ChatSessionStore::load(original, &stored, &error), qPrintable(error));
    int calls = 0, replies = 0; for (const auto &message : stored.messages) { calls += message.toolCalls.size(); replies += message.role == ChatRole::Tool; }
    QCOMPARE(calls, 2); QCOMPARE(replies, 2); QCOMPARE(model.requests, 1); QCOMPARE(host.catalog.versions().size(), 0);
  }
  void queuedLateHostResultCannotWriteIntoTheNewSession() {
    Host host; Model model; QVERIFY(model.open()); AiChatController controller; QVERIFY(host.bind(controller));
    const auto original = controller.session().id; m_sessions.append(original); controller.setConfig(model.config());
    bool scheduled = false, switched = false; QSignalSpy results(&controller, &AiChatController::toolResultReady);
    connect(&controller, &AiChatController::toolExecutionStarted, this, [&](const auto &) {
      if (scheduled) return; scheduled = true;
      QTimer::singleShot(0, &controller, [&] { controller.cancel(); controller.newSession(); m_sessions.append(controller.session().id); switched = true; });
    });
    controller.sendUserText("cancel before the queued real query report arrives"); QTRY_VERIFY_WITH_TIMEOUT(switched, 10000);
    QTest::qWait(30); QVERIFY(controller.messages().isEmpty()); QCOMPARE(results.size(), 0); QCOMPARE(model.requests, 1);
    ChatSession stored; QString error; QVERIFY2(ChatSessionStore::load(original, &stored, &error), qPrintable(error));
    int replies = 0; for (const auto &message : stored.messages) replies += message.role == ChatRole::Tool; QCOMPARE(replies, 2);
  }
  void sessionSaveObserverCannotLetAnOuterSwitchOverwriteANewerSession() {
    Host host; Model model; model.firstTools = false; QVERIFY(model.open()); AiChatController controller; QVERIFY(host.bind(controller));
    m_sessions.append(controller.session().id); controller.setConfig(model.config()); controller.sendUserText("saved original turn");
    QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 10000);
    bool switched = false; QString newer;
    connect(&controller, &AiChatController::sessionListChanged, this, [&] {
      if (switched) return; switched = true; controller.newSession(); newer = controller.session().id; m_sessions.append(newer);
    });
    controller.newSession(); QVERIFY(switched); QCOMPARE(controller.session().id, newer); QVERIFY(controller.messages().isEmpty());
  }
  void realPiSseRoundUsesHostQueriesAndPreservesHostOnlyBinding() {
    const auto root = qEnvironmentVariable("PALEO_TEST_PI_NATIVE_ROOT");
    if (root.isEmpty()) { QVERIFY2(qEnvironmentVariable("PALEO_REQUIRE_PI_EXECUTION") != "1", "Required actual Pi fixture missing"); return; }
    Host host; Model model; model.sse = true; QVERIFY(model.open()); AiChatController controller; QVERIFY(host.bind(controller));
    m_sessions.append(controller.session().id); controller.setConfig(model.config());
    controller.setPiRuntime({true, root, qEnvironmentVariable("PALEO_TEST_PI_NATIVE_SHA256")});
    QSignalSpy errors(&controller, &AiChatController::errorOccurred), results(&controller, &AiChatController::toolResultReady);
    controller.sendUserText("Pi asks C++ to read the real project"); QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 180000);
    QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.first().first().toString()));
    QCOMPARE(model.requests, 2); QCOMPARE(results.size(), 2); QVERIFY(results.first()[1].toBool());
    QCOMPARE(controller.session().hostToolBindings.size(), 2); QCOMPARE(host.catalog.versions().size(), 0);
    for (const auto &body : model.bodies) { QVERIFY(!body.contains("host_tool_bindings")); QVERIFY(!body.contains("host_turn_id")); }
  }
  void realPiHotSseCoalescesWhileTrueOverflowFailsWhole_data() {
    QTest::addColumn<bool>("overflow"); QTest::addColumn<int>("fragments"); QTest::addColumn<QString>("fragment");
    QTest::newRow("200-small-deltas") << false << 200 << QString("hot-delta");
    QTest::newRow("10000-single-character-deltas") << false << 10000 << QString("x");
    QTest::newRow("escaped-small-deltas") << false << 2000 << QString("\"\\\n");
    QTest::newRow("actual-pending-budget") << true << 1 << QString(70000, 'x');
    QTest::newRow("json-escaping-overflow") << true << 1 << QString(60000, '\\');
  }
  void realPiHotSseCoalescesWhileTrueOverflowFailsWhole() {
    QFETCH(bool, overflow); QFETCH(int, fragments); QFETCH(QString, fragment);
    const auto root = qEnvironmentVariable("PALEO_TEST_PI_NATIVE_ROOT");
    if (root.isEmpty()) { QVERIFY2(qEnvironmentVariable("PALEO_REQUIRE_PI_EXECUTION") != "1", "Required actual Pi fixture missing"); return; }
    Host host; Model model; model.firstTools = false; model.sse = true; model.fragments = fragments;
    model.fragment = fragment; QVERIFY(model.open());
    AiChatController controller; QVERIFY(host.bind(controller)); m_sessions.append(controller.session().id); controller.setConfig(model.config());
    controller.setPiRuntime({true, root, qEnvironmentVariable("PALEO_TEST_PI_NATIVE_SHA256")}); QSignalSpy errors(&controller, &AiChatController::errorOccurred);
    controller.sendUserText("exercise bounded IPC backpressure"); QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 180000);
    QCOMPARE(model.requests, 1);
    if (overflow) { QVERIFY(!errors.isEmpty()); QVERIFY(errors.first().first().toString().contains("pi_outgoing_budget")); }
    else { QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.first().first().toString())); QCOMPARE(controller.messages().last().content, model.fragment.repeated(fragments)); }
    QCOMPARE(host.catalog.versions().size(), 0);
  }
  void realPiSingleOwnerCallbackCoalescesEscapedText_data() {
    QTest::addColumn<QString>("fragment");
    QTest::newRow("10000-single-ascii-deltas") << QString("x");
    QTest::newRow("10000-escaped-deltas") << QString("\"");
  }
  void realPiSingleOwnerCallbackCoalescesEscapedText() {
    QFETCH(QString, fragment); const auto root = qEnvironmentVariable("PALEO_TEST_PI_NATIVE_ROOT");
    if (root.isEmpty()) { QVERIFY2(qEnvironmentVariable("PALEO_REQUIRE_PI_EXECUTION") != "1", "Required actual Pi fixture missing"); return; }
    PaleoTaskService tasks; tasks.setMaxWorkerThreads(3); PiChatBridge bridge;
    QSignalSpy finished(&bridge, &PiChatBridge::finished), failed(&bridge, &PiChatBridge::failed);
    int accepted = 0; bool modelCompleted = false;
    const auto cleanup = qScopeGuard([&] {
      disconnect(&bridge, nullptr, &bridge, nullptr); bridge.abort(false); tasks.shutdown(10000, true);
    });
    connect(&bridge, &PiChatBridge::modelRequested, &bridge, [&] {
      // No event-loop turn can flush the pending text inside this callback.
      // The same 10000 fragments must fit regardless of their delivery count.
      while (accepted < 10000 && bridge.modelDelta(fragment)) ++accepted;
      modelCompleted = bridge.modelFinished({}, "stop");
    });
    QCOMPARE(bridge.start({true, root, qEnvironmentVariable("PALEO_TEST_PI_NATIVE_SHA256")}, &tasks,
      "burst-session", "burst-turn", {}, [] { return true; }), QString());
    QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 180000);
    QCOMPARE(accepted, 10000); QVERIFY(modelCompleted);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    QCOMPARE(finished.first().first().toString(), QString("ok"));
  }
};
QTEST_GUILESS_MAIN(TestPiChatBridge)
#include "tst_pichatbridge.moc"
