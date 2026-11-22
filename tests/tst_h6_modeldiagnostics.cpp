// 层：测试壳
#include <QtTest>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <functional>

#include "../src/workflow/llmconnectiondiagnostics.h"
#include "../src/workflow/llmdiagnosticcontract.h"
#include "../src/workflow/llmdiagnosticreport.h"
#include "../src/workflow/llmmodelinventory.h"

// H6（模型接入诊断）服务级回归：直连生产 LlmConnectionDiagnostics /
// LlmModelInventory / 报告渲染（QTcpServer 假服务器，无外部账号）。
//   · W1 本地规则：结构/凭据/模型缺失本地拒绝零网络；端点身份与凭据来源
//     判定（显式 > 清除 > 身份一致沿用 > 无）；
//   · W2 生命周期：显式启动、单飞、取消单终态、迟到结果不覆盖新候选；
//   · W3 目录：可缺席（404/405≠文本不可用）、形状/空目录/条目界限/凭据
//     回显剔除/字节预算/不跟随重定向；
//   · W4 文本与流式：真实 POST（stream/model/max_tokens/Authorization 对
//     账）、stream=true 普通 JSON 兼容成功、真实 SSE 场景结论诚实；
//   · W5 工具往返：严格帧匹配/多帧/错 nonce/未知名/未调用/恰好两请求/
//     原样回放/零第三轮/零业务副作用；
//   · W6 报告：分项独立、真实 SSE 恒标未独立确认、usage 服务端报告口径。
// 断言纪律：不断言墙钟时长；请求次数/方法/路径/头/体按原始字节对账。

using namespace paleo::chat;

namespace {

constexpr const char kKey[] = "sk-test-key-123456";

bool spinUntil(const std::function<bool()> &predicate, int timeoutMs = 8000) {
  QElapsedTimer clock;
  clock.start();
  while (!predicate() && clock.elapsed() < timeoutMs)
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  return predicate();
}

// 脚本化本地 HTTP 服务器：每请求按脚本回（可挂起），记录原始请求。
class ScriptServer : public QObject {
  Q_OBJECT
public:
  struct Request {
    QByteArray method;
    QByteArray path;
    QByteArray auth;
    QByteArray body;
  };
  struct Response {
    int status = 200;
    QByteArray contentType = "application/json";
    QByteArray extraHeaders;
    QList<QByteArray> writes;
    int writeDelayMs = 15;
    bool closeAfter = true;
    bool hold = false;
  };
  using Handler = std::function<Response(const Request &)>;

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
  int requestCount() const { return m_requests.size(); }
  const QList<Request> &requests() const { return m_requests; }

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
      const QList<QByteArray> lines = buffer->left(headerEnd).split('\n');
      Request request;
      for (const QByteArray &line : lines) {
        if (line.toLower().startsWith("content-length:"))
          length = line.mid(15).trimmed().toLongLong();
        else if (line.toLower().startsWith("authorization:"))
          request.auth = line.mid(14).trimmed();
      }
      if (buffer->size() < headerEnd + 4 + length)
        return;
      const QByteArray firstLine = lines.first().trimmed();
      const QList<QByteArray> parts = firstLine.split(' ');
      request.method = parts.value(0);
      request.path = parts.value(1);
      request.body = buffer->mid(headerEnd + 4, length);
      m_requests.append(request);
      respond(sock, m_handler(request));
      *buffer = buffer->mid(headerEnd + 4 + length);
    });
  }
  void respond(QTcpSocket *sock, const Response &response) {
    if (response.hold)
      return; // 挂起不答：逼取消/超时路径
    QByteArray head =
      "HTTP/1.1 " + QByteArray::number(response.status) + " OK\r\n"
      "Connection: close\r\nContent-Type: " + response.contentType + "\r\n" +
      response.extraHeaders + "\r\n";
    sock->write(head);
    const QList<QByteArray> writes = response.writes;
    const int delay = response.writeDelayMs;
    for (int i = 0; i < writes.size(); ++i)
      QTimer::singleShot(delay * (i + 1), sock, [sock, bytes = writes.at(i)] {
        sock->write(bytes);
      });
    if (response.closeAfter)
      QTimer::singleShot(delay * (writes.size() + 1), sock,
                         [sock] { sock->disconnectFromHost(); });
  }

  Handler m_handler;
  QTcpServer m_server;
  QList<Request> m_requests;
};

QByteArray sse(const QByteArray &json, bool done = false) {
  QByteArray out = "data: " + json + "\n\n";
  if (done)
    out += "data: [DONE]\n\n";
  return out;
}

// 非流式 chat completions 应答体（content + finish_reason + usage）。
QByteArray completionBody(const QString &content, const char *reason = "stop") {
  QJsonObject choice;
  choice.insert("index", 0);
  choice.insert("finish_reason", reason);
  choice.insert("message", QJsonObject{{"content", content}});
  QJsonObject body;
  body.insert("choices", QJsonArray{choice});
  body.insert("usage", QJsonObject{{"prompt_tokens", 3}, {"completion_tokens", 5}});
  return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

// 非流式 chat completions 工具调用应答体（message.tool_calls 形状）。
QByteArray toolCallJsonBody(const QString &id, const QString &name,
                            const QString &argsJson) {
  QJsonObject call;
  call.insert("id", id);
  call.insert("type", QStringLiteral("function"));
  call.insert("function",
              QJsonObject{{"name", name}, {"arguments", argsJson}});
  QJsonObject message;
  message.insert("content", QJsonValue());
  message.insert("tool_calls", QJsonArray{call});
  QJsonObject choice;
  choice.insert("index", 0);
  choice.insert("finish_reason", QStringLiteral("tool_calls"));
  choice.insert("message", message);
  QJsonObject body;
  body.insert("choices", QJsonArray{choice});
  body.insert("usage", QJsonObject{{"prompt_tokens", 3}, {"completion_tokens", 5}});
  return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QJsonDocument bodyJson(const QByteArray &raw) {
  return QJsonDocument::fromJson(raw);
}

// 流式工具帧（单事件：完整调用帧）。
QByteArray toolFrameSse(const QString &id, const QString &name,
                        const QString &argsJson, bool done, int index = 0) {
  QJsonObject call;
  call.insert("index", index);
  call.insert("id", id);
  call.insert("type", QStringLiteral("function"));
  call.insert("function",
              QJsonObject{{"name", name}, {"arguments", argsJson}});
  QJsonObject delta;
  delta.insert("tool_calls", QJsonArray{call});
  QJsonObject choice;
  choice.insert("index", 0);
  choice.insert("delta", delta);
  QJsonObject event;
  event.insert("choices", QJsonArray{choice});
  return sse(QJsonDocument(event).toJson(QJsonDocument::Compact), done);
}

LlmDiagnosticCandidate candidateFor(const QUrl &endpoint, bool stream = false) {
  LlmDiagnosticCandidate candidate;
  candidate.endpoint = endpoint;
  candidate.model = QStringLiteral("geo-7b");
  candidate.stream = stream;
  candidate.timeoutMs = 3000;
  candidate.totalTimeoutMs = 5000;
  candidate.maxTokens = 1024;
  candidate.allowInsecureHttp = false;
  candidate.key = QByteArray(kKey);
  candidate.keySource = LlmDiagnosticCandidate::KeySource::ExplicitInput;
  return candidate;
}

// 显式运行一项动作并等待单次终态（actionFinished 或本地拒绝）。
bool runAndWait(LlmConnectionDiagnostics &diag, LlmDiagnosticsAction action,
                const LlmDiagnosticCandidate &candidate) {
  QSignalSpy finished(&diag, &LlmConnectionDiagnostics::actionFinished);
  QSignalSpy rejected(&diag, &LlmConnectionDiagnostics::candidateRejected);
  diag.runAction(action, candidate);
  spinUntil([&] { return finished.count() + rejected.count() >= 1; });
  if (rejected.count() >= 1)
    qWarning("runAndWait rejected: %s",
             qPrintable(rejected.at(0).at(0).toString()));
  return rejected.count() == 0;
}

} // namespace

class TestH6ModelDiagnostics : public QObject {
  Q_OBJECT
private slots:
  void endpointIdentityAndKeySourceRules();
  void probeRequiresModelLocalReject();
  void missingKeyLocalRejectZeroHttp();
  void busySecondClickLocalRejected();
  void catalogUnsupportedTextStillWorks();
  void catalogListedModelCompletionRejected();
  void catalogEmptyAndUnknownShape();
  void credentialEchoFilteredFromCatalog();
  void catalogByteBudgetExceeded();
  void catalogTimeoutDirectInventory();
  void textProbeRequestShape();
  void streamPlainJsonCompatibleAccepted();
  void realSseStreamObservedHonestSse();
  void non2xxFakeSuccessBodyNotPassed();
  void toolNoCallIndeterminate();
  void toolWrongNonceFails();
  void toolUnknownNameFails();
  void toolMultiCallFails();
  void toolExactMatchRoundTrip();
  void toolSecondRequestToolCallNoThirdRound();
  void cancelDuringFirstRequestZeroFollowUp();
  void lateResultDroppedAfterNewCandidate();
  void redirectNotFollowedSecondEndpointZeroAuth();
  void destroyDuringProbeSafeShutdown();
  void catalogAllFilteredIsEmptyWithReason();
};

void TestH6ModelDiagnostics::endpointIdentityAndKeySourceRules() {
  // 端点身份：scheme/host 大小写、尾斜杠不计；base path/端口差异必须不等。
  QVERIFY(llmEndpointIdentityMatches(QUrl(QStringLiteral("https://a.com/v1")),
                                     QUrl(QStringLiteral("HTTPS://A.COM/v1/"))));
  QVERIFY(!llmEndpointIdentityMatches(QUrl(QStringLiteral("https://a.com/v1")),
                                      QUrl(QStringLiteral("https://a.com/v2"))));
  QVERIFY(!llmEndpointIdentityMatches(QUrl(QStringLiteral("https://a.com/v1")),
                                      QUrl(QStringLiteral("https://b.com/v1"))));
  QVERIFY(!llmEndpointIdentityMatches(QUrl(QStringLiteral("https://a.com/v1")),
                                      QUrl(QStringLiteral("https://a.com:8443/v1"))));
  // 凭据来源：显式 > 清除（不复用）> 身份一致沿用 > 端点变更无凭据。
  const QUrl current(QStringLiteral("https://a.com/v1"));
  using Source = LlmDiagnosticCandidate::KeySource;
  QCOMPARE(llmResolveDiagnosticKeySource(true, false, QUrl(QStringLiteral("https://x.com/v9")),
                                         current, true),
           Source::ExplicitInput);
  QCOMPARE(llmResolveDiagnosticKeySource(false, true, current, current, true),
           Source::None);
  QCOMPARE(llmResolveDiagnosticKeySource(false, false, current, current, true),
           Source::InMemoryCurrent);
  QCOMPARE(llmResolveDiagnosticKeySource(false, false,
                                         QUrl(QStringLiteral("https://a.com/v2")),
                                         current, true),
           Source::None);
  QCOMPARE(llmResolveDiagnosticKeySource(false, false, current, current, false),
           Source::None);
}

void TestH6ModelDiagnostics::probeRequiresModelLocalReject() {
  LlmConnectionDiagnostics diag;
  LlmDiagnosticCandidate candidate = candidateFor(QUrl(QStringLiteral("http://127.0.0.1:1/v1")));
  candidate.model.clear();
  QSignalSpy rejected(&diag, &LlmConnectionDiagnostics::candidateRejected);
  diag.runAction(LlmDiagnosticsAction::Text, candidate);
  QCOMPARE(rejected.count(), 1); // 本地拒绝（零网络——无服务器可连）
}

void TestH6ModelDiagnostics::missingKeyLocalRejectZeroHttp() {
  ScriptServer server;
  QVERIFY(server.start([](const ScriptServer::Request &) {
    return ScriptServer::Response{};
  }));
  LlmConnectionDiagnostics diag;
  LlmDiagnosticCandidate candidate = candidateFor(server.endpoint());
  candidate.key.clear();
  candidate.keySource = LlmDiagnosticCandidate::KeySource::None;
  QVERIFY(!runAndWait(diag, LlmDiagnosticsAction::Catalog, candidate));
  QCOMPARE(server.requestCount(), 0); // 零网络：凭据规则本地拒绝
}

void TestH6ModelDiagnostics::busySecondClickLocalRejected() {
  ScriptServer server;
  QVERIFY(server.start([](const ScriptServer::Request &) {
    ScriptServer::Response hold;
    hold.hold = true;
    return hold;
  }));
  LlmConnectionDiagnostics diag;
  diag.runAction(LlmDiagnosticsAction::Stream, candidateFor(server.endpoint(), true));
  spinUntil([&] { return diag.busy() && server.requestCount() >= 1; });
  QSignalSpy rejected(&diag, &LlmConnectionDiagnostics::candidateRejected);
  diag.runAction(LlmDiagnosticsAction::Text, candidateFor(server.endpoint()));
  QCOMPARE(rejected.count(), 1); // 在途再启动 = 该次点击的单次终态
  QCOMPARE(server.requestCount(), 1); // 零额外网络
  diag.cancel();
}

void TestH6ModelDiagnostics::catalogUnsupportedTextStillWorks() {
  ScriptServer server;
  QVERIFY(server.start([](const ScriptServer::Request &request) {
    if (request.method == "GET")
      return ScriptServer::Response{404, "application/json", {}, {}, 0, true, false};
    return ScriptServer::Response{200, "application/json",
                                  {}, {completionBody(QStringLiteral("诊断正常"))}, 0,
                                  true, false};
  }));
  LlmConnectionDiagnostics diag;
  QVERIFY(runAndWait(diag, LlmDiagnosticsAction::Catalog,
                     candidateFor(server.endpoint())));
  QCOMPARE(diag.report().catalog.status, LlmDiagnosticStatus::Unsupported);
  QVERIFY(runAndWait(diag, LlmDiagnosticsAction::Text,
                     candidateFor(server.endpoint())));
  const LlmDiagnosticReport report = diag.report();
  QCOMPARE(report.text.status, LlmDiagnosticStatus::Passed);
  QCOMPARE(report.text.httpRequests, 1);
  QVERIFY(report.text.usageReported);
  // 目录缺席不把文本标红/不自动换路：请求面 = 1 GET + 1 POST。
  QCOMPARE(server.requestCount(), 2);
  QCOMPARE(server.requests().at(0).method, "GET");
  QCOMPARE(server.requests().at(0).path, "/v1/models");
  QCOMPARE(server.requests().at(1).method, "POST");
  QCOMPARE(server.requests().at(1).path, "/v1/chat/completions");
}

void TestH6ModelDiagnostics::catalogListedModelCompletionRejected() {
  ScriptServer server;
  QVERIFY(server.start([](const ScriptServer::Request &request) {
    if (request.method == "GET") {
      const QJsonObject catalog{
        {"data", QJsonArray{QJsonObject{{"id", QStringLiteral("geo-7b")}}}}};
      return ScriptServer::Response{
        200, "application/json", {},
        {QJsonDocument(catalog).toJson(QJsonDocument::Compact)}};
    }
    return ScriptServer::Response{401, "application/json", {}, {R"({"error":{"message":"bad key"}})"}, 0, true, false};
  }));
  LlmConnectionDiagnostics diag;
  QSignalSpy models(&diag, &LlmConnectionDiagnostics::modelCandidatesChanged);
  QVERIFY(runAndWait(diag, LlmDiagnosticsAction::Catalog,
                     candidateFor(server.endpoint())));
  QCOMPARE(diag.report().catalog.status, LlmDiagnosticStatus::Passed);
  QCOMPARE(diag.report().catalog.models, QStringList{QStringLiteral("geo-7b")});
  QCOMPARE(models.count(), 1);
  // 目录存在 ≠ 模型可用：用目录模型做补全 → 401 按实际阶段分类
  // （网络终态完成，非本地拒绝）。
  QVERIFY(runAndWait(diag, LlmDiagnosticsAction::Text,
                     candidateFor(server.endpoint())));
  QCOMPARE(diag.report().text.status, LlmDiagnosticStatus::Unauthorized);
}

void TestH6ModelDiagnostics::catalogEmptyAndUnknownShape() {
  ScriptServer server;
  QVERIFY(server.start([&server](const ScriptServer::Request &request) {
    if (request.path == "/v1/models") {
      if (server.requestCount() == 1)
        return ScriptServer::Response{200, "application/json", {},
                                      {QByteArrayLiteral(R"({"data": []})")}};
      return ScriptServer::Response{200, "application/json", {},
                                    {QByteArrayLiteral(R"({"models": ["x"]})")}};
    }
    return ScriptServer::Response{};
  }));
  LlmConnectionDiagnostics diag;
  QVERIFY(runAndWait(diag, LlmDiagnosticsAction::Catalog,
                     candidateFor(server.endpoint())));
  QCOMPARE(diag.report().catalog.status, LlmDiagnosticStatus::Empty);
  QVERIFY(runAndWait(diag, LlmDiagnosticsAction::Catalog,
                     candidateFor(server.endpoint())));
  QCOMPARE(diag.report().catalog.status, LlmDiagnosticStatus::UnknownShape);
  // 目录未知形状不改写文本能力（只观测、不红标）。
  QCOMPARE(diag.report().text.status, LlmDiagnosticStatus::Untested);
}

void TestH6ModelDiagnostics::credentialEchoFilteredFromCatalog() {
  ScriptServer server;
  QVERIFY(server.start([](const ScriptServer::Request &) {
    const QJsonObject catalog{
      {"data", QJsonArray{QJsonObject{{"id", QStringLiteral("good-model")}},
                          QJsonObject{{"id", QLatin1String(kKey)}}}}};
    return ScriptServer::Response{200, "application/json", {},
                                  {QJsonDocument(catalog).toJson(QJsonDocument::Compact)}};
  }));
  LlmConnectionDiagnostics diag;
  QVERIFY(runAndWait(diag, LlmDiagnosticsAction::Catalog,
                     candidateFor(server.endpoint())));
  const LlmCatalogObservation catalog = diag.report().catalog;
  QCOMPARE(catalog.status, LlmDiagnosticStatus::Passed);
  QCOMPARE(catalog.models, QStringList{QStringLiteral("good-model")});
  QVERIFY(catalog.credentialFiltered); // 回显条目被剔除，不进 widget/报告
}

void TestH6ModelDiagnostics::catalogByteBudgetExceeded() {
  ScriptServer server;
  QVERIFY(server.start([](const ScriptServer::Request &) {
    QByteArray huge(2ll * 1024 * 1024 + 1024, 'x');
    return ScriptServer::Response{200, "application/json", {}, {huge}};
  }));
  LlmConnectionDiagnostics diag;
  QVERIFY(runAndWait(diag, LlmDiagnosticsAction::Catalog,
                     candidateFor(server.endpoint())));
  QCOMPARE(diag.report().catalog.status, LlmDiagnosticStatus::TooLarge);
  QCOMPARE(server.requestCount(), 1); // 零自动重试
}

void TestH6ModelDiagnostics::catalogTimeoutDirectInventory() {
  // 直接驱动生产 LlmModelInventory（controller 用同一对象/同一默认时限；
  // 这里用短时限夹具避免慢测）。
  ScriptServer server;
  QVERIFY(server.start([](const ScriptServer::Request &) {
    ScriptServer::Response hold;
    hold.hold = true;
    return hold;
  }));
  LlmModelInventory::Limits limits;
  limits.idleTimeoutMs = 200;
  limits.totalTimeoutMs = 800;
  LlmModelInventory inventory(limits);
  LlmDiagnosticCandidate candidate = candidateFor(server.endpoint());
  QSignalSpy finished(&inventory, &LlmModelInventory::finished);
  inventory.probe(candidate);
  QVERIFY(spinUntil([&] { return finished.count() == 1; }, 4000));
  QCOMPARE(finished.at(0).at(0).value<LlmCatalogObservation>().status,
           LlmDiagnosticStatus::Timeout);
  QCOMPARE(server.requestCount(), 1); // 零自动重试
}

void TestH6ModelDiagnostics::textProbeRequestShape() {
  ScriptServer server;
  QVERIFY(server.start([](const ScriptServer::Request &) {
    return ScriptServer::Response{200, "application/json", {},
                                  {completionBody(QStringLiteral("诊断正常"))}};
  }));
  LlmConnectionDiagnostics diag;
  QVERIFY(runAndWait(diag, LlmDiagnosticsAction::Text,
                     candidateFor(server.endpoint())));
  QCOMPARE(diag.report().text.status, LlmDiagnosticStatus::Passed);
  QCOMPARE(diag.report().text.finishReason, QStringLiteral("stop"));
  // 真实请求体对账：合成 messages、stream=false、探测上限 ≤候选值、唯一鉴权头。
  QCOMPARE(diag.report().generation, 1); // 显式启动一代次（报告头可见）
  const ScriptServer::Request request = server.requests().first();
  const QJsonObject body = bodyJson(request.body).object();
  QCOMPARE(request.auth, QByteArray("Bearer ") + QByteArray(kKey));
  QCOMPARE(body.value("model").toString(), QStringLiteral("geo-7b"));
  QCOMPARE(body.value("stream").toBool(), false);
  QCOMPARE(body.value("max_tokens").toInt(), 64); // min(64, 1024)
  const QJsonArray messages = body.value("messages").toArray();
  QCOMPARE(messages.size(), 1);
  QCOMPARE(messages.at(0).toObject().value("role").toString(), QStringLiteral("user"));
  QVERIFY(!body.contains("tools")); // 零真实工具定义上送
  QCOMPARE(diag.report().probeMaxTokens, 64);
  QCOMPARE(diag.report().candidateMaxTokens, 1024);
}

void TestH6ModelDiagnostics::streamPlainJsonCompatibleAccepted() {
  ScriptServer server;
  QVERIFY(server.start([](const ScriptServer::Request &) {
    // stream=true 却回普通 JSON（application/json）：G1 兼容层成功口径。
    return ScriptServer::Response{200, "application/json", {},
                                  {completionBody(QStringLiteral("诊断正常"))}};
  }));
  LlmConnectionDiagnostics diag;
  QVERIFY(runAndWait(diag, LlmDiagnosticsAction::Stream,
                     candidateFor(server.endpoint(), true)));
  const LlmDiagnosticReport report = diag.report();
  QCOMPARE(report.stream.status, LlmDiagnosticStatus::Passed);
  QCOMPARE(report.stream.httpRequests, 1);
  QCOMPARE(server.requestCount(), 1); // 零自动重试/换模型
  // 诚实口径：结论=「被当前客户端接受」，不得宣称已证明真实 SSE。
  QVERIFY(report.stream.detail.contains(QStringLiteral("未独立确认")));
  QVERIFY(!report.stream.detail.contains(QStringLiteral("已确认")));
}

void TestH6ModelDiagnostics::realSseStreamObservedHonestSse() {
  ScriptServer server;
  QVERIFY(server.start([](const ScriptServer::Request &) {
    const QByteArray chunk1 = sse(
      QJsonDocument(QJsonObject{
        {"choices", QJsonArray{QJsonObject{
                      {"index", 0},
                      {"delta", QJsonObject{{"content", QStringLiteral("诊断")}}}}}}})
        .toJson(QJsonDocument::Compact));
    const QByteArray chunk2 = sse(
      QJsonDocument(QJsonObject{
        {"choices", QJsonArray{QJsonObject{
                      {"index", 0},
                      {"delta", QJsonObject{{"content", QStringLiteral("正常")}}}}}}})
        .toJson(QJsonDocument::Compact),
        true);
    return ScriptServer::Response{200, "text/event-stream", {}, {chunk1, chunk2}};
  }));
  LlmConnectionDiagnostics diag;
  QVERIFY(runAndWait(diag, LlmDiagnosticsAction::Stream,
                     candidateFor(server.endpoint(), true)));
  QCOMPARE(diag.report().stream.status, LlmDiagnosticStatus::Passed);
  QVERIFY(diag.report().stream.deltaCount >= 2);
  // 即使真见过 SSE 事件，公开信号口径仍不宣称「已证明真实 SSE」。
  QVERIFY(diag.report().stream.detail.contains(QStringLiteral("未独立确认")));
}

void TestH6ModelDiagnostics::non2xxFakeSuccessBodyNotPassed() {
  ScriptServer server;
  QVERIFY(server.start([](const ScriptServer::Request &) {
    // 非 2xx 带伪成功体：HTTP 分类优先，不得把伪成功体当通过。
    return ScriptServer::Response{401, "application/json", {},
                                  {completionBody(QStringLiteral("伪成功"))}};
  }));
  LlmConnectionDiagnostics diag;
  QVERIFY(runAndWait(diag, LlmDiagnosticsAction::Text,
                     candidateFor(server.endpoint())));
  QCOMPARE(diag.report().text.status, LlmDiagnosticStatus::Unauthorized);
  QCOMPARE(server.requestCount(), 1);
}

void TestH6ModelDiagnostics::toolNoCallIndeterminate() {
  ScriptServer server;
  QVERIFY(server.start([](const ScriptServer::Request &) {
    return ScriptServer::Response{200, "application/json", {},
                                  {completionBody(QStringLiteral("直接作答"))}};
  }));
  LlmConnectionDiagnostics diag;
  QVERIFY(runAndWait(diag, LlmDiagnosticsAction::Tool,
                     candidateFor(server.endpoint())));
  const LlmProbeObservation tool = diag.report().toolRoundTrip;
  QCOMPARE(tool.status, LlmDiagnosticStatus::Indeterminate);
  QVERIFY(!tool.toolFrameObserved);
  QCOMPARE(tool.httpRequests, 1); // 零第二请求
  // 请求面：tools 上送 = 唯一诊断 echo，tool_choice=auto。
  const QJsonObject body = bodyJson(server.requests().first().body).object();
  const QJsonArray tools = body.value("tools").toArray();
  QCOMPARE(tools.size(), 1);
  QCOMPARE(tools.at(0).toObject().value("function").toObject().value("name").toString(),
           llmDiagnosticEchoToolName());
  QCOMPARE(body.value("tool_choice").toString(), QStringLiteral("auto"));
}

void TestH6ModelDiagnostics::toolWrongNonceFails() {
  ScriptServer server;
  QVERIFY(server.start([&server](const ScriptServer::Request &) {
    if (server.requestCount() == 1)
      return ScriptServer::Response{
        200, "text/event-stream", {},
        {toolFrameSse(QStringLiteral("call-1"), llmDiagnosticEchoToolName(),
                      QStringLiteral(R"({"nonce":"wrong"})"), true)}};
    return ScriptServer::Response{200, "application/json", {},
                                  {completionBody(QStringLiteral("不应到达"))}};
  }));
  LlmConnectionDiagnostics diag;
  QVERIFY(runAndWait(diag, LlmDiagnosticsAction::Tool,
                     candidateFor(server.endpoint(), true))); // SSE 流式帧
  QCOMPARE(diag.report().toolRoundTrip.status, LlmDiagnosticStatus::Malformed);
  QCOMPARE(diag.report().toolRoundTrip.httpRequests, 1); // 错实参零后续
  QVERIFY(!diag.report().toolRoundTrip.toolFrameObserved);
}

void TestH6ModelDiagnostics::toolUnknownNameFails() {
  ScriptServer server;
  QVERIFY(server.start([](const ScriptServer::Request &) {
    return ScriptServer::Response{
      200, "text/event-stream", {},
      {toolFrameSse(QStringLiteral("call-1"), QStringLiteral("paleo.query_project"),
                    QStringLiteral(R"({"nonce":"abc"})"), true)}};
  }));
  LlmConnectionDiagnostics diag;
  QVERIFY(runAndWait(diag, LlmDiagnosticsAction::Tool,
                     candidateFor(server.endpoint(), true)));
  QCOMPARE(diag.report().toolRoundTrip.status, LlmDiagnosticStatus::Malformed);
  QCOMPARE(diag.report().toolRoundTrip.httpRequests, 1);
  QVERIFY(!diag.report().toolRoundTrip.toolFrameObserved);
}

void TestH6ModelDiagnostics::toolMultiCallFails() {
  ScriptServer server;
  QVERIFY(server.start([](const ScriptServer::Request &request) {
    const QString nonce =
      QRegularExpression(QStringLiteral("[0-9a-f]{24}"))
        .match(QString::fromUtf8(request.body))
        .captured(0);
    return ScriptServer::Response{
      200, "text/event-stream", {},
      {toolFrameSse(QStringLiteral("call-1"), llmDiagnosticEchoToolName(),
                    QStringLiteral(R"({"nonce":"%1"})").arg(nonce), false, 0),
       toolFrameSse(QStringLiteral("call-2"), llmDiagnosticEchoToolName(),
                    QStringLiteral(R"({"nonce":"%1"})").arg(nonce), true, 1)}};
  }));
  LlmConnectionDiagnostics diag;
  QVERIFY(runAndWait(diag, LlmDiagnosticsAction::Tool,
                     candidateFor(server.endpoint(), true)));
  QCOMPARE(diag.report().toolRoundTrip.status, LlmDiagnosticStatus::Malformed);
  QCOMPARE(diag.report().toolRoundTrip.httpRequests, 1); // 多帧零后续
}

void TestH6ModelDiagnostics::toolExactMatchRoundTrip() {
  // 首请求：SSE 工具帧（nonce 从请求体提取原样回显）+ [DONE]；
  // 次请求：纯文本完成。全程恰好两次 POST。
  ScriptServer server;
  QVERIFY(server.start([&server](const ScriptServer::Request &request) {
    if (server.requestCount() == 1) {
      const QRegularExpression pattern(QStringLiteral("[0-9a-f]{24}"));
      const QString nonce =
        pattern.match(QString::fromUtf8(request.body)).captured(0);
      return ScriptServer::Response{
        200, "text/event-stream", {},
        {toolFrameSse(QStringLiteral("call-diag-1"), llmDiagnosticEchoToolName(),
                      QStringLiteral(R"({"nonce":"%1"})").arg(nonce), true)}};
    }
    return ScriptServer::Response{200, "application/json", {},
                                  {completionBody(QStringLiteral("后续文本"))}};
  }));
  LlmConnectionDiagnostics diag;
  QVERIFY(runAndWait(diag, LlmDiagnosticsAction::Tool,
                     candidateFor(server.endpoint(), true)));
  const LlmProbeObservation tool = diag.report().toolRoundTrip;
  QCOMPARE(tool.status, LlmDiagnosticStatus::Passed);
  QVERIFY(tool.toolFrameObserved);
  QVERIFY(tool.toolResultSentBack);
  QVERIFY(tool.followUpTextCompleted);
  QCOMPARE(tool.httpRequests, 2);
  QCOMPARE(server.requestCount(), 2);
  // 第二请求协议对账：原样帧回放（assistant.tool_calls）+ role=tool 按
  // 原 call ID 配对 + 固定内存 echo 结果 + 无 tools 上送。
  const QJsonObject second = bodyJson(server.requests().at(1).body).object();
  QVERIFY(!second.contains("tools"));
  const QJsonArray messages = second.value("messages").toArray();
  QCOMPARE(messages.size(), 3);
  QCOMPARE(messages.at(1).toObject().value("role").toString(), QStringLiteral("assistant"));
  const QJsonArray calls = messages.at(1).toObject().value("tool_calls").toArray();
  QCOMPARE(calls.size(), 1);
  const QString callId = calls.at(0).toObject().value("id").toString();
  QCOMPARE(callId, QStringLiteral("call-diag-1"));
  QCOMPARE(calls.at(0).toObject().value("function").toObject().value("arguments").toString(),
           QStringLiteral(R"({"nonce":"%1"})")
             .arg(QRegularExpression(QStringLiteral("[0-9a-f]{24}"))
                    .match(QString::fromUtf8(server.requests().at(0).body))
                    .captured(0)));
  QCOMPARE(messages.at(2).toObject().value("role").toString(), QStringLiteral("tool"));
  QCOMPARE(messages.at(2).toObject().value("tool_call_id").toString(), callId);
  const QString nonce =
    QRegularExpression(QStringLiteral("[0-9a-f]{24}"))
      .match(QString::fromUtf8(server.requests().at(0).body))
      .captured(0);
  QCOMPARE(messages.at(2).toObject().value("content").toString(),
           QStringLiteral(R"({"echo":"%1"})").arg(nonce));
  // 零业务副作用：无会话/操作存储可写面（诊断对象私有内存，见架构断言）。
  QCOMPARE(tool.secondToolCallSeen, false);
}

void TestH6ModelDiagnostics::toolSecondRequestToolCallNoThirdRound() {
  ScriptServer server;
  QVERIFY(server.start([&server](const ScriptServer::Request &request) {
    const QRegularExpression pattern(QStringLiteral("[0-9a-f]{24}"));
    const QString nonce = pattern.match(QString::fromUtf8(request.body)).captured(0);
    if (server.requestCount() == 1)
      return ScriptServer::Response{
        200, "text/event-stream", {},
        {toolFrameSse(QStringLiteral("call-x"), llmDiagnosticEchoToolName(),
                      QStringLiteral(R"({"nonce":"%1"})").arg(nonce), true)}};
    // 后续请求又要求工具：不触发第三轮。
    return ScriptServer::Response{
      200, "text/event-stream", {},
      {toolFrameSse(QStringLiteral("call-y"), llmDiagnosticEchoToolName(),
                    QStringLiteral(R"({"nonce":"%1"})").arg(nonce), true)}};
  }));
  LlmConnectionDiagnostics diag;
  QVERIFY(runAndWait(diag, LlmDiagnosticsAction::Tool,
                     candidateFor(server.endpoint(), true)));
  QCOMPARE(diag.report().toolRoundTrip.status, LlmDiagnosticStatus::Indeterminate);
  QVERIFY(diag.report().toolRoundTrip.secondToolCallSeen);
  QCOMPARE(diag.report().toolRoundTrip.httpRequests, 2);
  QCOMPARE(server.requestCount(), 2); // 零第三轮
  QVERIFY(!diag.report().toolRoundTrip.followUpTextCompleted);
}

void TestH6ModelDiagnostics::cancelDuringFirstRequestZeroFollowUp() {
  ScriptServer server;
  QVERIFY(server.start([](const ScriptServer::Request &) {
    ScriptServer::Response hold;
    hold.hold = true;
    return hold;
  }));
  LlmConnectionDiagnostics diag;
  diag.runAction(LlmDiagnosticsAction::Tool, candidateFor(server.endpoint()));
  spinUntil([&] { return diag.busy() && server.requestCount() >= 1; });
  diag.cancel();
  QCOMPARE(diag.report().toolRoundTrip.status, LlmDiagnosticStatus::Cancelled);
  QVERIFY(!diag.busy());
  QCOMPARE(server.requestCount(), 1); // 首请求在途被取消
  // 取消后事件循环再跑一段：零后续 POST（取消即零后续）。
  QTest::qWait(150);
  QCOMPARE(server.requestCount(), 1);
}

void TestH6ModelDiagnostics::lateResultDroppedAfterNewCandidate() {
  ScriptServer server;
  QVERIFY(server.start([&server](const ScriptServer::Request &) {
    if (server.requestCount() == 1) {
      ScriptServer::Response hold;
      hold.hold = true; // 旧候选：挂起后由取消终止
      return hold;
    }
    return ScriptServer::Response{200, "application/json", {},
                                  {completionBody(QStringLiteral("新候选"))}};
  }));
  LlmConnectionDiagnostics diag;
  diag.runAction(LlmDiagnosticsAction::Text, candidateFor(server.endpoint()));
  spinUntil([&] { return diag.busy() && server.requestCount() >= 1; });
  diag.cancel(); // 旧代次终止
  QVERIFY(runAndWait(diag, LlmDiagnosticsAction::Text,
                     candidateFor(server.endpoint())));
  // 新报告只含新候选一次请求；旧候选的迟到路径不覆盖（报告已重置）。
  QCOMPARE(diag.report().text.httpRequests, 1);
  QCOMPARE(diag.report().text.status, LlmDiagnosticStatus::Passed);
  QCOMPARE(server.requestCount(), 2);
}

void TestH6ModelDiagnostics::redirectNotFollowedSecondEndpointZeroAuth() {
  ScriptServer secondServer;
  QVERIFY(secondServer.start([](const ScriptServer::Request &) {
    return ScriptServer::Response{200, "application/json", {},
                                  {completionBody(QStringLiteral("不应到达"))}};
  }));
  ScriptServer firstServer;
  QVERIFY(firstServer.start([&](const ScriptServer::Request &) {
    return ScriptServer::Response{
      307, "application/json",
      QByteArray("Location: ") + secondServer.endpoint().toString().toUtf8()
        + "/models\r\n",
      {}};
  }));
  LlmConnectionDiagnostics diag;
  QVERIFY(runAndWait(diag, LlmDiagnosticsAction::Catalog,
                     candidateFor(firstServer.endpoint())));
  QCOMPARE(diag.report().catalog.status, LlmDiagnosticStatus::Network);
  QVERIFY(diag.report().catalog.detail.contains(QStringLiteral("未跟随")));
  QCOMPARE(firstServer.requestCount(), 1);
  QCOMPARE(secondServer.requestCount(), 0); // 不跟随、不重发凭据
}

void TestH6ModelDiagnostics::destroyDuringProbeSafeShutdown() {
  // 关窗/销毁打断在途探测：析构安全终止（abort 在途请求），之后零新请求、
  // 零崩溃（生命周期由 QPointer/世代号纪律保证，与 G1 同款）。
  ScriptServer server;
  QVERIFY(server.start([](const ScriptServer::Request &) {
    ScriptServer::Response hold;
    hold.hold = true;
    return hold;
  }));
  auto *diag = new LlmConnectionDiagnostics;
  diag->runAction(LlmDiagnosticsAction::Text, candidateFor(server.endpoint()));
  spinUntil([&] { return diag->busy() && server.requestCount() >= 1; });
  delete diag; // 模拟对话框关闭：owner 析构
  QTest::qWait(150);
  QCOMPARE(server.requestCount(), 1); // 零新请求（无重试/无续发）
}

void TestH6ModelDiagnostics::catalogAllFilteredIsEmptyWithReason() {
  // 条目全部形状非法 → Empty 语义 + 原因 detail（不再「通过（0 项）」）。
  ScriptServer server;
  QVERIFY(server.start([](const ScriptServer::Request &) {
    const QJsonObject catalog{
      {"data", QJsonArray{QJsonObject{{"no_id", 1}},
                          QJsonObject{{"id", QStringLiteral("   ")}}}}};
    return ScriptServer::Response{200, "application/json", {},
                                  {QJsonDocument(catalog).toJson(QJsonDocument::Compact)}};
  }));
  LlmConnectionDiagnostics diag;
  QVERIFY(runAndWait(diag, LlmDiagnosticsAction::Catalog,
                     candidateFor(server.endpoint())));
  QCOMPARE(diag.report().catalog.status, LlmDiagnosticStatus::Empty);
  QVERIFY(diag.report().catalog.detail.contains(QStringLiteral("无可用条目")));
  QCOMPARE(diag.report().catalog.malformedEntries, 2);
}

QTEST_MAIN(TestH6ModelDiagnostics)
#include "tst_h6_modeldiagnostics.moc"
