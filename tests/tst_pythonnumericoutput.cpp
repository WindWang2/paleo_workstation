// 层：测试壳
#include "services/pythonnumericoutput.h"
#include "services/pythongenerated.h"
#include "fixtures/pythonanalysisreportfixture.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QtEndian>
#include <QtTest>
#include <cstring>
#include <limits>

using namespace paleo::python;
namespace {
QJsonObject column(const QString &id, const QString &type, const QString &role = "value") {
  return {{"id", id}, {"label", id}, {"unit", "1"}, {"dtype", type}, {"role", role}};
}
QJsonObject declaration(int count = 3, int rows = 8192) {
  QJsonArray columns{column("source_row", "<i8", "source_row")};
  for (int index = 1; index < count; ++index) columns.append(column(QString("c%1").arg(index), index == 2 ? "|b1" : "<f8"));
  return {{"contract", "paleo.python-table-declaration.v1"}, {"columns", columns}, {"maxRows", rows}};
}
void integer(QByteArray &data, qint64 value) {
  char bytes[8]; qToLittleEndian<qint64>(value, bytes); data.append(bytes, 8);
}
void number(QByteArray &data, double value) {
  quint64 bits; std::memcpy(&bits, &value, 8); char bytes[8]; qToLittleEndian<quint64>(bits, bytes); data.append(bytes, 8);
}
QByteArray table(const QJsonObject &decl, int rows = 3) {
  QByteArray data;
  for (const auto &entry : decl.value("columns").toArray()) {
    const auto value = entry.toObject(); const auto type = value.value("dtype").toString();
    for (int row = 0; row < rows; ++row) {
      if (type == "<i8") integer(data, qint64(9007199254740993LL) + row);
      else if (type == "<f8") number(data, row + 0.5);
      else data += char(row % 2);
    }
    data += QByteArray((rows + 7) / 8, 0);
  }
  return data;
}
QByteArray report() { return R"({"contract":"paleo.python-generated-report.v1","metrics":[],"tables":[],"notes":[]})"; }
QJsonObject port(const char *id, const char *encoding, const QByteArray &bytes) {
  return {{"id", id}, {"encoding", encoding}, {"size", bytes.size()},
      {"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}};
}
QByteArray frame(const QJsonObject &decl, const QByteArray &data, int rows = 3, const QByteArray &body = report(),
                 const std::function<void(QJsonObject &)> &edit = {}) {
  auto tablePort = port("TABLE", "paleo.numeric-columns.v1", data);
  tablePort["rows"] = rows; tablePort["columns"] = decl.value("columns");
  QJsonObject header{{"contract", "paleo.python-output-frame.v2"},
      {"ports", QJsonArray{port("REPORT", "utf8-json", body), tablePort}}};
  if (edit) edit(header);
  const auto json = QJsonDocument(header).toJson(QJsonDocument::Compact);
  QByteArray out("PALEOPYO2\n"); char length[4]; qToLittleEndian<quint32>(json.size(), length);
  return out + QByteArray(length, 4) + json + body + data;
}
AnalysisReportValidation validateReport(const QByteArray &body) {
  const auto outer = QByteArray("{\"contract\":\"paleo.python-output.v1\",\"ports\":{\"REPORT\":") + body + "}}";
  return validateGeneratedReport(outer, report_fixture::metadata(AnalysisKind::Generated, 3, 1), QString(64, 'a'), QString(64, 'b'), {});
}
NumericOutputValidation accept(const QByteArray &bytes, const QJsonObject &decl = declaration()) {
  return validateNumericOutput(bytes, decl, {}, validateReport);
}
void emptyFailure(const NumericOutputValidation &result) {
  QVERIFY(!result.ok()); QVERIFY(result.report.isEmpty()); QVERIFY(result.csv.isEmpty());
  QVERIFY(result.tableMetadata.isEmpty()); QVERIFY(result.summary.isEmpty());
}
}
class TestPythonNumericOutput : public QObject {
  Q_OBJECT
private slots:
  void encodesTypedValuesAndKeepsIntegerAxisExact() {
    const auto decl = declaration(); const auto accepted = accept(frame(decl, table(decl)));
    QVERIFY2(accepted.ok(), qPrintable(accepted.code));
    QCOMPARE(accepted.csv, QByteArray("source_row,c1,c2\r\n9007199254740993,0.5,false\r\n9007199254740994,1.5,true\r\n9007199254740995,2.5,false\r\n"));
    QCOMPARE(accepted.tableMetadata.value("rows").toInt(), 3);
    const auto report = QJsonDocument::fromJson(accepted.report).object();
    QCOMPARE(report.value("codeSha256").toString(), QString(64, 'a'));
    QCOMPARE(report.value("claims").toString(), QString("agent_generated_values_not_independently_recomputed"));
  }
  void csvHasOnlyFixedColumnIdsAndLocaleIndependentNumbers() {
    auto decl = declaration(2); auto columns = decl.value("columns").toArray(); auto value = columns[1].toObject();
    value["label"] = "=HYPERLINK(\"untrusted\")"; value["unit"] = "m/s"; columns[1] = value; decl["columns"] = columns;
    const auto previous = QLocale(); QLocale::setDefault(QLocale(QLocale::German));
    const auto accepted = accept(frame(decl, table(decl)), decl); QLocale::setDefault(previous);
    QVERIFY2(accepted.ok(), qPrintable(accepted.code)); QVERIFY(!accepted.csv.contains("HYPERLINK"));
    QVERIFY(accepted.csv.contains(",0.5\r\n")); QCOMPARE(accepted.tableMetadata.value("columns"), QJsonValue(columns));
  }
  void nullIsAnEmptyFieldRatherThanZeroOrFalse() {
    const auto decl = declaration(); auto data = table(decl);
    const int firstColumnBytes = 25, secondColumnBytes = 25;
    for (int byte = 0; byte < 8; ++byte) data[firstColumnBytes + 8 + byte] = 0;
    data[firstColumnBytes + 24] = char(2); data[firstColumnBytes + secondColumnBytes + 1] = 0;
    data[data.size() - 1] = char(2);
    const auto accepted = accept(frame(decl, data)); QVERIFY2(accepted.ok(), qPrintable(accepted.code));
    QVERIFY(accepted.csv.contains("9007199254740994,,\r\n"));
  }
  void rejectsMalformedFramesAsAWhole_data() {
    QTest::addColumn<QByteArray>("bytes"); const auto decl = declaration(); const auto data = table(decl);
    const auto valid = frame(decl, data);
    QTest::newRow("truncated") << valid.chopped(1); QTest::newRow("trailing") << valid + 'x';
    auto oversized = valid; qToLittleEndian<quint32>(16385, oversized.data() + 10); QTest::newRow("header-budget") << oversized;
    auto edit = [&](const char *name, const std::function<void(QJsonObject &)> &change) { QTest::newRow(name) << frame(decl, data, 3, report(), change); };
    edit("wrong-profile", [](auto &h) { h["contract"] = "paleo.python-output.v1"; });
    edit("extra-path", [](auto &h) { h["path"] = "arbitrary.csv"; });
    edit("duplicate-port", [](auto &h) { auto p = h["ports"].toArray(); p[1] = p[0]; h["ports"] = p; });
    edit("wrong-order", [](auto &h) { auto p = h["ports"].toArray(); h["ports"] = QJsonArray{p[1], p[0]}; });
    edit("forged-column", [](auto &h) { auto p = h["ports"].toArray(); auto t = p[1].toObject(); t["columns"] = QJsonArray{}; p[1] = t; h["ports"] = p; });
    edit("bad-payload-sha", [](auto &h) { auto p = h["ports"].toArray(); auto t = p[1].toObject(); t["sha256"] = QString(64, 'f'); p[1] = t; h["ports"] = p; });
    QTest::newRow("report-business-contract") << frame(decl, data, 3, "{}");
    QTest::newRow("report-duplicate-key") << frame(decl, data, 3, R"({"contract":"a","\u0063ontract":"b"})");
    QTest::newRow("report-bad-utf8") << frame(decl, data, 3, QByteArray("{\"x\":\"\xff\"}", 9));
  }
  void rejectsMalformedFramesAsAWhole() { QFETCH(QByteArray, bytes); emptyFailure(accept(bytes)); }
  void rejectsInvalidNumbersMasksAndAxes_data() {
    QTest::addColumn<QByteArray>("payload"); const auto decl = declaration(); auto original = table(decl);
    auto add = [&](const char *name, const std::function<void(QByteArray &)> &edit) { auto data = original; edit(data); QTest::newRow(name) << data; };
    add("nonfinite", [](auto &data) { quint64 bits = 0x7ff0000000000000ULL; qToLittleEndian<quint64>(bits, data.data() + 25); });
    add("nan", [](auto &data) { qToLittleEndian<quint64>(0x7ff8000000000001ULL, data.data() + 25); });
    add("noncanonical-null", [](auto &data) { data[49] = 1; });
    add("unused-mask-bits", [](auto &data) { data[49] = char(128); });
    add("null-axis", [](auto &data) { data[24] = 1; });
    add("repeated-axis", [](auto &data) { std::memcpy(data.data() + 8, data.constData(), 8); });
    add("zero-axis", [](auto &data) { qToLittleEndian<qint64>(0, data.data()); });
    add("invalid-bool", [](auto &data) { data[50] = 2; });
    QTest::newRow("extra-payload") << original + 'x'; QTest::newRow("short-payload") << original.chopped(1);
  }
  void rejectsInvalidNumbersMasksAndAxes() {
    QFETCH(QByteArray, payload); const auto decl = declaration(); emptyFailure(accept(frame(decl, payload)));
  }
  void handlesMaximumShapeAndCancellationWithoutPartialPorts() {
    const auto decl = declaration(16); const auto data = table(decl, 8192); const auto bytes = frame(decl, data, 8192);
    const auto accepted = accept(bytes, decl); QVERIFY2(accepted.ok(), qPrintable(accepted.code));
    QVERIFY(accepted.csv.size() <= NumericCsvBytes); QCOMPARE(accepted.tableMetadata.value("rows").toInt(), 8192);
    int checks = 0; emptyFailure(validateNumericOutput(bytes, decl, {}, validateReport, [&] { return ++checks > 8; }));
    ExecutionPolicy small; small.outputBytes = 128;
    emptyFailure(validateNumericOutput(bytes, decl, small, validateReport));
    small = {}; small.memoryBytes = 1;
    const auto rejected = validateNumericOutput(bytes, decl, small, validateReport);
    QCOMPARE(rejected.code, QString("python_output_memory_budget")); emptyFailure(rejected);
  }
  void declarationRejectsStringsFormulaIdsUnknownAxesAndOversizedShapes() {
    for (const auto &edit : QVector<std::function<void(QJsonObject &)>>{
      [](auto &d) { d["maxRows"] = 8193; },
      [](auto &d) { d["path"] = "output.csv"; },
      [](auto &d) { auto c = d["columns"].toArray(); auto v = c[1].toObject(); v["dtype"] = "string"; c[1] = v; d["columns"] = c; },
      [](auto &d) { auto c = d["columns"].toArray(); auto v = c[1].toObject(); v["id"] = "=formula"; c[1] = v; d["columns"] = c; },
      [](auto &d) { auto c = d["columns"].toArray(); auto v = c[1].toObject(); v["id"] = "column\n"; c[1] = v; d["columns"] = c; },
      [](auto &d) { auto c = d["columns"].toArray(); auto v = c[1].toObject(); v["unit"] = "unknown"; c[1] = v; d["columns"] = c; },
      [](auto &d) { auto c = d["columns"].toArray(); auto v = c[2].toObject(); v["unit"] = "m"; c[2] = v; d["columns"] = c; },
      [](auto &d) { auto c = d["columns"].toArray(); c[1] = c[0]; d["columns"] = c; },
      [](auto &d) { auto c = d["columns"].toArray(); auto v = c[0].toObject(); v["unit"] = "m"; c[0] = v; d["columns"] = c; }
    }) { auto decl = declaration(); edit(decl); QVERIFY(!validateNumericOutputDeclaration(decl).isEmpty()); }
  }
};
QTEST_GUILESS_MAIN(TestPythonNumericOutput)
#include "tst_pythonnumericoutput.moc"
