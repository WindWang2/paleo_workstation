#include <QtTest>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "../src/ai/chat/chatmessage.h"
#include "../src/ai/chat/chatsession.h"
#include "../src/ai/chat/domaintools.h"
#include "../src/ai/chat/llmclient.h"

// 方向51：对话域模型 + 会话持久化 + SSE 解析 + 配置校验 + 领域工具表。
// 全部纯逻辑/本地磁盘（会话落在用户目录），不发网络请求。
class TestAiChat : public QObject {
  Q_OBJECT
private slots:
  void initTestCase();
  void messageRoundTripKeepsRoleAndToolFrames();
  void unknownRoleDegradesToUserNotAssistant();
  void sessionRoundTripAndStoreLivesOutsideProject();
  void streamParserReassemblesSplitChunksAndDone();
  void streamParserAccumulatesToolCallArguments();
  void streamParserReportsMalformedEvent();
  void configRejectsEmptyEndpointModelAndPlainHttp();
  void configBuildsCompletionsUrl();
  void domainToolsEnumerateWithValidSchemas();
  void toolDispatchIsHonestAboutNotBeingWired();

private:
  QString m_projectDir;
};

void TestAiChat::initTestCase() {
  QTemporaryDir dir;
  m_projectDir = dir.path();
}

void TestAiChat::messageRoundTripKeepsRoleAndToolFrames() {
  ChatMessage system;
  system.role = ChatRole::System;
  system.content = QStringLiteral("你是助手");
  QVERIFY(chatRoleFromKey(QStringLiteral("system"), nullptr));
  QCOMPARE(chatRoleKey(system.role), QStringLiteral("system"));

  ChatMessage assistant;
  assistant.role = ChatRole::Assistant;
  ChatToolCall call;
  call.id = QStringLiteral("call_1");
  call.name = QStringLiteral("paleo.tile_classification");
  call.argumentsJson = QStringLiteral("{\"model\":\"seg3\",\"grid_columns\":8}");
  assistant.toolCalls.append(call);

  const QJsonObject json = assistant.toJson();
  const ChatMessage back = ChatMessage::fromJson(json);
  QCOMPARE(back.role, ChatRole::Assistant);
  QCOMPARE(back.toolCalls.size(), 1);
  QCOMPARE(back.toolCalls.first(), call);
  QVERIFY(back.toolCalls.first().hasValidArguments());

  ChatMessage reply; // role=tool 回指调用
  reply.role = ChatRole::Tool;
  reply.toolCallId = call.id;
  reply.content = QStringLiteral("{\"cells\":[0,1]}");
  const ChatMessage replyBack = ChatMessage::fromJson(reply.toJson());
  QCOMPARE(replyBack.toolCallId, call.id);
  QCOMPARE(replyBack.role, ChatRole::Tool);

  // 请求体形态：timestamp 是本机元数据，协议里没有——混进去会被严格服务端拒收。
  ChatMessage user;
  user.role = ChatRole::User;
  user.content = QStringLiteral("hi");
  user.timestamp = QDateTime::currentDateTime();
  QVERIFY(user.toJson().contains(QStringLiteral("timestamp")));
  QVERIFY(!user.toProtocolJson().contains(QStringLiteral("timestamp")));
  QCOMPARE(user.toProtocolJson().value(QStringLiteral("role")).toString(),
           QStringLiteral("user"));
  QCOMPARE(user.toProtocolJson().value(QStringLiteral("content")).toString(),
           QStringLiteral("hi"));
}

void TestAiChat::unknownRoleDegradesToUserNotAssistant() {
  ChatRole role = ChatRole::Assistant;
  QVERIFY(!chatRoleFromKey(QStringLiteral("model"), &role));
  QCOMPARE(role, ChatRole::Assistant); // 不改写输出参数
  QJsonObject object;
  object.insert(QStringLiteral("role"), QStringLiteral("model"));
  const ChatMessage message = ChatMessage::fromJson(object);
  QCOMPARE(message.role, ChatRole::User); // 未知角色降级为用户，不冒充助手
}

void TestAiChat::sessionRoundTripAndStoreLivesOutsideProject() {
  // 硬纪律：会话文件不得落在工程目录里（对话不是解释成果）。
  const QString dir = ChatSessionStore::directory();
  QVERIFY2(!dir.isEmpty(), "会话目录必须可解析");
  QVERIFY2(!dir.startsWith(m_projectDir),
           qPrintable(QStringLiteral("会话目录落在工程里：%1").arg(dir)));

  ChatSession session;
  session.id = QStringLiteral("tst-aichat-session");
  session.model = QStringLiteral("test-model");
  ChatMessage user;
  user.role = ChatRole::User;
  user.content = QStringLiteral("这口井的层位怎么定？");
  session.messages.append(user);
  QString error;
  QVERIFY2(ChatSessionStore::save(session, &error), qPrintable(error));

  ChatSession loaded;
  QVERIFY2(ChatSessionStore::load(session.id, &loaded, &error),
           qPrintable(error));
  QCOMPARE(loaded.id, session.id);
  QCOMPARE(loaded.model, session.model);
  QCOMPARE(loaded.messages.size(), 1);
  QCOMPARE(loaded.messages.first().content, user.content);
  QCOMPARE(loaded.displayTitle(), user.content);
  QVERIFY(ChatSessionStore::listIds().contains(session.id));
  QVERIFY2(ChatSessionStore::remove(session.id, &error), qPrintable(error));
  QVERIFY(!ChatSessionStore::listIds().contains(session.id));

  // 空 id 必须被拒（不写无名文件）。
  ChatSession anonymous;
  QVERIFY(!ChatSessionStore::save(anonymous, &error));
  QVERIFY(!error.isEmpty());
}

void TestAiChat::streamParserReassemblesSplitChunksAndDone() {
  LlmStreamParser parser;
  const QByteArray event =
    "data: {\"choices\":[{\"delta\":{\"content\":\"地\"}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"层\"}}]}\n\n";
  // 逐字节喂：SSE 边界可能在任意位置被 TCP 切开。
  QStringList deltas;
  bool done = false;
  QString error;
  QString text;
  for (char byte : event) {
    parser.feed(QByteArray(1, byte), &deltas, &done, &error);
    // feed() 每次只给「本块新完成的事件」，跨调用要自己拼接。
    for (const QString &part : deltas)
      text += part;
  }
  QVERIFY2(error.isEmpty(), qPrintable(error));
  QVERIFY(!done);
  QCOMPARE(text, QStringLiteral("地层")); // 拼接语义：增量之和 = 完整文本

  parser.feed(QByteArrayLiteral("data: [DONE]\n\n"), &deltas, &done, &error);
  QVERIFY(done);
  QVERIFY(deltas.isEmpty());
}

void TestAiChat::streamParserAccumulatesToolCallArguments() {
  LlmStreamParser parser;
  QStringList deltas;
  bool done = false;
  QString error;
  // 首段带 id/name，后段只补 arguments（真实服务就是这个下发形状）。
  parser.feed(QByteArrayLiteral(
                "data: {\"choices\":[{\"delta\":{\"tool_calls\":["
                "{\"index\":0,\"id\":\"c1\",\"type\":\"function\",\"function\":"
                "{\"name\":\"paleo.tile_classification\",\"arguments\":\"{\\\"model\\\"\"}}]}}]}\n\n"),
              &deltas, &done, &error);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  QVERIFY(parser.takeCompleteCalls().isEmpty()); // 半截 JSON 不算完成
  parser.feed(QByteArrayLiteral(
                "data: {\"choices\":[{\"delta\":{\"tool_calls\":["
                "{\"index\":0,\"function\":{\"arguments\":\":\\\"seg3\\\"}\"}}]}}]}\n\n"),
              &deltas, &done, &error);
  const QVector<ChatToolCall> calls = parser.takeCompleteCalls();
  QCOMPARE(calls.size(), 1);
  QCOMPARE(calls.first().name, QStringLiteral("paleo.tile_classification"));
  QCOMPARE(calls.first().id, QStringLiteral("c1"));
  QCOMPARE(calls.first().argumentsJson, QStringLiteral("{\"model\":\"seg3\"}"));
  QVERIFY(calls.first().hasValidArguments());
  // 取过即不再取（防重复 emit）。
  QVERIFY(parser.takeCompleteCalls().isEmpty());
  QVERIFY(parser.flush().isEmpty());
}

void TestAiChat::streamParserReportsMalformedEvent() {
  LlmStreamParser parser;
  QStringList deltas;
  bool done = false;
  QString error;
  parser.feed(QByteArrayLiteral("data: {not json}\n\n"), &deltas, &done,
              &error);
  QVERIFY2(!error.isEmpty(), "非法事件必须如实报错");
  QVERIFY(deltas.isEmpty());
}

void TestAiChat::configRejectsEmptyEndpointModelAndPlainHttp() {
  LlmConfig empty;
  QVERIFY(!empty.enabled());
  QVERIFY(empty.validate().contains(QStringLiteral("尚未配置")));

  LlmConfig config = LlmConfig::fromParts(
    QUrl(QStringLiteral("http://example.com/v1")),
    QStringLiteral("test-model"), QByteArrayLiteral("sk-test"));
  QVERIFY(!config.enabled());
  QVERIFY(config.validate().contains(QStringLiteral("https://"))); // http 非 loopback 须 https

  // loopback 明文放行（测试/本机假端点）；缺模型仍禁用。
  LlmConfig loopback = LlmConfig::fromParts(
    QUrl(QStringLiteral("http://127.0.0.1:8000/v1")), QString(),
    QByteArrayLiteral("sk-test"));
  QVERIFY(!loopback.enabled());
  QVERIFY(loopback.validate().contains(QStringLiteral("模型")));
  loopback.model = QStringLiteral("test-model");
  QVERIFY(loopback.enabled());
  loopback.apiKey.clear();
  QVERIFY(!loopback.enabled()); // 无密钥 = 禁用态（不冒充可用）
  QVERIFY(loopback.validate().contains(QStringLiteral("密钥")));
}

void TestAiChat::configBuildsCompletionsUrl() {
  LlmConfig config = LlmConfig::fromParts(
    QUrl(QStringLiteral("https://api.example.com/v1")),
    QStringLiteral("test-model"));
  QCOMPARE(config.completionsUrl().toString(),
           QStringLiteral("https://api.example.com/v1/chat/completions"));
  config.endpoint = QUrl(QStringLiteral("https://api.example.com/v1/"));
  QCOMPARE(config.completionsUrl().toString(),
           QStringLiteral("https://api.example.com/v1/chat/completions"));
}

void TestAiChat::domainToolsEnumerateWithValidSchemas() {
  const QVector<AiToolSpec> tools = builtinAiToolSpecs();
  QCOMPARE(tools.size(), 3);
  QStringList names;
  for (const AiToolSpec &spec : tools) {
    QVERIFY2(!spec.name.isEmpty(), "工具必须有稳定 key");
    QVERIFY2(!spec.description.isEmpty(), "工具必须有描述");
    QVERIFY2(!spec.title.isEmpty(), "工具必须有显示名");
    QVERIFY2(!spec.parameters.isEmpty(), "工具必须有入参表");
    names.append(spec.name);
    // 入参 schema 与 function 描述都要是可解析的 JSON object。
    QVERIFY(spec.toJsonSchema().value(QStringLiteral("type")).toString() ==
            QStringLiteral("object"));
    QVERIFY(spec.toChatFunction().value(QStringLiteral("name")).toString() ==
            spec.name);
    QJsonParseError parseError;
    QJsonDocument::fromJson(spec.resultSchemaJson.toUtf8(), &parseError);
    QVERIFY2(parseError.error == QJsonParseError::NoError,
             qPrintable(parseError.errorString()));
    // 必填项缺失必须被拒，且拒绝理由带参数名。
    QJsonObject blank;
    QVERIFY(!spec.validateParameters(blank).isEmpty());
  }
  QVERIFY(names.contains(QStringLiteral("paleo.tile_classification")));
  QVERIFY(names.contains(QStringLiteral("paleo.horizon_tracking_suggestion")));
  QVERIFY(names.contains(QStringLiteral("paleo.well_facies_prediction")));
  QVERIFY(aiToolSpec(QStringLiteral("paleo.nonexistent")).name.isEmpty());
}

void TestAiChat::toolDispatchIsHonestAboutNotBeingWired() {
  // 未登记 → NotFound，且不给 target。
  const AiToolDispatch unknown =
    dispatchAiTool(QStringLiteral("paleo.nonexistent"), {});
  QCOMPARE(unknown.status, AiToolDispatchStatus::NotFound);
  QVERIFY(unknown.target.isEmpty());

  // 已登记、参数齐全 → 必须写出「入口在哪」与「为什么现在跑不了」。
  QJsonObject arguments;
  arguments.insert(QStringLiteral("model"), QStringLiteral("seg3"));
  arguments.insert(QStringLiteral("grid_columns"), 8);
  arguments.insert(QStringLiteral("grid_rows"), 8);
  const AiToolDispatch tiled =
    dispatchAiTool(QStringLiteral("paleo.tile_classification"), arguments);
  QVERIFY(tiled.status == AiToolDispatchStatus::NotImplemented ||
          tiled.status == AiToolDispatchStatus::Disabled);
  QVERIFY2(!tiled.target.isEmpty(), "分发必须给出目标入口");
  QVERIFY2(!tiled.note.isEmpty(), "分发必须说清为什么不可执行");
  QVERIFY(tiled.note.contains(QStringLiteral("未接线")) ||
          tiled.note.contains(QStringLiteral("ONNX")));

  // 参数不合法 → Disabled + 原因（不假装已受理）。
  QJsonObject bad;
  bad.insert(QStringLiteral("grid_columns"), QStringLiteral("八"));
  const AiToolDispatch rejected = dispatchAiTool(
    QStringLiteral("paleo.tile_classification"), bad);
  QCOMPARE(rejected.status, AiToolDispatchStatus::Disabled);
  QVERIFY(!rejected.note.isEmpty());
}

int main(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);
  TestAiChat tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_aichat.moc"
