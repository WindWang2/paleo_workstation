// 层：测试壳
// H7-W8/W9：真实 controller / runner / loopback 模型端点（原生与受控 Pi）上的
// read_result 预算续读闭环。
//
// oracle：真正送出的 role=tool content 是 final envelope；每页 ≤ 本次调用有效
// 配额且 data/nextCursor 可同时抵达；逐页重组 == 原不可变值的可观测投影；
// 读页不重新执行原查询（catalog 读取计数不变），也不签发新引用。
#include "workflow/aichatcontroller.h"
#include "workflow/aichattoolrunner.h"
#include "workflow/chatresultstore.h"
#include "services/toolresultprotocol.h"
#include "services/paleotaskservice.h"
#include "services/piprotocol.h"
#include "catalog/datacatalog.h"
#include "ai/chat/chatsession.h"

#include <QtTest>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSet>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <functional>
#include <memory>

namespace {
QByteArray assistantJson(const QJsonObject &message, const char *finish) {
  return QJsonDocument(QJsonObject{{"choices", QJsonArray{QJsonObject{
      {"message", message}, {"finish_reason", finish}}}}})
      .toJson(QJsonDocument::Compact);
}
QJsonObject toolMessage(const QJsonArray &calls) {
  return {{"role", "assistant"}, {"content", ""}, {"tool_calls", calls}};
}
QJsonObject toolCall(const QString &id, const QString &name, const QJsonObject &args) {
  return {{"id", id}, {"type", "function"},
          {"function", QJsonObject{{"name", name},
                                   {"arguments", QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}}}};
}
QJsonObject textMessage(const QString &text) {
  return {{"role", "assistant"}, {"content", text}};
}
QJsonObject lastToolEnvelope(const QJsonObject &body) {
  const auto messages = body.value("messages").toArray();
  for (int i = messages.size() - 1; i >= 0; --i) {
    const auto message = messages.at(i).toObject();
    if (message.value("role").toString() != QLatin1String("tool")) continue;
    return QJsonDocument::fromJson(message.value("content").toString().toUtf8()).object();
  }
  return {};
}
int toolMessagesNamed(const QJsonObject &body, const QString &name) {
  int count = 0;
  const auto messages = body.value("messages").toArray();
  for (int i = 0; i < messages.size(); ++i) {
    const auto message = messages.at(i).toObject();
    if (message.value("role").toString() != QLatin1String("assistant")) continue;
    for (const auto &call : message.value("tool_calls").toArray())
      count += call.toObject().value("function").toObject().value("name").toString() == name;
  }
  return count;
}
QStringList sessionToolRefs(const QVector<ChatMessage> &messages) {
  QStringList refs;
  for (const auto &message : messages) {
    if (message.role != ChatRole::Tool) continue;
    const auto envelope = QJsonDocument::fromJson(message.content.toUtf8()).object();
    for (const auto &ref : envelope.value("resultRefs").toArray()) {
      const auto text = ref.toObject().value("ref").toString();
      if (!refs.contains(text)) refs << text;
    }
  }
  return refs;
}

// 逐请求应答的 loopback 端点：responder 拿到完整请求体，返回 assistant message。
class LlmServer : public QObject {
public:
  using Responder = std::function<QJsonObject(int, const QJsonObject &)>;
  bool start(Responder responder) {
    m_responder = std::move(responder);
    connect(&m_server, &QTcpServer::newConnection, this,
            [this] { serve(m_server.nextPendingConnection()); });
    return m_server.listen(QHostAddress::LocalHost, 0);
  }
  QUrl endpoint() const {
    return QUrl(QStringLiteral("http://127.0.0.1:%1/v1").arg(m_server.serverPort()));
  }
  int requests() const { return m_bodies.size(); }
  QJsonObject body(int index) const { return m_bodies.value(index); }

private:
  void serve(QTcpSocket *socket) {
    if (!socket) return;
    connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    auto buffer = std::make_shared<QByteArray>();
    connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
      buffer->append(socket->readAll());
      const int header = buffer->indexOf("\r\n\r\n");
      if (header < 0) return;
      const int length = QString::fromUtf8(buffer->left(header))
          .section("Content-Length:", 1, 1, QString::SectionCaseInsensitiveSeps)
          .section('\r', 0, 0).trimmed().toInt();
      if (length < 0 || buffer->size() < header + 4 + length) return;
      const auto body = QJsonDocument::fromJson(buffer->mid(header + 4, length)).object();
      const int index = m_bodies.size();
      m_bodies.append(body);
      const auto message = m_responder ? m_responder(index, body) : textMessage(QStringLiteral("no responder"));
      const bool tools = !message.value("tool_calls").toArray().isEmpty();
      const auto payload = assistantJson(message, tools ? "tool_calls" : "stop");
      socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                    + QByteArray::number(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload);
      socket->disconnectFromHost();
    });
  }
  QTcpServer m_server;
  Responder m_responder;
  QVector<QJsonObject> m_bodies;
};

struct Host {
  QTemporaryDir project;
  DataCatalog catalog;
  PaleoTaskService tasks;
  ~Host() { tasks.shutdown(10000, true); }
  bool bind(AiChatController &controller, int wells = 0, QStringList *names = nullptr,
            int wellNamePadding = 36) {
    if (!catalog.open(project.path())) return false;
    tasks.setMaxWorkerThreads(3);
    for (int i = 0; i < wells; ++i) {
      CatalogEntity entity;
      entity.id = QStringLiteral("w%1").arg(i, 3, 10, QLatin1Char('0'));
      entity.entityType = QStringLiteral("well");
      entity.name = QStringLiteral("W-%1-%2").arg(i, 3, 10, QLatin1Char('0'))
                        .arg(QString(wellNamePadding, QLatin1Char('x')));
      if (!catalog.addEntity(entity)) return false;
      if (names) names->append(entity.name);
    }
    controller.toolRunner()->setTaskService(&tasks);
    AiChatToolContext context;
    context.catalog = &catalog;
    context.projectDir = project.path();
    context.documentId = QStringLiteral("host-document");
    context.documentEpoch = 1;
    controller.toolRunner()->setContext(context);
    return true;
  }
};

// 通用页游标：从真实 envelope 里抽 ref/child selector，按实际 nextCursor 续读。
struct PageWalker {
  QString ref;
  QString selector;
  QString cursor;
  QStringList pendingSelectors;
  QVector<QString> names;
  int pages = 0;
  int reads = 0;
  QString error;

  QJsonObject readRequest() {
    ++reads;
    QJsonObject args{{"result_ref", ref}, {"page_size", 64}, {"max_bytes", 65536}};
    if (!selector.isEmpty()) args.insert("selector", selector);
    if (!cursor.isEmpty()) args.insert("cursor", cursor);
    return args;
  }
  QJsonObject start(const QJsonObject &queryEnvelope) {
    const auto refs = queryEnvelope.value("resultRefs").toArray();
    if (refs.isEmpty()) { error = QStringLiteral("query envelope has no resultRefs"); return {}; }
    ref = refs.first().toObject().value("ref").toString();
    return readRequest();
  }
  QJsonObject nextSelector() {
    if (pendingSelectors.isEmpty()) return {};
    selector = pendingSelectors.takeFirst();
    cursor.clear();
    return readRequest();
  }
  QJsonObject advance(const QJsonObject &envelope) {
    if (!paleo::chat::isResultEnvelope(envelope)) {
      error = QStringLiteral("tool content is not a paleo.tool-result.v1 envelope: ")
              + QString::fromUtf8(paleo::chat::compactJson(envelope));
      return {};
    }
    const auto data = envelope.value("data").toObject();
    if (data.value("code").toString() != QLatin1String("ok")) {
      error = QStringLiteral("read page failed: ") + data.value("code").toString();
      return {};
    }
    const auto entries = data.value("entries").toArray();
    ++pages;
    for (const auto &value : entries) {
      const auto entry = value.toObject();
      if (entry.contains("child")) {
        pendingSelectors << entry.value("child").toObject().value("selector").toString();
        continue;
      }
      const auto item = entry.value("value").toObject();
      if (item.contains("name")) names << item.value("name").toString();
    }
    if (data.value("hasMore").toBool()) {
      cursor = data.value("nextCursor").toString();
      if (cursor.isEmpty()) error = QStringLiteral("hasMore page without nextCursor");
      return readRequest();
    }
    return nextSelector();
  }
};

// 受控 Pi peer（同 tst_g2_turncheckpoint 语义）：按模型给出的调用顺序请求；
// duplicateFirst 在首个 tool_result 之后重发同一已完成请求，验证桥内复用。
class PagePeer {
public:
  bool duplicateFirst = false;
  QString duplicatedSourceContent;
  QString duplicateContent;
  int toolResults = 0;
  QString run(PaleoTask *task, const std::shared_ptr<paleo::pi::ConversationChannel> &channel,
              const std::function<void()> &notify) {
    QByteArray buffer;
    QElapsedTimer clock;
    clock.start();
    bool stopping = false, idleSent = false, closed = false;
    bool duplicateRequested = false, duplicated = false;
    int round = 0;
    QStringList pendingCalls;
    QSet<QString> answered;
    quint64 sequence = 0;
    const auto peer = [&](const QString &type, const QJsonObject &payload) {
      bool wake = false;
      const auto &identity = channel->identity();
      const QJsonObject envelope{{"protocol", QLatin1String(paleo::pi::ConversationProtocol)},
          {"type", type}, {"sessionId", identity.sessionId}, {"generation", identity.generation},
          {"turnId", identity.turnId}, {"nonce", identity.nonce},
          {"sequence", QString::number(++sequence)}, {"payload", payload}};
      if (!channel->acceptPeer(envelope, wake)) return false;
      if (wake && notify) notify();
      return true;
    };
    while (!closed) {
      if (clock.elapsed() > 60000) return QStringLiteral("test_driver_timeout");
      const auto channelError = channel->error();
      if (!channelError.isEmpty() && channelError != QLatin1String("pi_conversation_busy"))
        return channelError;
      if (!stopping && (task->cancelRequested() || channel->stopReason())) stopping = true;
      if (stopping && !idleSent) {
        idleSent = true;
        if (!peer("idle", {{"code", channel->stopReason() == 1 ? QStringLiteral("cancelled")
                                                               : QStringLiteral("interrupted")}}))
          return channel->error();
      }
      const auto outgoing = channel->takeOutgoing();
      if (!outgoing.isEmpty()) buffer += outgoing;
      QJsonObject frame;
      bool complete = false;
      QString error;
      while (paleo::pi::takeFrame(buffer, frame, complete, error)) {
        if (!complete) break;
        const auto type = frame.value("type").toString();
        const auto payload = frame.value("payload").toObject();
        if (type == QLatin1String("open")) {
          if (!peer("opened", {{"upstreamCommit", "u"}, {"coreVersion", "c"}, {"nodeVersion", "n"},
                               {"toolExecution", "sequential"}, {"modelTransport", "host-only"},
                               {"sandboxed", false}}))
            return channel->error();
        } else if (type == QLatin1String("tool_definition")) {
          if (!peer("configured", {{"name", payload.value("name").toString()}})) return channel->error();
        } else if (type == QLatin1String("start")) {
          round = 1;
          if (!peer("model_request", {{"round", QStringLiteral("1")}})) return channel->error();
        } else if (type == QLatin1String("model_call")) {
          pendingCalls.append(payload.value("callId").toString());
        } else if (type == QLatin1String("model_end")) {
          answered.clear();
          if (!pendingCalls.isEmpty() &&
              !peer("tool_request", {{"round", QString::number(round)}, {"callId", pendingCalls.first()}}))
            return channel->error();
          if (pendingCalls.isEmpty() && !peer("idle", {{"code", QStringLiteral("ok")}}))
            return channel->error();
        } else if (type == QLatin1String("tool_result")) {
          const auto id = payload.value("callId").toString();
          if (!answered.contains(id)) {
            answered.insert(id);
            ++toolResults;
            if (duplicateFirst && !duplicated) {
              duplicated = true;
              duplicateRequested = true;
              duplicatedSourceContent = payload.value("content").toString();
              // 同一已完成请求的重复帧：桥必须重发原内容，不再派发执行。
              if (!peer("tool_request", {{"round", QString::number(round)}, {"callId", id}}))
                return channel->error();
              continue;
            }
          } else if (duplicateRequested) {
            duplicateRequested = false;
            duplicateContent = payload.value("content").toString();
          }
          if (!stopping && answered.size() < pendingCalls.size()) {
            for (const auto &call : pendingCalls) {
              if (answered.contains(call)) continue;
              if (!peer("tool_request", {{"round", QString::number(round)}, {"callId", call}}))
                return channel->error();
              break;
            }
          } else if (!stopping) {
            pendingCalls.clear();
            ++round;
            if (!peer("model_request", {{"round", QString::number(round)}})) return channel->error();
          }
        } else if (type == QLatin1String("close")) {
          if (!peer("closed", {})) return channel->error();
          closed = true;
          break;
        }
      }
      QThread::msleep(2);
    }
    return {};
  }
};
} // namespace

class TestH7ResultWire : public QObject {
  Q_OBJECT
  QStringList m_sessions;
private slots:
  void initTestCase() {
    QStandardPaths::setTestModeEnabled(true);
    QDir(ChatSessionStore::directory()).removeRecursively();
  }
  void cleanupTestCase() {
    for (const auto &id : m_sessions) ChatSessionStore::remove(id, nullptr);
  }
  void nativeQueryResultIsRetrievedAcrossPages();
  void secondTurnContinuesTheSameTemporaryReference();
  void controlledPiPagesAndDuplicateFrames();
  void runnerFinalisationUsesEffectiveSubBudget();
  void multiCallFrameKeepsIndependentQuotas();
};

void TestH7ResultWire::nativeQueryResultIsRetrievedAcrossPages() {
  Host host;
  QStringList expected;
  AiChatController controller;
  // 22 条井记录的 /wells 数组本身就超过 8192 字节，首次查询必得临时引用。
  QVERIFY(host.bind(controller, 22, &expected, /*wellNamePadding=*/96));
  m_sessions.append(controller.session().id);

  std::atomic_int entityPages{0};
  host.catalog.setReadObserver([&](const char *what) {
    if (QString::fromLatin1(what) == QLatin1String("entityPage")) ++entityPages;
  });

  PageWalker walker;
  QString error;
  int readCalls = 0;
  LlmServer server;
  QVERIFY(server.start([&](int index, const QJsonObject &body) -> QJsonObject {
    if (index == 0)
      return toolMessage({toolCall(QStringLiteral("call-q"), QStringLiteral("paleo.query_project"),
          {{"topic", "wells"}, {"page_size", 256}, {"max_bytes", 65536}})});
    const auto previous = lastToolEnvelope(body);
    if (walker.ref.isEmpty()) {
      const auto args = walker.start(previous);
      if (!walker.error.isEmpty()) { error = walker.error; return textMessage(QStringLiteral("error")); }
      return toolMessage({toolCall(QStringLiteral("call-r%1").arg(++readCalls),
                                   QStringLiteral("paleo.read_result"), args)});
    }
    const auto args = walker.advance(previous);
    if (!walker.error.isEmpty()) { error = walker.error; return textMessage(QStringLiteral("error")); }
    if (args.isEmpty())
      return textMessage(QStringLiteral("retrieved %1 wells in %2 pages").arg(walker.names.size()).arg(walker.pages));
    return toolMessage({toolCall(QStringLiteral("call-r%1").arg(++readCalls),
                                 QStringLiteral("paleo.read_result"), args)});
  }));

  auto config = LlmConfig::fromParts(server.endpoint(), QStringLiteral("h7-fixture"),
                                     QByteArrayLiteral("sk-h7-fixture"), false);
  // 模拟端点每次只回一个工具调用或一句终答，按这个规模预留模型输出，
  // 让单轮预算覆盖本例要验证的完整分页往返。
  config.maxTokens = 256;
  controller.setConfig(config);
  QSignalSpy started(&controller, &AiChatController::toolExecutionStarted);
  QSignalSpy errors(&controller, &AiChatController::errorOccurred);
  controller.sendUserText(QStringLiteral("列出工程里的井"));
  QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 30000);
  QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.first().first().toString()));
  QVERIFY2(error.isEmpty(), qPrintable(error));
  QVERIFY(!walker.ref.isEmpty());
  QVERIFY(walker.pages >= 2); // 真实配额切页（8192 字节），不是一次读完
  QCOMPARE(walker.names.size(), expected.size());
  QCOMPARE(walker.names, expected);

  // 真正送出的 role=tool content：合法 envelope、≤ 单页上限、data/nextCursor 同达。
  const auto body = server.body(server.requests() - 1);
  const auto messages = body.value("messages").toArray();
  int readEnvelopes = 0;
  int childEntries = 0;
  int cursorPages = 0;
  for (const auto &value : messages) {
    const auto message = value.toObject();
    if (message.value("role").toString() != QLatin1String("tool")) continue;
    const auto content = message.value("content").toString();
    const auto envelope = QJsonDocument::fromJson(content.toUtf8()).object();
    if (!envelope.contains("data")) continue; // 首帧是带 resultRefs 的摘要
    ++readEnvelopes;
    QVERIFY(paleo::chat::isResultEnvelope(envelope));
    QVERIFY(content.toUtf8().size() <= paleo::chat::kResultPageBytes);
    const auto data = envelope.value("data").toObject();
    QVERIFY(data.value("code").toString() == QLatin1String("ok"));
    if (data.value("hasMore").toBool()) {
      ++cursorPages;
      QVERIFY(!data.value("nextCursor").toString().isEmpty());
    }
    for (const auto &entry : data.value("entries").toArray())
      childEntries += entry.toObject().contains("child");
  }
  QCOMPARE(readEnvelopes, walker.pages);
  QVERIFY(childEntries >= 1); // 大成员经同一引用的 child selector 下钻
  QVERIFY(cursorPages >= 1);
  // 读页不重跑原查询：catalog 的 entityPage 只发生一次（原工具调用）。
  QCOMPARE(entityPages.load(), 1);
  // 对话里只有一个临时引用，读页不签发 result_ref 链。
  QCOMPARE(sessionToolRefs(controller.messages()).size(), 1);
  QCOMPARE(toolMessagesNamed(body, QStringLiteral("paleo.query_project")), 1);
  QCOMPARE(toolMessagesNamed(body, QStringLiteral("paleo.read_result")), walker.reads);
  QCOMPARE(started.size(), 1 + walker.reads);
}

void TestH7ResultWire::secondTurnContinuesTheSameTemporaryReference() {
  Host host;
  QStringList expected;
  AiChatController controller;
  QVERIFY(host.bind(controller, 22, &expected));
  m_sessions.append(controller.session().id);
  std::atomic_int entityPages{0};
  host.catalog.setReadObserver([&](const char *what) {
    if (QString::fromLatin1(what) == QLatin1String("entityPage")) ++entityPages;
  });

  // 第一轮：真实查询触发 resultRef；模型读 /wells 的第一页后结束。
  QString ref, selector;
  int turn = 1, readCalls = 0;
  bool turn1Read = false, turn2Read = false;
  QString error;
  LlmServer server;
  QVERIFY(server.start([&](int index, const QJsonObject &body) -> QJsonObject {
    if (turn == 1) {
      if (index == 0)
        return toolMessage({toolCall(QStringLiteral("call-q"), QStringLiteral("paleo.query_project"),
            {{"topic", "wells"}, {"page_size", 256}, {"max_bytes", 65536}})});
      if (!turn1Read) {
        const auto refs = lastToolEnvelope(body).value("resultRefs").toArray();
        if (refs.isEmpty()) { error = QStringLiteral("turn1 no ref"); return textMessage(QStringLiteral("error")); }
        ref = refs.first().toObject().value("ref").toString();
        selector = QStringLiteral("/wells");
        turn1Read = true;
        return toolMessage({toolCall(QStringLiteral("call-r%1").arg(++readCalls),
            QStringLiteral("paleo.read_result"),
            {{"result_ref", ref}, {"selector", selector}, {"page_size", 4}, {"max_bytes", 65536}})});
      }
      return textMessage(QStringLiteral("turn1 done"));
    }
    // 第二轮：用同一临时引用从 /wells 起点重新读取。
    if (!turn2Read) {
      turn2Read = true;
      return toolMessage({toolCall(QStringLiteral("call-s%1").arg(++readCalls),
          QStringLiteral("paleo.read_result"),
          {{"result_ref", ref}, {"selector", selector}, {"page_size", 64}, {"max_bytes", 65536}})});
    }
    const auto data = lastToolEnvelope(body).value("data").toObject();
    if (data.value("hasMore").toBool()) {
      return toolMessage({toolCall(QStringLiteral("call-s%1").arg(++readCalls),
          QStringLiteral("paleo.read_result"),
          {{"result_ref", ref}, {"selector", selector}, {"page_size", 64}, {"max_bytes", 65536},
           {"cursor", data.value("nextCursor")}})});
    }
    return textMessage(QStringLiteral("turn2 done"));
  }));

  controller.setConfig(LlmConfig::fromParts(server.endpoint(), QStringLiteral("h7-fixture"),
                                            QByteArrayLiteral("sk-h7-fixture"), false));
  QSignalSpy errors(&controller, &AiChatController::errorOccurred);
  controller.sendUserText(QStringLiteral("先读一页"));
  QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 30000);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  QVERIFY(!ref.isEmpty());
  const auto firstTurnBody = server.body(server.requests() - 1);
  QJsonObject firstPage;
  for (const auto &value : firstTurnBody.value("messages").toArray()) {
    const auto message = value.toObject();
    if (message.value("role").toString() != QLatin1String("tool")) continue;
    const auto envelope = QJsonDocument::fromJson(message.value("content").toString().toUtf8()).object();
    if (envelope.value("data").toObject().contains("entries")) firstPage = envelope;
  }
  QVERIFY(!firstPage.isEmpty());
  QVERIFY(firstPage.value("data").toObject().value("hasMore").toBool());

  // 第二轮：额度重置（仍是同一会话/TTL 内），同一 ref 可继续读取。
  turn = 2;
  const int before = server.requests();
  controller.sendUserText(QStringLiteral("继续读同一结果"));
  QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 30000);
  QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.first().first().toString()));
  QVERIFY(server.requests() > before);
  const auto last = lastToolEnvelope(server.body(server.requests() - 1));
  QVERIFY(paleo::chat::isResultEnvelope(last));
  QVERIFY(last.value("data").toObject().value("entries").toArray().size() > 0);
  QCOMPARE(entityPages.load(), 1); // 第二轮读页仍未重跑查询
}

void TestH7ResultWire::controlledPiPagesAndDuplicateFrames() {
  Host host;
  QStringList expected;
  AiChatController controller;
  QVERIFY(host.bind(controller, 0, nullptr));
  m_sessions.append(controller.session().id);
  controller.setPiRuntime({true, {}, {}});

  // 真实工具编码路径产生不可变临时引用（同一 store/HMAC/上限），随后由受控
  // peer 驱动 read_result，而不是把自造引用塞进 store。
  QJsonArray values;
  for (int i = 0; i < 10; ++i)
    values.append(QJsonObject{{"id", QStringLiteral("row-%1").arg(i)},
                              {"name", QStringLiteral("W-%1-%2").arg(i).arg(QString(30, QLatin1Char('y')))},
                              {"pad", QString(60, QLatin1Char('z'))}});
  const auto source = controller.toolRunner()->modelResult(
      {"source", "paleo.query_project", "{}"},
      QString::fromUtf8(QJsonDocument(QJsonObject{{"contract", "paleo.project.query.page.v1"},
                                                   {"wells", values}}).toJson(QJsonDocument::Compact)),
      true, 1024);
  const auto refs = source.value("resultRefs").toArray();
  QVERIFY(!refs.isEmpty());
  const auto reference = refs.first().toObject().value("ref").toString();
  QVERIFY(!reference.isEmpty());

  QString error;
  int reads = 0;
  QString selector{QStringLiteral("/wells")};
  QString cursor;
  bool finished = false;
  LlmServer server;
  QVERIFY(server.start([&](int index, const QJsonObject &body) -> QJsonObject {
    if (finished) return textMessage(QStringLiteral("pi done"));
    if (index == 0)
      return toolMessage({toolCall(QStringLiteral("pi-r0"), QStringLiteral("paleo.read_result"),
          {{"result_ref", reference}, {"selector", selector}, {"page_size", 5}, {"max_bytes", 65536}})});
    const auto envelope = lastToolEnvelope(body);
    const auto data = envelope.value("data").toObject();
    if (!paleo::chat::isResultEnvelope(envelope) || data.value("code").toString() != QLatin1String("ok")) {
      error = QStringLiteral("bad pi page: ") + QString::fromUtf8(paleo::chat::compactJson(envelope));
      return textMessage(QStringLiteral("error"));
    }
    if (data.value("hasMore").toBool()) {
      cursor = data.value("nextCursor").toString();
      if (cursor.isEmpty()) { error = QStringLiteral("pi hasMore without cursor"); return textMessage(QStringLiteral("error")); }
      return toolMessage({toolCall(QStringLiteral("pi-r%1").arg(++reads),
          QStringLiteral("paleo.read_result"),
          {{"result_ref", reference}, {"selector", selector}, {"cursor", cursor},
           {"page_size", 5}, {"max_bytes", 65536}})});
    }
    finished = true;
    return textMessage(QStringLiteral("pi read complete"));
  }));

  PagePeer peer;
  peer.duplicateFirst = true;
  controller.piBridge()->setWorkerDriver(
      [&peer](PaleoTask *task, const std::shared_ptr<paleo::pi::ConversationChannel> &channel,
              const std::function<void()> &notify) { return peer.run(task, channel, notify); });
  controller.setConfig(LlmConfig::fromParts(server.endpoint(), QStringLiteral("h7-fixture"),
                                            QByteArrayLiteral("sk-h7-fixture"), false));
  QSignalSpy errors(&controller, &AiChatController::errorOccurred);
  QSignalSpy started(&controller, &AiChatController::toolExecutionStarted);
  controller.sendUserText(QStringLiteral("通过 Pi 继续读取工具结果"));
  QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 60000);
  QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.first().first().toString()));
  QVERIFY2(error.isEmpty(), qPrintable(error));
  QVERIFY(finished);
  QVERIFY(peer.toolResults >= 2);
  // 重复 tool_request 复用原 content：工具执行次数不含重复帧。
  QCOMPARE(started.size(), reads + 1);
  QVERIFY(!peer.duplicatedSourceContent.isEmpty());
  QCOMPARE(peer.duplicateContent, peer.duplicatedSourceContent);
  // 每帧都是合法的、有界的最终 envelope（外层仍是 Pi frame，内层是工具内容）。
  for (int i = 0; i < server.requests(); ++i) {
    for (const auto &value : server.body(i).value("messages").toArray()) {
      const auto message = value.toObject();
      if (message.value("role").toString() != QLatin1String("tool")) continue;
      const auto content = message.value("content").toString();
      if (!content.contains("payloadContract")) continue;
      QVERIFY(content.toUtf8().size() <= paleo::chat::kResultPageBytes);
      const auto envelope = QJsonDocument::fromJson(content.toUtf8()).object();
      QVERIFY(paleo::chat::isResultEnvelope(envelope));
      QVERIFY(envelope.contains("data"));
    }
  }
}

// 直接驱动生产 runner（无 controller）：验证最终化按「请求上限 ∩ 宿主配额」
// 的生效值复核，而不是只按宿主配额重包。
void TestH7ResultWire::runnerFinalisationUsesEffectiveSubBudget() {
  AiChatToolRunner runner;
  runner.setResultSession(QStringLiteral("h7-runner"));
  QJsonArray rows;
  for (int i = 0; i < 80; ++i)
    rows.append(QJsonObject{{"id", QStringLiteral("row-%1").arg(i)},
                            {"name", QString(24, QLatin1Char('n'))},
                            {"pad", QString(40, QLatin1Char('p'))}});
  const auto seed = runner.modelResult(
      {"seed", "paleo.query_project", "{}"},
      QString::fromUtf8(QJsonDocument(QJsonObject{{"wells", rows}}).toJson(QJsonDocument::Compact)),
      true, 1024);
  const auto refs = seed.value("resultRefs").toArray();
  QVERIFY(!refs.isEmpty());
  const auto reference = refs.first().toObject().value("ref").toString();
  const QJsonObject args{{"result_ref", reference}, {"selector", "/wells"},
                         {"page_size", 8}, {"max_bytes", 1024}};

  // 1) 宿主额度（4096）大于请求上限（1024）：页按 1024 生成；最终化若用 4096
  //    重包会把 transport.maxBytes 写成 4096 并可能超出请求上限。
  QSignalSpy first(&runner, &AiChatToolRunner::toolFinished);
  runner.run({"r1", "paleo.read_result",
              QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}, 4096);
  QVERIFY(QTest::qWaitFor([&] { return first.size() >= 1; }, 5000));
  QVERIFY(first.at(0).at(1).toBool());
  const auto page = first.at(0).at(2).toString();
  const auto delivered = runner.modelResult({"r1", "paleo.read_result", "{}"}, page, true, 4096);
  QVERIFY(paleo::chat::compactJson(delivered).size() <= 1024);
  QCOMPARE(delivered.value("transport").toObject().value("maxBytes").toInt(), 1024);
  QVERIFY(delivered.contains("data"));

  // 2) 最终额度被压小：明确交付失败（同一 cursor 可用更小 max_bytes 重试），
  //    不把成功页降级成无 data/无 cursor 的摘要。
  QSignalSpy second(&runner, &AiChatToolRunner::toolFinished);
  runner.run({"r2", "paleo.read_result",
              QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}, 4096);
  QVERIFY(QTest::qWaitFor([&] { return second.size() >= 1; }, 5000));
  const auto shrunk = runner.modelResult({"r2", "paleo.read_result", "{}"},
                                         second.at(0).at(2).toString(), true, 700);
  QCOMPARE(shrunk.value("transport").toObject().value("code").toString(),
           QStringLiteral("tool_reply_budget_changed"));
  QCOMPARE(shrunk.value("retryAdvice").toString(), QStringLiteral("retry_read_same_cursor"));
  QVERIFY(!shrunk.contains("data"));
  // 失败已消费并清理单槽：同 id 再用更小预算走普通编码（页预算不足），
  // 不会再次冒充“额度漂移”。
  const auto afterFailure = runner.modelResult({"r2", "paleo.read_result", "{}"},
                                               second.at(0).at(2).toString(), true, 700);
  QCOMPARE(afterFailure.value("transport").toObject().value("code").toString(),
           QStringLiteral("page_budget_exceeded"));
}

// 同一模型帧两个工具调用：各自冻结/携带自己的配额，结果一一对账，前后调用
// 不共享“下一次预算”。
void TestH7ResultWire::multiCallFrameKeepsIndependentQuotas() {
  Host host;
  QStringList expected;
  AiChatController controller;
  QVERIFY(host.bind(controller, 22, &expected));
  m_sessions.append(controller.session().id);
  std::atomic_int entityPages{0};
  host.catalog.setReadObserver([&](const char *what) {
    if (QString::fromLatin1(what) == QLatin1String("entityPage")) ++entityPages;
  });

  QString refA, refB;
  bool readIssued = false;
  LlmServer server;
  QVERIFY(server.start([&](int index, const QJsonObject &body) -> QJsonObject {
    if (index == 0) {
      const QJsonObject args{{"topic", "wells"}, {"page_size", 256}, {"max_bytes", 65536}};
      return toolMessage({toolCall(QStringLiteral("call-a"), QStringLiteral("paleo.query_project"), args),
                          toolCall(QStringLiteral("call-b"), QStringLiteral("paleo.query_project"), args)});
    }
    // provider id 会被宿主换绑成 host_*；按工具结果的追加顺序取两个引用
    // （串行派发 = call-a 的结果先到）。
    QStringList found;
    for (const auto &value : body.value("messages").toArray()) {
      const auto message = value.toObject();
      if (message.value("role").toString() != QLatin1String("tool")) continue;
      const auto envelope = QJsonDocument::fromJson(message.value("content").toString().toUtf8()).object();
      const auto refs = envelope.value("resultRefs").toArray();
      if (refs.isEmpty()) continue;
      found << refs.first().toObject().value("ref").toString();
    }
    if (found.size() >= 2) { refA = found.at(0); refB = found.at(1); }
    if (readIssued) return textMessage(QStringLiteral("multi done"));
    readIssued = true;
    return toolMessage({toolCall(QStringLiteral("call-r"), QStringLiteral("paleo.read_result"),
        {{"result_ref", refB}, {"selector", "/wells"}, {"page_size", 4}, {"max_bytes", 65536}})});
  }));

  controller.setConfig(LlmConfig::fromParts(server.endpoint(), QStringLiteral("h7-fixture"),
                                            QByteArrayLiteral("sk-h7-fixture"), false));
  QSignalSpy started(&controller, &AiChatController::toolExecutionStarted);
  QSignalSpy errors(&controller, &AiChatController::errorOccurred);
  controller.sendUserText(QStringLiteral("同一帧查两次，再读第二个结果"));
  QTRY_VERIFY_WITH_TIMEOUT(!controller.streaming(), 30000);
  QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.first().first().toString()));
  QVERIFY(!refA.isEmpty());
  QVERIFY(!refB.isEmpty());
  QVERIFY(refA != refB);
  QCOMPARE(entityPages.load(), 2); // 两个独立调用各执行一次原查询
  QCOMPARE(sessionToolRefs(controller.messages()).size(), 2);
  QCOMPARE(started.size(), 3);

  // 第二调用自己的结果可读，页 envelope 有界且 data/cursor 可达。
  const auto last = lastToolEnvelope(server.body(server.requests() - 1));
  QVERIFY(paleo::chat::isResultEnvelope(last));
  const auto data = last.value("data").toObject();
  QCOMPARE(data.value("code").toString(), QStringLiteral("ok"));
  QCOMPARE(data.value("returnedCount").toInt(), 4);
  QVERIFY(paleo::chat::compactJson(last).size() <=
          last.value("transport").toObject().value("maxBytes").toInt());
  QVERIFY(last.value("transport").toObject().value("maxBytes").toInt() <=
          paleo::chat::kResultPageBytes);
  // 两个调用各自的工具信封都不超过单页上限；每个 envelope 的
  // transport.maxBytes 就是该调用派发时冻结的配额——两次调用值不同，
  // 证明没有共享一个全局“下一次预算”。
  QVector<int> quotas;
  for (const auto &value : server.body(1).value("messages").toArray()) {
    const auto message = value.toObject();
    if (message.value("role").toString() != QLatin1String("tool")) continue;
    QVERIFY(message.value("content").toString().toUtf8().size() <= paleo::chat::kResultPageBytes);
    const auto envelope = QJsonDocument::fromJson(message.value("content").toString().toUtf8()).object();
    quotas << envelope.value("transport").toObject().value("maxBytes").toInt();
  }
  QCOMPARE(quotas.size(), 2);
  for (const int quota : quotas) {
    QVERIFY(quota >= paleo::chat::kMinimumEnvelopeBytes);
    QVERIFY(quota <= paleo::chat::kResultPageBytes);
  }
  QVERIFY(quotas.at(0) != quotas.at(1));
}

QTEST_GUILESS_MAIN(TestH7ResultWire)
#include "tst_h7_resultwire.moc"
