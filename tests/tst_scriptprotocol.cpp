#include "../src/services/scriptprotocol.h"

#include <QtTest>

// 方向68：脚本 stdout JSON 行协议解析——progress/result/error 三型 +
// 一切不遵守协议的行按纯文本兜底（协议是可选约定，永不强制）。
class TestScriptProtocol : public QObject
{
  Q_OBJECT
private slots:
  void progressLine();
  void resultLine();
  void errorLine();
  void plainTextPassesThrough();
  void jsonWithoutMarkerIsText();
  void malformedJsonIsText();
  void unknownTypeIsText();
  void missingFieldsDefault();
};

void TestScriptProtocol::progressLine()
{
  const ScriptMessage msg = parseScriptLine(QStringLiteral(
      R"({"paleo":"1","type":"progress","percent":42,"message":"跑到一半"})"));
  QCOMPARE(msg.type, ScriptMessage::Type::Progress);
  QCOMPARE(msg.percent, 42);
  QCOMPARE(msg.message, QStringLiteral("跑到一半"));
  QVERIFY(msg.raw.startsWith(QLatin1Char('{')));
}

void TestScriptProtocol::resultLine()
{
  const ScriptMessage msg = parseScriptLine(QStringLiteral(
      R"({"paleo":"1","type":"result","path":"out/result.geojson","kind":"geojson","message":"done"})"));
  QCOMPARE(msg.type, ScriptMessage::Type::Result);
  QCOMPARE(msg.path, QStringLiteral("out/result.geojson"));
  QCOMPARE(msg.kind, QStringLiteral("geojson"));
  QCOMPARE(msg.message, QStringLiteral("done"));
}

void TestScriptProtocol::errorLine()
{
  const ScriptMessage msg = parseScriptLine(QStringLiteral(
      R"({"paleo":"1","type":"error","message":"读不到输入","code":7})"));
  QCOMPARE(msg.type, ScriptMessage::Type::Error);
  QCOMPARE(msg.message, QStringLiteral("读不到输入"));
  QCOMPARE(msg.code, 7);
}

void TestScriptProtocol::plainTextPassesThrough()
{
  const ScriptMessage msg = parseScriptLine(QStringLiteral("hello plain stdout"));
  QCOMPARE(msg.type, ScriptMessage::Type::Text);
  QCOMPARE(msg.raw, QStringLiteral("hello plain stdout"));
}

void TestScriptProtocol::jsonWithoutMarkerIsText()
{
  // 合法 JSON 但无 "paleo":"1" 标记——不猜协议意图，按纯文本呈现。
  const ScriptMessage msg = parseScriptLine(
      QStringLiteral(R"({"type":"progress","percent":42})"));
  QCOMPARE(msg.type, ScriptMessage::Type::Text);
}

void TestScriptProtocol::malformedJsonIsText()
{
  QCOMPARE(parseScriptLine(QStringLiteral("{not json")).type,
           ScriptMessage::Type::Text);
  QCOMPARE(parseScriptLine(QStringLiteral("[1,2,3]")).type,
           ScriptMessage::Type::Text);
  QCOMPARE(parseScriptLine(QString()).type, ScriptMessage::Type::Text);
}

void TestScriptProtocol::unknownTypeIsText()
{
  const ScriptMessage msg = parseScriptLine(
      QStringLiteral(R"({"paleo":"1","type":"surprise"})"));
  QCOMPARE(msg.type, ScriptMessage::Type::Text);
}

void TestScriptProtocol::missingFieldsDefault()
{
  const ScriptMessage progress = parseScriptLine(
      QStringLiteral(R"({"paleo":"1","type":"progress"})"));
  QCOMPARE(progress.type, ScriptMessage::Type::Progress);
  QCOMPARE(progress.percent, -1);
  QVERIFY(progress.message.isEmpty());

  const ScriptMessage error = parseScriptLine(
      QStringLiteral(R"({"paleo":"1","type":"error"})"));
  QCOMPARE(error.type, ScriptMessage::Type::Error);
  QCOMPARE(error.code, 0);

  const ScriptMessage result = parseScriptLine(
      QStringLiteral(R"({"paleo":"1","type":"result"})"));
  QCOMPARE(result.type, ScriptMessage::Type::Result);
  QVERIFY(result.path.isEmpty());
  QVERIFY(result.kind.isEmpty());
}

QTEST_GUILESS_MAIN(TestScriptProtocol)
#include "tst_scriptprotocol.moc"
