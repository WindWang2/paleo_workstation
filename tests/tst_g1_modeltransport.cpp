#include <QtTest>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <functional>

#include "../src/ai/chat/llmclient.h"

// G1（模型通信）传输回归：直连生产 LlmClient / LlmStreamParser。
//   · W1 attempt 冻结：在途请求不受 setConfig 影响；同步 send/cancel/delete
//     订阅者不产生串扰/双终态/UAF；
//   · W2 双时限：idle 可被真实到货重置、绝对总时限不被心跳/滴答延长；
//   · W3 按字节边界分帧：任意拆包 == 一次喂入（表驱动全边界等价）；
//   · W4 工具调用完整性与 choice 隔离：封印/翻覆/身份冲突/终局校验；
//   · W5 统一终态裁决：HTTP 优先、[DONE]/EOF/网络失败/tail 单一收口；
//   · W6 全路径字节预算：readyRead 与 finished tail 同一把尺子；
//   · W8 诊断脱敏：按 attempt 冻结密钥（配置切换后仍检旧 key）。
// 断言纪律：不断言墙钟时长，只断言语义与有界期限内的信号到达。

namespace {
constexpr qint64 kCap = 8ll * 1024 * 1024; // 与 llmclient.cpp 同值的响应上限

bool spinUntil(const std::function<bool()> &predicate, int timeoutMs = 8000) {
  QElapsedTimer clock;
  clock.start();
  while (!predicate() && clock.elapsed() < timeoutMs)
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  return predicate();
}

QByteArray sse(const QByteArray &json, bool done = false) {
  QByteArray out = "data: " + json + "\n\n";
  if (done)
    out += "data: [DONE]\n\n";
  return out;
}
QByteArray deltaEvent(const QString &text) {
  return sse(QJsonDocument(QJsonObject{
      {"choices", QJsonArray{QJsonObject{{"delta", QJsonObject{{"content", text}}}}}}})
                 .toJson(QJsonDocument::Compact));
}
QByteArray toolEvent(int index, const QString &name, const QString &args,
                     std::optional<QString> id = {}, bool finalFrame = true) {
  QJsonObject call;
  call.insert("index", index);
  if (id)
    call.insert("id", *id);
  QJsonObject function;
  function.insert("arguments", args);
  if (!name.isEmpty())
    function.insert("name", name);
  call.insert("function", function);
  QJsonObject delta;
  delta.insert("tool_calls", QJsonArray{call});
  QJsonObject choice;
  choice.insert("delta", delta);
  QJsonObject event;
  event.insert("choices", QJsonArray{choice});
  if (finalFrame)
    event.insert("usage", QJsonObject{{"prompt_tokens", 3}, {"completion_tokens", 4}});
  return sse(QJsonDocument(event).toJson(QJsonDocument::Compact));
}

// 脚本化本地 HTTP 服务器：每连接按脚本写响应（可分块、可重复心跳、可挂起）。
class RawServer : public QObject {
  Q_OBJECT
public:
  struct Response {
    int status = 200;
    QByteArray contentType = "text/event-stream";
    QByteArray extraHeaders;   // 形如 "Location: https://x\r\n"
    QList<QByteArray> writes;  // 依次写的 body 块
    int writeDelayMs = 15;     // 块间隔（让 readyRead 多次触发）
    bool closeAfter = true;    // 最后一块后关连接
    int repeatLastMs = 0;      // >0：每 N ms 重写最后一块且永不关（心跳/滴答）
    bool hold = false;         // 只回头部不写 body（逼 idle）
  };
  using Handler = std::function<Response()>;

  bool start(Handler handler) {
    m_handler = std::move(handler);
    connect(&m_server, &QTcpServer::newConnection, this, [this] {
      while (m_server.hasPendingConnections())
        serve(m_server.nextPendingConnection());
    });
    return m_server.listen(QHostAddress::LocalHost, 0);
  }
  QUrl endpoint() const {
    return QUrl(QStringLiteral("http://127.0.0.1:%1/v1").arg(m_server.serverPort()));
  }
  int requestCount() const { return m_requests; }
  qint64 bytesWritten() const { return m_bytesWritten; }
  const QList<QByteArray> &rawRequests() const { return m_raw; }

private:
  void serve(QTcpSocket *sock) {
    connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
    auto buffer = std::make_shared<QByteArray>();
    connect(sock, &QTcpSocket::readyRead, sock, [this, sock, buffer] {
      buffer->append(sock->readAll());
      const int headerEnd = buffer->indexOf("\r\n\r\n");
      if (headerEnd < 0)
        return;
      qsizetype length = 0;
      for (const QByteArray &line : buffer->left(headerEnd).split('\n'))
        if (line.toLower().startsWith("content-length:"))
          length = line.mid(15).trimmed().toLongLong();
      if (buffer->size() < headerEnd + 4 + length)
        return;
      m_raw.append(*buffer);
      ++m_requests;
      respond(sock, m_handler());
      *buffer = buffer->mid(headerEnd + 4 + length);
    });
  }
  void respond(QTcpSocket *sock, const Response &response) {
    if (response.hold)
      return; // 挂起不答：逼客户端空闲超时
    QByteArray head =
      "HTTP/1.1 " + QByteArray::number(response.status) + " OK\r\n"
      "Connection: close\r\nContent-Type: " + response.contentType + "\r\n" +
      response.extraHeaders + "\r\n";
    sock->write(head);
    const QList<QByteArray> writes = response.writes;
    const int delay = response.writeDelayMs;
    for (int i = 0; i < writes.size(); ++i)
      QTimer::singleShot(delay * (i + 1), sock,
                         [this, sock, bytes = writes.at(i)] {
                           m_bytesWritten += bytes.size();
                           sock->write(bytes);
                         });
    if (response.repeatLastMs > 0 && !writes.isEmpty()) {
      const QByteArray last = writes.last();
      const int repeat = response.repeatLastMs;
      auto *ticker = new QTimer(sock);
      ticker->start(repeat);
      connect(ticker, &QTimer::timeout, sock, [this, sock, ticker, last] {
        if (sock->state() != QAbstractSocket::ConnectedState) {
          ticker->stop();
          return;
        }
        m_bytesWritten += last.size();
        sock->write(last);
      });
      return; // 永不主动关：由总时限/测试收尾断连
    }
    if (response.closeAfter)
      QTimer::singleShot(delay * (writes.size() + 1), sock,
                         [sock] { sock->disconnectFromHost(); });
  }

  Handler m_handler;
  QTcpServer m_server;
  int m_requests = 0;
  qint64 m_bytesWritten = 0;
  QList<QByteArray> m_raw;
};

// QNetworkReply 替身：确定性制造 readyRead / finished / tail 序列（W6）。
class FakeReply : public QNetworkReply {
  Q_OBJECT
public:
  explicit FakeReply(QObject *parent) : QNetworkReply(parent) {
    setOpenMode(QIODevice::ReadOnly | QIODevice::Unbuffered);
  }
  void setStatusCode(int status) {
    setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
  }
  void setContentType(const QByteArray &type) {
    setHeader(QNetworkRequest::ContentTypeHeader, QString::fromLatin1(type));
  }
  // 触发一次 readyRead（字节立即对客户端可见）。
  void deliver(const QByteArray &chunk) {
    m_buffer += chunk;
    emit readyRead();
  }
  // 只入缓冲不触发 readyRead：留给 finished 时刻的 readAll（确定性 tail）。
  void queueTail(const QByteArray &chunk) { m_buffer += chunk; }
  void finishNow() {
    m_finished = true;
    emit finished();
  }
  void abort() override {
    if (m_finished)
      return;
    m_finished = true;
    emit finished();
  }
  qint64 writeData(const char *, qint64) override { return -1; } // 只读
  qint64 bytesAvailable() const override {
    return m_buffer.size() + QNetworkReply::bytesAvailable();
  }

protected:
  qint64 readData(char *data, qint64 maxSize) override {
    const qint64 taken = qMin(maxSize, qint64(m_buffer.size()));
    memcpy(data, m_buffer.constData(), taken);
    m_buffer.remove(0, int(taken));
    return taken;
  }
  void close() override {}

private:
  QByteArray m_buffer;
  bool m_finished = false;
};

class FakeNam : public QNetworkAccessManager {
  Q_OBJECT
public:
  std::function<FakeReply *()> creator;
  QPointer<FakeReply> lastReply;

protected:
  QNetworkReply *createRequest(Operation, const QNetworkRequest &,
                               QIODevice *) override {
    auto *reply = creator ? creator() : new FakeReply(this);
    lastReply = reply;
    return reply;
  }
};

LlmConfig clientConfig(const QUrl &endpoint, const QByteArray &key = "sk-fixture",
                       bool stream = true, int idleMs = 5000,
                       int totalMs = 30000) {
  LlmConfig config;
  config.endpoint = endpoint;
  config.model = QStringLiteral("test-model");
  config.stream = stream;
  config.timeoutMs = idleMs;
  config.totalTimeoutMs = totalMs;
  config.apiKey = key;
  return config;
}

struct Recorder {
  QStringList deltas;
  QVector<ChatToolCall> calls;
  int finished = 0;
  QString finishReason;
  int promptTokens = -1;
  int completionTokens = -1;
  int errors = 0;
  QList<LlmErrorKind> errorKinds;
  QStringList errorMessages;
  void attach(LlmClient *client) {
    QObject::connect(client, &LlmClient::deltaReceived, client,
                     [this](const QString &text) { deltas.append(text); });
    QObject::connect(client, &LlmClient::toolCallReceived, client,
                     [this](const ChatToolCall &call) { calls.append(call); });
    QObject::connect(client, &LlmClient::finished, client,
                     [this](const QString &reason, int prompt, int completion) {
                       ++finished;
                       finishReason = reason;
                       promptTokens = prompt;
                       completionTokens = completion;
                     });
    QObject::connect(client, &LlmClient::errorOccurred, client,
                     [this](LlmErrorKind kind, const QString &message) {
                       ++errors;
                       errorKinds.append(kind);
                       errorMessages.append(message);
                     });
  }
  QString joined() const { return deltas.join(QString()); }
};

struct ParserOutcome {
  QString deltas;
  QVector<ChatToolCall> calls;
  bool done = false;
  QString error;
  QString finishReason;
  int promptTokens = 0;
  int completionTokens = 0;

  bool operator==(const ParserOutcome &other) const {
    return deltas == other.deltas && calls == other.calls &&
           done == other.done && error == other.error &&
           finishReason == other.finishReason &&
           promptTokens == other.promptTokens &&
           completionTokens == other.completionTokens;
  }
};

// 按给定喂入策略跑一遍 parser，汇总终局结果（事件序列的可比较形态）。
ParserOutcome runParser(const QByteArray &payload, bool byteByByte) {
  LlmStreamParser parser;
  ParserOutcome out;
  auto collect = [&parser, &out](bool *done, QString *feedError) {
    if (done && *done)
      out.done = true;
    if (feedError && !feedError->isEmpty() && out.error.isEmpty())
      out.error = *feedError;
    for (const ChatToolCall &call : parser.takeCompleteCalls(&out.error))
      out.calls.append(call);
  };
  if (byteByByte) {
    for (int i = 0; i < payload.size(); ++i) {
      QStringList deltas;
      bool done = false;
      QString error;
      parser.feed(payload.mid(i, 1), &deltas, &done, &error);
      for (const QString &text : deltas)
        out.deltas += text;
      collect(&done, &error);
    }
  } else {
    QStringList deltas;
    bool done = false;
    QString error;
    parser.feed(payload, &deltas, &done, &error);
    for (const QString &text : deltas)
      out.deltas += text;
    collect(&done, &error);
  }
  QString flushError;
  for (const ChatToolCall &call : parser.flush(&flushError))
    out.calls.append(call);
  if (out.error.isEmpty())
    out.error = flushError;
  out.finishReason = parser.finishReason();
  out.promptTokens = parser.promptTokens;
  out.completionTokens = parser.completionTokens;
  return out;
}
} // namespace

class TestG1ModelTransport : public QObject {
  Q_OBJECT
private slots:
  // W3：任意拆包 == 一次喂入（表驱动全边界等价）。
  void parserByteSplitEquivalence_data();
  void parserByteSplitEquivalence();
  // W3：跨块 CRLF/裸 CR/BOM 的显式边界（等价表之外的可读钉子）。
  void parserCrlfSplitAcrossChunksCountsOneBreak();
  void parserBareCrIsALineBreak();
  void parserStripsBomOnceAtStreamStart();
  void parserOnlyStripsSingleLeadingSpace();
  // W4：choice 隔离与工具调用完整性。
  void parserIgnoresNonZeroChoices();
  void parserSealedCallCannotBeExtended();
  void parserIdentityConflictIsReported();
  void parserTerminalRejectsIncompleteArguments();
  void parserTerminalRejectsMissingName();
  void parserExplicitEmptyObjectIsSealed();
  void parserHugeIndexIsBounded();
  // W1：attempt 冻结与同步订阅者。
  void attemptFreezesStreamEndpointAndKey();
  void sendReplacesAttemptOldAttemptStaysSilent();
  void syncCancelInsideDeltaSlot();
  void syncSendInsideDeltaSlot();
  void deleteClientInsideDeltaSlot();
  // W2：双时限。
  void idleTimeoutWhenNoDataArrives();
  void totalDeadlineNotExtendedByHeartbeat();
  void totalDeadlineNotExtendedByProgress();
  void progressingStreamWithinBudgetFinishes();
  // W5：统一裁决。
  void unauthorizedWithSseShapeYieldsZeroDeltasAndAuthError();
  void redirectLocationIsRedacted();
  void eofWithBrokenTailFailsMalformed();
  void normalCloseWithoutDoneTolerated();
  void errorEnvelope200FailsHonestly();
  void emptyChoicesNonStreamFails();
  void nonObjectMessageFails();
  void finishReasonAndUsagePassThrough();
  void zeroEventStreamFails();
  void reentrantCancelOnErrorKeepsSingleTerminal();
  // W6：预算。
  void budgetExactCapSucceeds();
  void budgetOneByteOverFails();
  void budgetTailOnlyOverflowFails();
  void budgetUnclosedLineHitsCap();
  // W8：诊断按 attempt 密钥脱敏（配置切换后仍检旧 key）。
  void diagnosticsRedactFrozenAttemptKey();

private:
  QVector<ChatMessage> oneShot() const {
    ChatMessage message;
    message.role = ChatRole::User;
    message.content = QStringLiteral("hi");
    return {message};
  }
};

// ---- W3 表驱动等价 --------------------------------------------------------

void TestG1ModelTransport::parserByteSplitEquivalence_data() {
  QTest::addColumn<QByteArray>("payload");
  const char *simple =
    "data: {\"choices\":[{\"delta\":{\"content\":\"砂岩\"}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"为主\"}}],"
    "\"usage\":{\"prompt_tokens\":11,\"completion_tokens\":2}}\n\n"
    "data: [DONE]\n\n";
  QTest::newRow("lf-simple") << QByteArray(simple);
  QTest::newRow("crlf-endings")
    << QByteArray("data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\r\n"
                  "\r\n"
                  "data: [DONE]\r\n\r\n");
  QTest::newRow("bare-cr-endings")
    << QByteArray("data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\r"
                  "\r"
                  "data: [DONE]\r\r");
  QTest::newRow("mixed-endings")
    << QByteArray("data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\r\n"
                  "\n"
                  "data: [DONE]\n\r");
  QTest::newRow("bom-prefix")
    << QByteArray("\xEF\xBB\xBF"
                  "data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\n\n"
                  "data: [DONE]\n\n");
  QTest::newRow("comments-and-heartbeats")
    << QByteArray(": ping\n\n"
                  "data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\n\n"
                  ": keepalive\n\n"
                  "data: [DONE]\n\n");
  QTest::newRow("multi-data-lines")
    << QByteArray("data: {\"choices\":\n"
                  "data: [{\"delta\":{\"content\":\"a\"}}]}\n\n"
                  "data: [DONE]\n\n");
  QTest::newRow("utf8-emoji")
    << QByteArray("data: {\"choices\":[{\"delta\":{\"content\":\"岩性🔍\"}}]}\n\n"
                  "data: [DONE]\n\n");
  QTest::newRow("leading-space-after-colon")
    << QByteArray("data:  {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\n\n"
                  "data:   [DONE]\n\n");
  QTest::newRow("done-then-trailing-content")
    << QByteArray("data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\n\n"
                  "data: [DONE]\n\n"
                  "data: {\"choices\":[{\"delta\":{\"content\":\"late\"}}]}\n\n");
  QTest::newRow("tool-call-then-done")
    << QByteArray("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                  "\"id\":\"c1\",\"type\":\"function\",\"function\":"
                  "{\"name\":\"paleo.query_project\",\"arguments\":\"{\\\"topic\\\":"
                  "\\\"wells\\\"}\"}}]}}]}\n\n"
                  "data: [DONE]\n\n");
  QTest::newRow("tool-call-split-fragments")
    << QByteArray("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                  "\"id\":\"c1\",\"function\":{\"name\":\"paleo.query_project\","
                  "\"arguments\":\"{\\\"topic\\\"\"}}]}}]}\n\n"
                  "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                  "\"function\":{\"arguments\":\":\\\"wells\\\"}\"}}]}}]}\n\n"
                  "data: [DONE]\n\n");
  QTest::newRow("finish-reason-event")
    << QByteArray("data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\n\n"
                  "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"length\"}]}\n\n"
                  "data: [DONE]\n\n");
  QTest::newRow("empty-data-event")
    << QByteArray("data: \n\n"
                  "data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\n\n"
                  "data: [DONE]\n\n");
}

void TestG1ModelTransport::parserByteSplitEquivalence() {
  QFETCH(QByteArray, payload);
  const ParserOutcome oneShot = runParser(payload, false);
  const ParserOutcome perByte = runParser(payload, true);
  QVERIFY2(oneShot == perByte,
           qPrintable(QStringLiteral("任意拆包必须与一次喂入等价：\n整喂 "
                                     "deltas=%1 calls=%2 err=%3\n逐字节 "
                                     "deltas=%4 calls=%5 err=%6")
                        .arg(oneShot.deltas)
                        .arg(oneShot.calls.size())
                        .arg(oneShot.error)
                        .arg(perByte.deltas)
                        .arg(perByte.calls.size())
                        .arg(perByte.error)));
}

void TestG1ModelTransport::parserCrlfSplitAcrossChunksCountsOneBreak() {
  LlmStreamParser parser;
  QStringList deltas;
  bool done = false;
  QString error;
  parser.feed("data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\r",
              &deltas, &done, &error);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  QVERIFY(deltas.isEmpty()); // CR 可能是 CRLF 前半：不得提前成行
  parser.feed("\n\r\n", &deltas, &done, &error); // 补上 LF + 空行(CRLF)
  QCOMPARE(deltas.join(QString()), QStringLiteral("a"));
  QVERIFY(error.isEmpty());
}

void TestG1ModelTransport::parserBareCrIsALineBreak() {
  LlmStreamParser parser;
  QStringList deltas;
  bool done = false;
  QString error;
  // 末尾孤立 CR 可能是 CRLF 前半：不提前定界（增量协议的正确行为）。
  parser.feed("data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\r",
              &deltas, &done, &error);
  QVERIFY(deltas.isEmpty());
  parser.feed("\r\n", &deltas, &done, &error); // 第二个 CR 实为 CRLF：空行分发
  QCOMPARE(deltas.join(QString()), QStringLiteral("a"));
  QVERIFY(error.isEmpty());
  parser.feed("data: [DONE]\r", &deltas, &done, &error);
  QVERIFY(!done);
  parser.feed("\r", &deltas, &done, &error); // 裸 CR：[DONE] 行已定界
  QVERIFY(!done);                            // 但空行未到：事件不分发
  parser.feed("\n", &deltas, &done, &error); // 补齐 CRLF 空行
  QVERIFY(done);
}

void TestG1ModelTransport::parserStripsBomOnceAtStreamStart() {
  LlmStreamParser parser;
  QStringList deltas;
  bool done = false;
  QString error;
  parser.feed(QByteArray("\xEF\xBB", 2), &deltas, &done, &error); // 半个 BOM
  QVERIFY(error.isEmpty());
  parser.feed(QByteArray("\xBF", 1) +
                  "data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\n\n",
              &deltas, &done, &error);
  QCOMPARE(deltas.join(QString()), QStringLiteral("a")); // BOM 跨块也只剥一次
  QVERIFY(error.isEmpty());
}

void TestG1ModelTransport::parserOnlyStripsSingleLeadingSpace() {
  LlmStreamParser parser;
  QStringList deltas;
  bool done = false;
  QString error;
  // 两个空格：剥一个，第二个属于载荷——JSON 容忍前导空白，仍应解析成功；
  // 关键是不做 trimmed（值语义不被改写）。
  parser.feed("data:  {\"choices\":[{\"delta\":{\"content\":\" a\"}}]}\n\n",
              &deltas, &done, &error);
  QCOMPARE(deltas.join(QString()), QStringLiteral(" a"));
  QVERIFY2(error.isEmpty(), qPrintable(error));
}

// ---- W4 ------------------------------------------------------------------

void TestG1ModelTransport::parserIgnoresNonZeroChoices() {
  LlmStreamParser parser;
  QStringList deltas;
  bool done = false;
  QString error;
  // choice 1 携带文本与 index=0 工具调用：全部不得混入 choice 0 的产出。
  const QByteArray event = sse(QJsonDocument(QJsonObject{
      {"choices", QJsonArray{
          QJsonObject{{"index", 0}, {"delta", QJsonObject{{"content", "ok"}}}},
          QJsonObject{{"index", 1}, {"delta", QJsonObject{
              {"content", "EVIL"},
              {"tool_calls", QJsonArray{QJsonObject{
                  {"index", 0}, {"id", "evil"}, {"function", QJsonObject{
                      {"name", "paleo.query_project"}, {"arguments", "{\"evil\":1}"}}}}}}}}}}}})
                 .toJson(QJsonDocument::Compact));
  parser.feed(event, &deltas, &done, &error);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  QCOMPARE(deltas.join(QString()), QStringLiteral("ok")); // choice 1 文本被丢
  parser.feed("data: [DONE]\n\n", &deltas, &done, &error);
  QString callError;
  const auto calls = parser.flush(&callError);
  QCOMPARE(calls.size(), 0); // choice 1 的工具不与 choice 0 混拼
  QVERIFY2(callError.isEmpty(), qPrintable(callError));
}

void TestG1ModelTransport::parserSealedCallCannotBeExtended() {
  LlmStreamParser parser;
  QStringList deltas;
  bool done = false;
  QString error;
  parser.feed(toolEvent(0, "paleo.query_project", "{\"topic\":\"wells\"}", "c1"),
              &deltas, &done, &error);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  QCOMPARE(parser.takeCompleteCalls(&error).size(), 1); // 完整帧流中即交付
  // 临时合法后再追加：协议翻覆——已交付内容不得被推翻。
  parser.feed(toolEvent(0, "", ",\"more\":2}"), &deltas, &done, &error);
  QVERIFY2(!error.isEmpty(), "封印后的追加分片必须报错");
  QVERIFY(error.contains(QStringLiteral("分片")));
}

void TestG1ModelTransport::parserIdentityConflictIsReported() {
  LlmStreamParser parser;
  QStringList deltas;
  bool done = false;
  QString error;
  // 注意：不先 take（草稿留表内）——冲突判在事件处理期，与是否已交付无关。
  parser.feed(toolEvent(0, "paleo.query_project", "{\"a\":1}", "c1"), &deltas,
              &done, &error);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  parser.feed(toolEvent(0, "", "", "c2"), &deltas, &done, &error);
  QVERIFY2(!error.isEmpty(), "同 index 换 id 必须报错");
  QVERIFY(error.contains(QStringLiteral("冲突")));
}

void TestG1ModelTransport::parserTerminalRejectsIncompleteArguments() {
  LlmStreamParser parser;
  QStringList deltas;
  bool done = false;
  QString error;
  parser.feed(toolEvent(0, "paleo.query_project", "{\"topic\":"), &deltas,
              &done, &error);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  parser.feed("data: [DONE]\n\n", &deltas, &done, &error);
  QString terminalError;
  const auto calls = parser.flush(&terminalError);
  QCOMPARE(calls.size(), 0); // 残缺实参不得产出，也不得改造成 {}
  QVERIFY2(!terminalError.isEmpty(), "终局残缺实参必须报错");
  QVERIFY(terminalError.contains(QStringLiteral("不是合法 JSON 对象")));
}

void TestG1ModelTransport::parserTerminalRejectsMissingName() {
  LlmStreamParser parser;
  QStringList deltas;
  bool done = false;
  QString error;
  // 有 id 有实参、始终缺 name：终局如实报错（不得无声丢弃/改成无参）。
  parser.feed(toolEvent(3, "", "{\"a\":1}", "c9"), &deltas, &done, &error);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  parser.feed("data: [DONE]\n\n", &deltas, &done, &error);
  QString terminalError;
  const auto calls = parser.flush(&terminalError);
  QCOMPARE(calls.size(), 0);
  QVERIFY2(!terminalError.isEmpty(), "缺 name 的残帧必须报错");
  QVERIFY(terminalError.contains(QStringLiteral("缺 name")));
}

void TestG1ModelTransport::parserExplicitEmptyObjectIsSealed() {
  LlmStreamParser parser;
  QStringList deltas;
  bool done = false;
  QString error;
  parser.feed(toolEvent(0, "paleo.python_environment", "{}", "c1"), &deltas,
              &done, &error);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  const auto calls = parser.takeCompleteCalls(&error);
  QCOMPARE(calls.size(), 1);
  QCOMPARE(calls.first().argumentsJson, QStringLiteral("{}"));
  QVERIFY(calls.first().hasValidArguments());
}

void TestG1ModelTransport::parserHugeIndexIsBounded() {
  LlmStreamParser parser;
  QStringList deltas;
  bool done = false;
  QString error;
  parser.feed(toolEvent(2147483647, "paleo.query_project", "{\"topic\":\"wells\"}",
                        "big"), &deltas, &done, &error);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  const auto calls = parser.takeCompleteCalls(&error);
  QCOMPARE(calls.size(), 1); // 异常大 index：不崩溃、不误序、照常唯一交付
  QCOMPARE(calls.first().id, QStringLiteral("big"));
}

// ---- W1 ------------------------------------------------------------------

void TestG1ModelTransport::attemptFreezesStreamEndpointAndKey() {
  // A：流式、端点 A、key K1。事件分期到达；中途 setConfig 切到非流式端点 B
  // 再发 B——A 的请求/解析仍按 A 的冻结快照，B 按 B 的。
  RawServer serverA;
  QVERIFY(serverA.start([] {
    return RawServer::Response{
        .writes = {deltaEvent("砂岩"), deltaEvent("为主"), sse({}, true)},
        .writeDelayMs = 60};
  }));
  RawServer serverB;
  QVERIFY(serverB.start([] {
    return RawServer::Response{.contentType = "application/json",
                               .writes = {"{\"choices\":[{\"index\":0,"
                                          "\"finish_reason\":\"stop\",\"message\":"
                                          "{\"role\":\"assistant\",\"content\":"
                                          "\"石灰岩\"}}],\"usage\":"
                                          "{\"prompt_tokens\":7,"
                                          "\"completion_tokens\":3}}"},
                               .writeDelayMs = 10};
  }));
  LlmClient client;
  Recorder recorder;
  recorder.attach(&client);
  client.setConfig(clientConfig(serverA.endpoint(), "sk-A", true));
  client.send(oneShot());
  QVERIFY2(spinUntil([&] { return recorder.deltas.size() >= 1; }),
           "A 的首个增量必须先到");
  // A 在途：切换配置并发起 B（非流式、端点 B）。
  client.setConfig(clientConfig(serverB.endpoint(), "sk-B", false));
  client.send(oneShot());
  QVERIFY2(spinUntil([&] { return recorder.finished >= 1; }),
           "B（非流式）必须完成");
  // A 被有效 send 替换：A 只交付切换前已到的首个增量，后续事件（"为主"）
  // 属旧 attempt，静默。
  QCOMPARE(serverA.requestCount(), 1);
  QCOMPARE(serverB.requestCount(), 1);
  QCOMPARE(recorder.joined(), QStringLiteral("砂岩石灰岩"));
  QVERIFY(!recorder.joined().contains(QStringLiteral("为主")));
  QCOMPARE(recorder.finishReason, QStringLiteral("stop"));
  QCOMPARE(recorder.promptTokens, 7);
  QCOMPARE(recorder.completionTokens, 3);
  // A 的请求按冻结快照走了流式 Accept；B 按非流式 Accept + 端点 B。
  QVERIFY(serverA.rawRequests().first().contains("text/event-stream"));
  QVERIFY(serverA.rawRequests().first().contains("Bearer sk-A"));
  QVERIFY(serverB.rawRequests().first().contains("application/json"));
  QVERIFY(serverB.rawRequests().first().contains("Bearer sk-B"));
  QVERIFY(serverB.rawRequests().first().contains("/v1/chat/completions"));
}

void TestG1ModelTransport::sendReplacesAttemptOldAttemptStaysSilent() {
  // A 挂起（hold）；send B 到另一服务器正常完成——A 之后到任何信号都属
  // 旧 attempt，必须静默。
  RawServer serverA;
  QVERIFY(serverA.start([] { return RawServer::Response{.hold = true}; }));
  RawServer serverB;
  QVERIFY(serverB.start([] {
    return RawServer::Response{.writes = {deltaEvent("done"), sse({}, true)}};
  }));
  LlmClient client;
  Recorder recorder;
  recorder.attach(&client);
  client.setConfig(clientConfig(serverA.endpoint(), "sk-A", true, 60000, 60000));
  client.send(oneShot());
  QVERIFY(client.busy());
  client.setConfig(clientConfig(serverB.endpoint(), "sk-B"));
  client.send(oneShot());
  QVERIFY2(spinUntil([&] { return recorder.finished >= 1; }),
           "B 必须完成");
  QCOMPARE(recorder.errors, 0);
  QCOMPARE(recorder.joined(), QStringLiteral("done"));
  // 再转几轮事件：A 的迟到信号（若有）不得出现。
  spinUntil([] { return false; }, 150);
  QCOMPARE(recorder.finished, 1);
  QCOMPARE(recorder.deltas.size(), 1);
  QVERIFY(!client.busy());
}

void TestG1ModelTransport::syncCancelInsideDeltaSlot() {
  RawServer server;
  QVERIFY(server.start([] {
    return RawServer::Response{
        .writes = {deltaEvent("a"), deltaEvent("b"), sse({}, true)},
        .writeDelayMs = 60};
  }));
  LlmClient client;
  Recorder recorder;
  int cancels = 0;
  QObject::connect(&client, &LlmClient::deltaReceived, &client,
                   [&](const QString &) {
                     if (cancels++ == 0)
                       client.cancel(); // 首增量同步取消
                   });
  recorder.attach(&client);
  client.setConfig(clientConfig(server.endpoint()));
  client.send(oneShot());
  spinUntil([] { return false; }, 400);
  QCOMPARE(recorder.deltas.size(), 1); // 取消后旧 attempt 静默
  QCOMPARE(recorder.finished, 0);
  QCOMPARE(recorder.errors, 0);
  QVERIFY(!client.busy());
}

void TestG1ModelTransport::syncSendInsideDeltaSlot() {
  RawServer server;
  int sends = 0;
  QVERIFY(server.start([] {
    return RawServer::Response{
        .writes = {deltaEvent("first"), deltaEvent("second"), sse({}, true)},
        .writeDelayMs = 60};
  }));
  LlmClient client;
  Recorder recorder;
  client.setConfig(clientConfig(server.endpoint()));
  QObject::connect(&client, &LlmClient::deltaReceived, &client,
                   [&](const QString &) {
                     if (sends++ == 0)
                       client.send(oneShot()); // 首增量同步再发
                   });
  recorder.attach(&client);
  client.send(oneShot());
  QVERIFY2(spinUntil([&] { return recorder.finished >= 1; }, 5000),
           "重发轮必须完成");
  QCOMPARE(recorder.finished, 1);      // 旧 attempt 无终态
  QCOMPARE(recorder.errors, 0);
  QCOMPARE(server.requestCount(), 2);
}

void TestG1ModelTransport::deleteClientInsideDeltaSlot() {
  RawServer server;
  QVERIFY(server.start([] {
    return RawServer::Response{
        .writes = {deltaEvent("a"), deltaEvent("b"), sse({}, true)},
        .writeDelayMs = 60};
  }));
  auto *client = new LlmClient;
  client->setConfig(clientConfig(server.endpoint()));
  int deltas = 0;
  QObject::connect(client, &LlmClient::deltaReceived, client,
                   [&](const QString &) {
                     ++deltas;
                     delete client; // 订阅者直接删除 client
                   });
  client->send(oneShot());
  spinUntil([] { return false; }, 400);
  QCOMPARE(deltas, 1); // 删除后 lambda 守卫生效，不再触碰成员
}

// ---- W2 ------------------------------------------------------------------

void TestG1ModelTransport::idleTimeoutWhenNoDataArrives() {
  RawServer server;
  QVERIFY(server.start([] { return RawServer::Response{.hold = true}; }));
  LlmClient client;
  Recorder recorder;
  recorder.attach(&client);
  client.setConfig(clientConfig(server.endpoint(), "sk-x", true, 400, 30000));
  client.send(oneShot());
  QVERIFY2(spinUntil([&] { return recorder.errors >= 1; }, 4000),
           "无数据必须以空闲超时收尾");
  QCOMPARE(recorder.errorKinds.first(), LlmErrorKind::Timeout);
  QVERIFY2(recorder.errorMessages.first().contains(QStringLiteral("空闲超时")),
           qPrintable(recorder.errorMessages.first()));
  QVERIFY(!recorder.errorMessages.first().contains(QStringLiteral("总时限")));
  QCOMPARE(recorder.finished, 0);
  QVERIFY(!client.busy()); // 超时完成 reply 释放
}

void TestG1ModelTransport::totalDeadlineNotExtendedByHeartbeat() {
  // 心跳（SSE 注释）间隔小于 idle：idle 永不触发，但总时限必须终结。
  RawServer server;
  QVERIFY(server.start([] {
    return RawServer::Response{.writes = {QByteArrayLiteral(": ping\n\n")},
                               .repeatLastMs = 100};
  }));
  LlmClient client;
  Recorder recorder;
  recorder.attach(&client);
  client.setConfig(clientConfig(server.endpoint(), "sk-x", true, 2000, 900));
  client.send(oneShot());
  QVERIFY2(spinUntil([&] { return recorder.errors >= 1; }, 4000),
           "滴答不歇的流必须由总时限终结");
  QCOMPARE(recorder.errorKinds.first(), LlmErrorKind::Timeout);
  QVERIFY2(recorder.errorMessages.first().contains(QStringLiteral("总时限")),
           qPrintable(recorder.errorMessages.first()));
  QCOMPARE(recorder.deltas.size(), 0); // 心跳不是内容
  QCOMPARE(recorder.finished, 0);
  QVERIFY(!client.busy());
}

void TestG1ModelTransport::totalDeadlineNotExtendedByProgress() {
  // 持续合法增量但永不终结：idle 一直被重置，总时限仍有界结束——两条路径
  // 必须可区分（文案），且都不是成功。
  RawServer server;
  QVERIFY(server.start([] {
    return RawServer::Response{.writes = {deltaEvent("chunk")}, .repeatLastMs = 100};
  }));
  LlmClient client;
  Recorder recorder;
  recorder.attach(&client);
  client.setConfig(clientConfig(server.endpoint(), "sk-x", true, 2000, 900));
  client.send(oneShot());
  QVERIFY2(spinUntil([&] { return recorder.errors >= 1; }, 4000),
           "不终结的合法流必须由总时限终结");
  QCOMPARE(recorder.errorKinds.first(), LlmErrorKind::Timeout);
  QVERIFY(recorder.errorMessages.first().contains(QStringLiteral("总时限")));
  QVERIFY(recorder.deltas.size() > 0); // 有真实进展（区别于零数据 idle）
  QCOMPARE(recorder.finished, 0);      // 但不得算成功
  QVERIFY(!client.busy());
}

void TestG1ModelTransport::progressingStreamWithinBudgetFinishes() {
  // 正常有进展的长回答（增量间隔 < idle，总时长 << 总时限）：正常完成。
  RawServer server;
  QVERIFY(server.start([] {
    return RawServer::Response{
        .writes = {deltaEvent("地"), deltaEvent("层"), deltaEvent("剖面"),
                   sse({}, true)},
        .writeDelayMs = 50};
  }));
  LlmClient client;
  Recorder recorder;
  recorder.attach(&client);
  client.setConfig(clientConfig(server.endpoint(), "sk-x", true, 1000, 8000));
  client.send(oneShot());
  QVERIFY2(spinUntil([&] { return recorder.finished >= 1; }, 4000),
           "预算内的正常流必须完成");
  QCOMPARE(recorder.joined(), QStringLiteral("地层剖面"));
  QCOMPARE(recorder.errors, 0);
}

// ---- W5 ------------------------------------------------------------------

void TestG1ModelTransport::unauthorizedWithSseShapeYieldsZeroDeltasAndAuthError() {
  // 401 + SSE 外形（含内容和 [DONE]）：HTTP 分类优先，零内容派发、零成功。
  const QByteArray body = deltaEvent("secret-ish") + sse({}, true);
  RawServer server;
  QVERIFY(server.start([&] {
    return RawServer::Response{.status = 401,
                               .contentType = "text/event-stream",
                               .writes = {body}};
  }));
  LlmClient client;
  Recorder recorder;
  recorder.attach(&client);
  client.setConfig(clientConfig(server.endpoint()));
  client.send(oneShot());
  QVERIFY2(spinUntil([&] { return recorder.errors >= 1; }, 4000),
           "401 必须报错");
  QCOMPARE(recorder.errorKinds.first(), LlmErrorKind::Unauthorized);
  QCOMPARE(recorder.deltas.size(), 0); // 非 2xx 的 SSE 外形不派发内容
  QCOMPARE(recorder.calls.size(), 0);
  QCOMPARE(recorder.finished, 0);
}

void TestG1ModelTransport::redirectLocationIsRedacted() {
  const QByteArray key = "sk-redirect-secret-0123456789";
  RawServer server;
  QVERIFY(server.start([&] {
    return RawServer::Response{
        .status = 302,
        .contentType = "text/plain",
        .extraHeaders = "Location: https://evil.example/hook?key=" + key + "\r\n",
        .writes = {QByteArray()}};
  }));
  LlmClient client;
  Recorder recorder;
  recorder.attach(&client);
  client.setConfig(clientConfig(server.endpoint(), key));
  client.send(oneShot());
  QVERIFY2(spinUntil([&] { return recorder.errors >= 1; }, 4000),
           "重定向必须报错（不跟随）");
  QCOMPARE(recorder.errorKinds.first(), LlmErrorKind::Network);
  QVERIFY2(!recorder.errorMessages.first().contains(QLatin1String(key)),
           "Location 里的密钥必须脱敏后才能进诊断");
  QVERIFY(recorder.errorMessages.first().contains(QStringLiteral("[已隐藏]")));
}

void TestG1ModelTransport::eofWithBrokenTailFailsMalformed() {
  RawServer server;
  QVERIFY(server.start([] {
    return RawServer::Response{
        .writes = {deltaEvent("部分"), QByteArrayLiteral("data: {broken")},
        .writeDelayMs = 30};
  }));
  LlmClient client;
  Recorder recorder;
  recorder.attach(&client);
  client.setConfig(clientConfig(server.endpoint()));
  client.send(oneShot());
  QVERIFY2(spinUntil([&] { return recorder.errors >= 1; }, 4000),
           "EOF 尾部坏 JSON 必须失败");
  QCOMPARE(recorder.errorKinds.first(), LlmErrorKind::Malformed);
  QCOMPARE(recorder.finished, 0);
  QCOMPARE(recorder.calls.size(), 0);
}

void TestG1ModelTransport::normalCloseWithoutDoneTolerated() {
  RawServer server;
  QVERIFY(server.start([] {
    // 最后一个事件没有 [DONE] 也没有收尾空行（PW-061 容错范围）。
    return RawServer::Response{
        .writes = {deltaEvent("完整"), deltaEvent("回答"),
                   QByteArrayLiteral(
                       "data: {\"choices\":[{\"delta\":{},"
                       "\"finish_reason\":\"stop\"}]}")},
        .writeDelayMs = 40};
  }));
  LlmClient client;
  Recorder recorder;
  recorder.attach(&client);
  client.setConfig(clientConfig(server.endpoint()));
  client.send(oneShot());
  QVERIFY2(spinUntil([&] { return recorder.finished >= 1; }, 4000),
           qPrintable(QStringLiteral("deltas=%1 errors=%2 msgs=%3")
                        .arg(recorder.deltas.size())
                        .arg(recorder.errors)
                        .arg(recorder.errorMessages.join(QStringLiteral(";")))));
  QCOMPARE(recorder.joined(), QStringLiteral("完整回答"));
  QCOMPARE(recorder.errors, 0);
  QCOMPARE(recorder.finishReason, QStringLiteral("stop")); // finish_reason 透传
}

void TestG1ModelTransport::errorEnvelope200FailsHonestly() {
  RawServer server;
  QVERIFY(server.start([] {
    return RawServer::Response{
        .contentType = "application/json",
        .writes = {"{\"error\":{\"message\":\"quota exceeded\"}}"}};
  }));
  LlmClient client;
  Recorder recorder;
  recorder.attach(&client);
  client.setConfig(clientConfig(server.endpoint()));
  client.send(oneShot());
  QVERIFY2(spinUntil([&] { return recorder.errors >= 1; }, 4000),
           "200 error 信封必须报错");
  QCOMPARE(recorder.errorKinds.first(), LlmErrorKind::Malformed);
  QVERIFY(recorder.errorMessages.first().contains(QStringLiteral("quota exceeded")));
  QCOMPARE(recorder.finished, 0);
  QCOMPARE(recorder.deltas.size(), 0);
}

void TestG1ModelTransport::emptyChoicesNonStreamFails() {
  RawServer server;
  QVERIFY(server.start([] {
    return RawServer::Response{.contentType = "application/json",
                               .writes = {"{\"choices\":[]}"}};
  }));
  LlmClient client;
  Recorder recorder;
  recorder.attach(&client);
  client.setConfig(clientConfig(server.endpoint(), "sk", false));
  client.send(oneShot());
  QVERIFY2(spinUntil([&] { return recorder.errors >= 1; }, 4000), "空 choices 报错");
  QCOMPARE(recorder.errorKinds.first(), LlmErrorKind::Malformed);
  QCOMPARE(recorder.finished, 0);
}

void TestG1ModelTransport::nonObjectMessageFails() {
  RawServer server;
  QVERIFY(server.start([] {
    return RawServer::Response{
        .contentType = "application/json",
        .writes = {"{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\"}]}"}};
  }));
  LlmClient client;
  Recorder recorder;
  recorder.attach(&client);
  client.setConfig(clientConfig(server.endpoint(), "sk", false));
  client.send(oneShot());
  QVERIFY2(spinUntil([&] { return recorder.errors >= 1; }, 4000),
           "非对象 message 报错");
  QCOMPARE(recorder.errorKinds.first(), LlmErrorKind::Malformed);
  QCOMPARE(recorder.finished, 0);
}

void TestG1ModelTransport::finishReasonAndUsagePassThrough() {
  RawServer server;
  QVERIFY(server.start([] {
    const QByteArray tail =
      "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"length\"}],"
      "\"usage\":{\"prompt_tokens\":42,\"completion_tokens\":17}}\n\n"
      "data: [DONE]\n\n";
    return RawServer::Response{.writes = {deltaEvent("答案"), tail}};
  }));
  LlmClient client;
  Recorder recorder;
  recorder.attach(&client);
  client.setConfig(clientConfig(server.endpoint()));
  client.send(oneShot());
  QVERIFY2(spinUntil([&] { return recorder.finished >= 1; }, 4000), "必须完成");
  QCOMPARE(recorder.finishReason, QStringLiteral("length")); // 不改写为 stop
  QCOMPARE(recorder.promptTokens, 42);
  QCOMPARE(recorder.completionTokens, 17);
}

void TestG1ModelTransport::zeroEventStreamFails() {
  RawServer server;
  QVERIFY(server.start([] {
    // 纯心跳后直接关闭：没有任何可交付内容——不是成功。
    return RawServer::Response{.writes = {QByteArrayLiteral(": ping\n\n")}};
  }));
  LlmClient client;
  Recorder recorder;
  recorder.attach(&client);
  client.setConfig(clientConfig(server.endpoint()));
  client.send(oneShot());
  QVERIFY2(spinUntil([&] { return recorder.errors >= 1; }, 4000),
           "零事件流必须报错");
  QCOMPARE(recorder.errorKinds.first(), LlmErrorKind::Malformed);
  QCOMPARE(recorder.finished, 0);
}

void TestG1ModelTransport::reentrantCancelOnErrorKeepsSingleTerminal() {
  int cancels = 0;
  RawServer server;
  QVERIFY(server.start([] { return RawServer::Response{.hold = true}; }));
  LlmClient client;
  Recorder recorder;
  recorder.attach(&client);
  QObject::connect(&client, &LlmClient::errorOccurred, &client,
                   [&](LlmErrorKind, const QString &) {
                     if (cancels++ == 0)
                       client.cancel(); // 错误槽内同步取消
                   });
  client.setConfig(clientConfig(server.endpoint(), "sk", true, 300, 30000));
  client.send(oneShot());
  QVERIFY2(spinUntil([&] { return recorder.errors >= 1; }, 4000), "先超时报错");
  spinUntil([] { return false; }, 200);
  QCOMPARE(recorder.errors, 1); // 单终态：重入取消不再产生第二终态
  QCOMPARE(recorder.finished, 0);
}

// ---- W6 ------------------------------------------------------------------

void TestG1ModelTransport::budgetExactCapSucceeds() {
  // 恰好 8 MiB 的合法单事件流 + [DONE]：在上限内，必须成功。
  FakeNam nam;
  LlmClient client(&nam);
  Recorder recorder;
  recorder.attach(&client);
  client.setConfig(clientConfig(QUrl(QStringLiteral("https://cap.example/v1"))));
  nam.creator = [] { return new FakeReply(nullptr); };
  client.send(oneShot());
  auto *reply = nam.lastReply.data();
  QVERIFY(reply != nullptr);
  reply->setStatusCode(200);
  reply->setContentType("text/event-stream");
  // 构造总字节数恰为 kCap 的 data 事件（content 补齐差值）。
  QByteArray head = "data: {\"choices\":[{\"delta\":{\"content\":\"";
  const QByteArray tail = "\"}}]}\n\n";
  const QByteArray done = "data: [DONE]\n\n";
  const int padding = int(kCap) - head.size() - tail.size() - done.size();
  QVERIFY(padding > 0);
  reply->deliver(head + QByteArray(padding, 'x') + tail + done);
  reply->finishNow();
  QVERIFY2(spinUntil([&] { return recorder.finished >= 1 || recorder.errors >= 1; },
                     8000),
           "恰好上限必须收敛");
  QCOMPARE(recorder.errors, 0);
  QCOMPARE(recorder.finished, 1);
  QCOMPARE(recorder.joined().size(), padding); // 内容原样到达
}

void TestG1ModelTransport::budgetOneByteOverFails() {
  FakeNam nam;
  LlmClient client(&nam);
  Recorder recorder;
  recorder.attach(&client);
  client.setConfig(clientConfig(QUrl(QStringLiteral("https://cap.example/v1"))));
  nam.creator = [] { return new FakeReply(nullptr); };
  client.send(oneShot());
  auto *reply = nam.lastReply.data();
  QVERIFY(reply != nullptr);
  reply->setStatusCode(200);
  reply->setContentType("text/event-stream");
  QByteArray head = "data: {\"choices\":[{\"delta\":{\"content\":\"";
  const QByteArray tail = "\"}}]}\n\n";
  const int padding = int(kCap) - head.size() - tail.size() + 1; // 超一字节
  reply->deliver(head + QByteArray(padding, 'x') + tail);
  reply->finishNow();
  QVERIFY2(spinUntil([&] { return recorder.errors >= 1; }, 8000), "越限必须报错");
  QCOMPARE(recorder.errorKinds.first(), LlmErrorKind::TooLarge);
  QCOMPARE(recorder.finished, 0);
}

void TestG1ModelTransport::budgetTailOnlyOverflowFails() {
  // readyRead 阶段在上限内；越限字节只出现在 finished 时刻的 tail——同一把
  // 尺子必须拦下（经生产 finished 分支）。
  FakeNam nam;
  LlmClient client(&nam);
  Recorder recorder;
  recorder.attach(&client);
  client.setConfig(clientConfig(QUrl(QStringLiteral("https://cap.example/v1"))));
  nam.creator = [] { return new FakeReply(nullptr); };
  client.send(oneShot());
  auto *reply = nam.lastReply.data();
  QVERIFY(reply != nullptr);
  reply->setStatusCode(200);
  reply->setContentType("text/event-stream");
  const QByteArray filler(int(kCap) - 10, 'x'); // 无换行：不触发事件解析
  reply->deliver("data: " + filler);
  reply->queueTail(QByteArray(30, 'y')); // 只进缓冲：finished 才读得到
  reply->finishNow();
  QVERIFY2(spinUntil([&] { return recorder.errors >= 1; }, 8000),
           "tail 越限必须有界失败");
  QCOMPARE(recorder.errorKinds.first(), LlmErrorKind::TooLarge);
  QCOMPARE(recorder.finished, 0);
}

void TestG1ModelTransport::budgetUnclosedLineHitsCap() {
  // 单个未闭合 data 行（无换行输入）一次性送达超限体积后关连接：真实 socket
  // 路径也必须撞上限（字节计数不看内容形状；readyRead 与 tail 任意分布）。
  RawServer server;
  QVERIFY(server.start([] {
    return RawServer::Response{
        .writes = {QByteArray("data: ") + QByteArray(int(kCap) + 50, 'z')}};
  }));
  LlmClient client;
  Recorder recorder;
  recorder.attach(&client);
  client.setConfig(clientConfig(server.endpoint(), "sk", true, 30000, 30000));
  client.send(oneShot());
  QVERIFY2(spinUntil([&] { return recorder.errors >= 1; }, 30000),
           qPrintable(QStringLiteral("无换行增长流必须由字节上限终结"
                                     "（server 写出 %1 字节）")
                        .arg(server.bytesWritten())));
  QCOMPARE(recorder.errorKinds.first(), LlmErrorKind::TooLarge);
  QCOMPARE(recorder.finished, 0);
  QCOMPARE(recorder.deltas.size(), 0); // 没有完整事件，零内容派发
}

// ---- W8 ------------------------------------------------------------------

void TestG1ModelTransport::diagnosticsRedactFrozenAttemptKey() {
  // 请求用 K1 发出；响应前配置切到 K2；服务端错误信封回显 K1——脱敏必须
  // 按 attempt 冻结的 K1 判（只检查新 K2 会漏掉旧响应的回显）。
  const QByteArray oldKey = "sk-old-attempt-key";
  const QByteArray newKey = "sk-new-config-key";
  RawServer server;
  QVERIFY(server.start([&] {
    return RawServer::Response{
        .contentType = "application/json",
        .writes = {"{\"error\":{\"message\":\"invalid key " +
                   QByteArray(oldKey) + "\"}}"}};
  }));
  LlmClient client;
  Recorder recorder;
  recorder.attach(&client);
  client.setConfig(clientConfig(server.endpoint(), oldKey));
  client.send(oneShot());
  client.setConfig(clientConfig(server.endpoint(), newKey)); // 在途切换
  QVERIFY2(spinUntil([&] { return recorder.errors >= 1; }, 4000), "信封必须报错");
  QCOMPARE(recorder.errorKinds.first(), LlmErrorKind::Malformed);
  QVERIFY2(!recorder.errorMessages.first().contains(QLatin1String(oldKey)),
           "旧密钥回显必须脱敏");
  QVERIFY(recorder.errorMessages.first().contains(QStringLiteral("[已隐藏]")));
  QVERIFY(recorder.errorMessages.first().contains(QStringLiteral("invalid key")));
}

int main(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);
  TestG1ModelTransport tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_g1_modeltransport.moc"
