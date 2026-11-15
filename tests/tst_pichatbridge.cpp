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

namespace {
QByteArray reply(bool tools) {
  QJsonObject message{{"role", "assistant"}, {"content", tools ? "" : "host model completed"}};
  if (tools) {
    QJsonArray calls;
    for (int i = 0; i < 2; ++i) calls.append(QJsonObject{{"id", QString("call-%1").arg(i)}, {"type", "function"},
      {"function", QJsonObject{{"name", "paleo.query_project"}, {"arguments", "{\"topic\":\"wells\"}"}}}});
    message.insert("tool_calls", calls);
  }
  return QJsonDocument(QJsonObject{{"choices", QJsonArray{QJsonObject{{"message", message}, {"finish_reason", tools ? "tool_calls" : "stop"}}}}}).toJson(QJsonDocument::Compact);
}
class Model : public QObject {
public:
  QTcpServer server; int requests = 0, fragments = 1; bool firstTools = true, sse = false;
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
        const bool calls = firstTools && index == 0;
        const auto body = reply(calls); QByteArray payload = body, type = "application/json";
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
  void selectedMissingPiDoesNotFallBackToNativeOrSendProviderKeys() {
    Host host; Model model; QVERIFY(model.open()); AiChatController controller; QVERIFY(host.bind(controller));
    m_sessions.append(controller.session().id); controller.setConfig(model.config()); controller.setPiRuntime({true, {}, {}});
    QSignalSpy errors(&controller, &AiChatController::errorOccurred); controller.sendUserText("requires selected optional engine");
    QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 10000); QVERIFY(!errors.isEmpty()); QCOMPARE(model.requests, 0);
    QCOMPARE(host.catalog.versions().size(), 0);
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
    QTest::addColumn<bool>("overflow"); QTest::newRow("200-small-deltas") << false; QTest::newRow("actual-pending-budget") << true;
  }
  void realPiHotSseCoalescesWhileTrueOverflowFailsWhole() {
    QFETCH(bool, overflow); const auto root = qEnvironmentVariable("PALEO_TEST_PI_NATIVE_ROOT");
    if (root.isEmpty()) { QVERIFY2(qEnvironmentVariable("PALEO_REQUIRE_PI_EXECUTION") != "1", "Required actual Pi fixture missing"); return; }
    Host host; Model model; model.firstTools = false; model.sse = true; model.fragments = overflow ? 1 : 200;
    model.fragment = overflow ? QString(70000, 'x') : QString("hot-delta"); QVERIFY(model.open());
    AiChatController controller; QVERIFY(host.bind(controller)); m_sessions.append(controller.session().id); controller.setConfig(model.config());
    controller.setPiRuntime({true, root, qEnvironmentVariable("PALEO_TEST_PI_NATIVE_SHA256")}); QSignalSpy errors(&controller, &AiChatController::errorOccurred);
    controller.sendUserText("exercise bounded IPC backpressure"); QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 180000);
    QCOMPARE(model.requests, 1);
    if (overflow) { QVERIFY(!errors.isEmpty()); QVERIFY(errors.first().first().toString().contains("pi_outgoing_budget")); }
    else { QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.first().first().toString())); QCOMPARE(controller.messages().last().content, model.fragment.repeated(200)); }
    QCOMPARE(host.catalog.versions().size(), 0);
  }
};
QTEST_GUILESS_MAIN(TestPiChatBridge)
#include "tst_pichatbridge.moc"
