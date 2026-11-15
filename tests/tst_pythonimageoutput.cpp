// 层：测试壳
#include "services/pythonimageoutput.h"
#include "services/pythongenerated.h"
#include "fixtures/pythonanalysisreportfixture.h"
#include <QCryptographicHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtEndian>
#include <QtTest>
#include <cstring>
#include <limits>

using namespace paleo::python;
namespace {
QJsonObject tableDeclaration() {
  return {{"contract", "paleo.python-table-declaration.v1"}, {"maxRows", 2}, {"columns", QJsonArray{
    QJsonObject{{"id", "source_row"}, {"label", "Source row"}, {"unit", "1"}, {"dtype", "<i8"}, {"role", "source_row"}},
    QJsonObject{{"id", "value"}, {"label", "Value"}, {"unit", "API"}, {"dtype", "<f8"}, {"role", "value"}}}}};
}
QJsonObject declaration(int width = 3, int height = 2) {
  return {{"contract", "paleo.python-image-declaration.v1"}, {"width", width}, {"height", height}, {"format", "rgba8"}};
}
QByteArray table() {
  QByteArray bytes(34, 0); qToLittleEndian<qint64>(1, bytes.data()); qToLittleEndian<qint64>(2, bytes.data() + 8);
  for (int index = 0; index < 2; ++index) {
    const double number = index ? 2.5 : 1.25; quint64 bits; std::memcpy(&bits, &number, 8);
    qToLittleEndian<quint64>(bits, bytes.data() + 17 + index * 8);
  } return bytes;
}
QByteArray pixels() {
  QByteArray bytes;
  for (int index = 0; index < 6; ++index) {
    bytes += char(117 + index * 7); bytes += char(42 + index * 11); bytes += char(19 + index * 17); bytes += char(index * 47);
  } return bytes;
}
QByteArray report() { return R"({"contract":"paleo.python-generated-report.v1","metrics":[],"tables":[],"notes":[]})"; }
QJsonObject port(const char *id, const char *encoding, const QByteArray &bytes) {
  return {{"id", id}, {"encoding", encoding}, {"size", bytes.size()},
    {"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}};
}
QByteArray frame(const QJsonObject &image = declaration(), const QByteArray &rgba = pixels(),
                 const std::function<void(QJsonObject &)> &edit = {}, const QByteArray &body = report(), const QByteArray &numeric = table()) {
  auto tab = port("TABLE", "paleo.numeric-columns.v1", numeric); tab["rows"] = 2; tab["columns"] = tableDeclaration().value("columns");
  auto pic = port("IMAGE", "rgba8", rgba); pic["width"] = image.value("width"); pic["height"] = image.value("height");
  QJsonObject header{{"contract", "paleo.python-output-frame.v3"}, {"ports", QJsonArray{port("REPORT", "utf8-json", body), tab, pic}}};
  if (edit) edit(header); const auto json = QJsonDocument(header).toJson(QJsonDocument::Compact);
  QByteArray out("PALEOPYO3\n"); char length[4]; qToLittleEndian<quint32>(quint32(json.size()), length);
  return out + QByteArray(length, 4) + json + body + numeric + rgba;
}
QByteArray replaceHeader(const QByteArray &original, const QByteArray &header) {
  const auto previous = qFromLittleEndian<quint32>(original.constData() + 10); char length[4];
  qToLittleEndian<quint32>(quint32(header.size()), length);
  return original.left(10) + QByteArray(length, 4) + header + original.mid(14 + previous);
}
AnalysisReportValidation validateReport(const QByteArray &body) {
  return validateGeneratedReport(QByteArray("{\"contract\":\"paleo.python-output.v1\",\"ports\":{\"REPORT\":") + body + "}}",
      report_fixture::metadata(AnalysisKind::Generated, 2, 1), QString(64, 'a'), QString(64, 'b'), {});
}
ImageOutputValidation accept(const QByteArray &bytes, const QJsonObject &image = declaration()) {
  return validateImageOutput(bytes, tableDeclaration(), image, pngEncoderIdentity(), {}, validateReport);
}
void emptyFailure(const ImageOutputValidation &result) {
  QVERIFY(!result.ok()); QVERIFY(result.report.isEmpty()); QVERIFY(result.csv.isEmpty()); QVERIFY(result.png.isEmpty());
  QVERIFY(result.tableMetadata.isEmpty()); QVERIFY(result.imageMetadata.isEmpty()); QVERIFY(result.summary.isEmpty()); QCOMPARE(result.hostReservedBytes, 0);
}
void roundTrip(const ImageOutputValidation &result, const QJsonObject &image, const QByteArray &rgba) {
  // Decoding here accepts only our host-produced PNG, never a child-supplied format.
  const auto decoded = QImage::fromData(result.png, "PNG").convertToFormat(QImage::Format_RGBA8888);
  QCOMPARE(decoded.width(), image.value("width").toInt()); QCOMPARE(decoded.height(), image.value("height").toInt());
  for (int row = 0; row < decoded.height(); ++row)
    QCOMPARE(std::memcmp(decoded.constScanLine(row), rgba.constData() + row * decoded.width() * 4, size_t(decoded.width() * 4)), 0);
}
}
class TestPythonImageOutput : public QObject {
  Q_OBJECT
private slots:
  void returnsTheValidatedReportCsvAndHostPngTogether() {
    const auto bytes = frame(); const auto result = accept(bytes); QVERIFY2(result.ok(), qPrintable(result.code));
    QCOMPARE(result.csv, QByteArray("source_row,value\r\n1,1.25\r\n2,2.5\r\n"));
    QVERIFY(result.png.startsWith(QByteArray::fromHex("89504e470d0a1a0a"))); QVERIFY(result.png.size() <= ImageOutputPngBytes);
    roundTrip(result, declaration(), pixels()); QCOMPARE(result.png, accept(bytes).png);
    QCOMPARE(result.tableMetadata.value("rows").toInt(), 2); QCOMPARE(result.imageMetadata.value("spatial"), QJsonValue(false));
    const auto provenance = result.imageMetadata.value("provenance").toObject();
    QCOMPARE(provenance.value("encoder"), QJsonValue(pngEncoderIdentity())); QCOMPARE(provenance.value("declaration"), QJsonValue(declaration()));
    QCOMPARE(provenance.value("sourceRgbaSha256").toString(), QString::fromLatin1(QCryptographicHash::hash(pixels(), QCryptographicHash::Sha256).toHex()));
    QCOMPARE(QJsonDocument::fromJson(result.report).object().value("codeSha256").toString(), QString(64, 'a'));
    QCOMPARE(result.hostReservedBytes, imageOutputHostReservation(bytes.size(), tableDeclaration(), declaration()));
    QCOMPARE(result.summary.value("hostOutputReservedBytes").toString(), QString::number(result.hostReservedBytes));
  }
  void rejectsMalformedFramesAndAnyBadPortWithoutPartialResults_data() {
    QTest::addColumn<QByteArray>("bytes"); const auto valid = frame();
    QTest::newRow("truncated") << valid.chopped(1); QTest::newRow("trailing") << valid + 'x';
    QTest::newRow("frame-ceiling") << valid + QByteArray(NumericOutputFrameBytes + 1 - valid.size(), 0);
    auto oversized = valid; qToLittleEndian<quint32>(16385, oversized.data() + 10); QTest::newRow("header-budget") << oversized;
    auto magic = valid; magic[8] = '2'; QTest::newRow("old-magic") << magic;
    const auto headerSize = qFromLittleEndian<quint32>(valid.constData() + 10);
    auto header = valid.mid(14, headerSize); header.insert(1, "\"contract\":\"duplicate\",");
    QTest::newRow("header-duplicate-key") << replaceHeader(valid, header);
    header = valid.mid(14, headerSize); header.replace("paleo.python-output-frame.v3", QByteArray(1, char(0xff)));
    QTest::newRow("header-invalid-utf8") << replaceHeader(valid, header);
    const auto edit = [&](const char *name, const std::function<void(QJsonObject &)> &change) { QTest::newRow(name) << frame(declaration(), pixels(), change); };
    edit("old-contract", [](auto &h) { h["contract"] = "paleo.python-output-frame.v2"; });
    edit("child-path", [](auto &h) { h["path"] = "image.png"; });
    edit("wrong-order", [](auto &h) { const auto p = h["ports"].toArray(); h["ports"] = QJsonArray{p.at(2), p.at(1), p.at(0)}; });
    edit("duplicate-port", [](auto &h) { auto p = h["ports"].toArray(); p[2] = p.at(0); h["ports"] = p; });
    edit("child-png", [](auto &h) { auto p = h["ports"].toArray(); auto v = p.at(2).toObject(); v["encoding"] = "png"; p[2] = v; h["ports"] = p; });
    edit("child-mime", [](auto &h) { auto p = h["ports"].toArray(); auto v = p.at(2).toObject(); v["mime"] = "image/png"; p[2] = v; h["ports"] = p; });
    edit("changed-width", [](auto &h) { auto p = h["ports"].toArray(); auto v = p.at(2).toObject(); v["width"] = 2; p[2] = v; h["ports"] = p; });
    edit("wrong-size", [](auto &h) { auto p = h["ports"].toArray(); auto v = p.at(2).toObject(); v["size"] = 23; p[2] = v; h["ports"] = p; });
    edit("forged-column", [](auto &h) { auto p = h["ports"].toArray(); auto v = p.at(1).toObject(); v["columns"] = QJsonArray{}; p[1] = v; h["ports"] = p; });
    for (int index = 0; index < 3; ++index) edit(qPrintable(QString("port-%1-hash").arg(index)), [index](auto &h) {
      auto p = h["ports"].toArray(); auto v = p.at(index).toObject(); v["sha256"] = QString(64, 'f'); p[index] = v; h["ports"] = p;
    });
    auto corrupt = valid; corrupt[corrupt.size() - 1] = '!'; QTest::newRow("same-size-rgba-tamper") << corrupt;
    QTest::newRow("short-rgba") << frame(declaration(), pixels().chopped(1));
    QTest::newRow("long-rgba") << frame(declaration(), pixels() + 'x');
    QTest::newRow("report-business-contract") << frame(declaration(), pixels(), {}, "{}");
    QTest::newRow("report-duplicate-key") << frame(declaration(), pixels(), {}, R"({"contract":"a","\u0063ontract":"b"})");
    auto numbers = table(); qToLittleEndian<quint64>(0x7ff0000000000000ULL, numbers.data() + 17);
    QTest::newRow("nonfinite-table") << frame(declaration(), pixels(), {}, report(), numbers);
  }
  void rejectsMalformedFramesAndAnyBadPortWithoutPartialResults() { QFETCH(QByteArray, bytes); emptyFailure(accept(bytes)); }
  void rejectsInvalidFrozenDimensionsBeforeReportValidation_data() {
    QTest::addColumn<QJsonObject>("image");
    for (const auto &entry : QVector<QPair<const char *, QJsonValue>>{{"zero", 0}, {"negative", -1}, {"too-wide", 1025},
        {"fractional", 1.5}, {"overflow", std::numeric_limits<double>::max()}, {"wrong-type", "3"}}) {
      auto image = declaration(); image["width"] = entry.second; QTest::newRow(entry.first) << image;
    }
    auto extra = declaration(); extra["crs"] = "EPSG:4326"; QTest::newRow("child-spatial-claim") << extra;
    auto height = declaration(); height["height"] = 1025; QTest::newRow("too-high") << height;
    auto format = declaration(); format["format"] = "argb32"; QTest::newRow("different-format") << format;
  }
  void rejectsInvalidFrozenDimensionsBeforeReportValidation() {
    QFETCH(QJsonObject, image); bool readReport = false;
    const auto result = validateImageOutput(frame(), tableDeclaration(), image, pngEncoderIdentity(), {}, [&](const auto &bytes) { readReport = true; return validateReport(bytes); });
    QCOMPARE(result.code, QString("python_image_declaration_invalid")); emptyFailure(result); QVERIFY(!readReport);
    QCOMPARE(imageOutputHostReservation(NumericOutputFrameBytes, tableDeclaration(), image), -1);
  }
  void refusesEncoderDriftAndAccountsHostMemoryBeforeEncoding() {
    auto encoder = pngEncoderIdentity(); encoder["qtRuntimeVersion"] = "another-version"; bool readReport = false;
    const auto validator = [&](const auto &bytes) { readReport = true; return validateReport(bytes); };
    const auto changed = validateImageOutput(frame(), tableDeclaration(), declaration(), encoder, {}, validator);
    QCOMPARE(changed.code, QString("python_output_encoder_changed")); emptyFailure(changed); QVERIFY(!readReport);
    ExecutionPolicy policy; policy.memoryBytes = 1;
    const auto small = validateImageOutput(frame(), tableDeclaration(), declaration(), pngEncoderIdentity(), policy, validator);
    QCOMPARE(small.code, QString("python_output_memory_budget")); emptyFailure(small); QVERIFY(!readReport);
    policy = {}; policy.outputBytes = 128;
    QCOMPARE(validateImageOutput(frame(), tableDeclaration(), declaration(), pngEncoderIdentity(), policy, validator).code, QString("python_output_budget_exceeded"));
    QVERIFY(!readReport); QCOMPARE(imageOutputHostReservation(NumericOutputFrameBytes + 1, tableDeclaration(), declaration()), -1);
  }
  void cancellationAfterBusinessValidationStillReturnsNoPorts() {
    bool checked = false;
    const auto result = validateImageOutput(frame(), tableDeclaration(), declaration(), pngEncoderIdentity(), {},
      [&](const auto &bytes) { checked = true; return validateReport(bytes); }, [&] { return checked; });
    QVERIFY(checked); QCOMPARE(result.code, QString("cancelled")); emptyFailure(result);
    QCOMPARE(validateImageOutput(frame(), tableDeclaration(), declaration(), pngEncoderIdentity(), {}, validateReport, [] { return true; }).code, QString("cancelled"));
  }
  void finalHostEncodedGroupAlsoObeysTheFrozenOutputBudget() {
    const auto bytes = frame(); const auto accepted = accept(bytes); QVERIFY(accepted.ok());
    const auto total = accepted.report.size() + accepted.csv.size() + accepted.png.size(); QVERIFY(total > bytes.size());
    ExecutionPolicy policy; policy.outputBytes = total - 1;
    const auto result = validateImageOutput(bytes, tableDeclaration(), declaration(), pngEncoderIdentity(), policy, validateReport);
    QCOMPARE(result.code, QString("python_output_budget_exceeded")); emptyFailure(result);
  }
  void maximumDimensionsAreBoundedAndIncompressiblePngCannotOverrunTheCap() {
    const auto image = declaration(1024, 1024); QByteArray rgba(1024 * 1024 * 4, 0);
    const auto compact = accept(frame(image, rgba), image); QVERIFY2(compact.ok(), qPrintable(compact.code)); roundTrip(compact, image, rgba);
    QVERIFY(compact.png.size() < 64 * 1024);
    quint64 state = 0x9e3779b97f4a7c15ULL;
    for (qsizetype offset = 0; offset < rgba.size(); offset += 8) {
      state ^= state >> 12; state ^= state << 25; state ^= state >> 27; const auto word = state * 2685821657736338717ULL;
      qToLittleEndian<quint64>(word, rgba.data() + offset);
    }
    const auto rejected = accept(frame(image, rgba), image); QCOMPARE(rejected.code, QString("python_png_budget_exceeded")); emptyFailure(rejected);
  }
};
QTEST_GUILESS_MAIN(TestPythonImageOutput)
#include "tst_pythonimageoutput.moc"
