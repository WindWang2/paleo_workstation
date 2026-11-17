// 层：数据
#include "pythonimageoutput.h"
#include "pythonnumericoutput_internal.h"
#include "pythonoutputjson.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QImage>
#include <QImageWriter>
#include <QJsonDocument>
#include <QtEndian>
#include <QtGlobal>

namespace paleo::python {
namespace {
using namespace numeric_output;
constexpr qint64 HeaderBytes = 16 * 1024;
constexpr qint64 ReportBytes = 256 * 1024;
constexpr qint64 TableBytes = 2 * 1024 * 1024;
constexpr qint64 RgbaBytes = 1024 * 1024 * 4;
constexpr qsizetype Prefix = 14;
constexpr int QtPngCompression = 100;
bool dimensions(const QJsonObject &declaration, qint64 &width, qint64 &height) {
  return fields(declaration, {"contract", "width", "height", "format"}) &&
      declaration.value("contract") == "paleo.python-image-declaration.v1" && declaration.value("format") == "rgba8" &&
      integer(declaration.value("width"), 1, 1024, width) && integer(declaration.value("height"), 1, 1024, height);
}
bool digest(const QJsonValue &value) {
  if (!value.isString()) return false;
  const auto text = value.toString(); const auto bytes = text.toLatin1();
  return text.size() == 64 && QByteArray::fromHex(bytes).toHex() == bytes;
}
bool descriptor(const QJsonObject &port, const char *id, const char *encoding, qint64 maximum,
                std::initializer_list<const char *> names, qint64 &size) {
  return fields(port, names) && port.value("id") == QLatin1String(id) && port.value("encoding") == QLatin1String(encoding) &&
      integer(port.value("size"), 1, maximum, size) && digest(port.value("sha256"));
}
bool authenticated(QByteArrayView bytes, const QJsonObject &port) {
  return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() == port.value("sha256").toString().toLatin1();
}
class PngBuffer final : public QBuffer {
public:
  PngBuffer(QByteArray *bytes, qint64 limit, std::function<bool()> cancelled)
      : QBuffer(bytes), m_limit(limit), m_cancelled(std::move(cancelled)) {}
  QString code;
  bool seek(qint64 offset) override {
    if (offset < 0 || offset > m_limit) { exceeded(); return false; }
    return QBuffer::seek(offset);
  }
protected:
  qint64 writeData(const char *data, qint64 count) override {
    if (m_cancelled && m_cancelled()) { code = "cancelled"; return -1; }
    if (count < 0 || pos() < 0 || count > m_limit || pos() > m_limit - count) {
      exceeded(); return -1;
    }
    return QBuffer::writeData(data, count);
  }
private:
  void exceeded() { code = m_limit == ImageOutputPngBytes ? "python_png_budget_exceeded" : "python_output_budget_exceeded"; }
  qint64 m_limit;
  std::function<bool()> m_cancelled;
};
QString encodePng(QByteArrayView rgba, qint64 width, qint64 height, qint64 maximum, QByteArray &png,
                  const std::function<bool()> &cancelled) {
  if (cancelled && cancelled()) return "cancelled";
  if (maximum < 1 || maximum > ImageOutputPngBytes) return "python_output_budget_exceeded";
  QByteArray aligned;
  const uchar *pixels = reinterpret_cast<const uchar *>(rgba.data());
  if (reinterpret_cast<quintptr>(pixels) % alignof(quint32)) {
    // Variable JSON/table lengths need not align the final segment. A bounded
    // host copy supplies QImage's scanline alignment without changing pixels.
    aligned = QByteArray(rgba.data(), rgba.size()); pixels = reinterpret_cast<const uchar *>(aligned.constData());
  }
  // The const-data overload cannot detach/mutate the child's authenticated view.
  // No decoder is called: PNG is generated exclusively from frozen raw RGBA8.
  const QImage image(pixels, int(width), int(height),
                     qsizetype(width * 4), QImage::Format_RGBA8888);
  if (image.isNull() || image.sizeInBytes() != rgba.size()) return "python_image_invalid";
  PngBuffer buffer(&png, maximum, cancelled); if (!buffer.open(QIODevice::WriteOnly | QIODevice::Unbuffered)) return "python_png_encode_failed";
  // Qt's PNG compression option is 0..100, not libpng's 0..9 level.
  QImageWriter writer(&buffer, "png"); writer.setCompression(QtPngCompression);
  writer.setOptimizedWrite(false); writer.setProgressiveScanWrite(false);
  if (!writer.write(image)) { png.clear(); return buffer.code.isEmpty() ? QStringLiteral("python_png_encode_failed") : buffer.code; }
  if (png.isEmpty() || png.size() > maximum) { png.clear(); return "python_png_budget_exceeded"; }
  return {};
}
QByteArray numericFrame(const QJsonArray &ports, QByteArrayView payload) {
  const auto header = QJsonDocument(QJsonObject{{"contract", "paleo.python-output-frame.v2"},
      {"ports", QJsonArray{ports.at(0), ports.at(1)}}}).toJson(QJsonDocument::Compact);
  QByteArray out("PALEOPYO2\n"); char length[4]; qToLittleEndian<quint32>(quint32(header.size()), length);
  out.reserve(Prefix + header.size() + payload.size()); out.append(length, 4); out.append(header); out.append(payload.data(), payload.size());
  return out;
}
}
QString validateImageOutputDeclaration(const QJsonObject &declaration) {
  qint64 width = 0, height = 0;
  return dimensions(declaration, width, height) ? QString() : QStringLiteral("python_image_declaration_invalid");
}
QJsonObject pngEncoderIdentity() {
  return {{"contract", "paleo.png-encoder.v1"}, {"implementation", "qt.imagewriter.png"},
      {"qtCompileVersion", QLatin1String(QT_VERSION_STR)}, {"qtRuntimeVersion", QString::fromLatin1(qVersion())},
      {"pixelFormat", "RGBA8888"}, {"compression", QtPngCompression}, {"optimizedWrite", false}, {"progressiveScanWrite", false}};
}
qint64 imageOutputHostReservation(qint64 frameBytes, const QJsonObject &tableDeclaration,
                                 const QJsonObject &imageDeclaration) {
  qint64 width = 0, height = 0;
  if (!validateNumericOutputDeclaration(tableDeclaration).isEmpty() || !dimensions(imageDeclaration, width, height) ||
      frameBytes < Prefix || frameBytes > NumericOutputFrameBytes) return -1;
  const auto rgbaBytes = width * height * 4; // Factors were independently bounded before multiplication.
  // v3 capture/views; v2 repack/numeric copies; bounded CSV; REPORT DOM and identity
  // repacking; image conversion/scanline working copies; bounded PNG/final bytes;
  // additional Qt/libpng scratch; small headers/metadata. This is a conservative
  // host reservation, not a measurement/guarantee of the GUI's total RSS.
  return 2 * frameBytes + 2 * (Prefix + HeaderBytes + ReportBytes + TableBytes) + NumericCsvBytes +
      8 * 1024 * 1024 + 4 * rgbaBytes + 2 * ImageOutputPngBytes + 8 * 1024 * 1024 + 256 * 1024;
}
ImageOutputValidation validateImageOutput(const QByteArray &frame, const QJsonObject &tableDeclaration,
    const QJsonObject &imageDeclaration, const QJsonObject &frozenEncoder, const ExecutionPolicy &policy,
    const GeneratedReportValidator &validateReport, const std::function<bool()> &cancelled) {
  const auto fail = [](const QString &code) { ImageOutputValidation result; result.code = code; return result; };
  const auto stopped = [&] { return cancelled && cancelled(); };
  auto code = validateNumericOutputDeclaration(tableDeclaration); if (!code.isEmpty()) return fail(code);
  qint64 width = 0, height = 0;
  if (!dimensions(imageDeclaration, width, height)) return fail("python_image_declaration_invalid");
  if (frozenEncoder != pngEncoderIdentity()) return fail("python_output_encoder_changed");
  if (!policy.validate().isEmpty()) return fail("python_policy_invalid");
  if (!validateReport) return fail("python_output_validation_unavailable");
  if (stopped()) return fail("cancelled");
  if (frame.size() > qMin(NumericOutputFrameBytes, policy.outputBytes)) return fail("python_output_budget_exceeded");
  if (frame.size() < Prefix || !frame.startsWith("PALEOPYO3\n")) return fail("python_output_frame_invalid");
  const auto reservation = imageOutputHostReservation(frame.size(), tableDeclaration, imageDeclaration);
  if (reservation < 0 || reservation > policy.memoryBytes) return fail("python_output_memory_budget");
  const auto headerSize = qFromLittleEndian<quint32>(frame.constData() + 10);
  if (headerSize < 1 || headerSize > HeaderBytes || headerSize > frame.size() - Prefix) return fail("python_output_frame_invalid");
  QJsonObject header; code = parsePythonOutputObject(frame.mid(Prefix, headerSize), HeaderBytes, 4096, header);
  if (!code.isEmpty()) return fail(code);
  if (!fields(header, {"contract", "ports"}) || header.value("contract") != "paleo.python-output-frame.v3" ||
      !header.value("ports").isArray() || header.value("ports").toArray().size() != 3) return fail("python_output_contract_mismatch");
  const auto ports = header.value("ports").toArray();
  for (const auto &port : ports) if (!port.isObject()) return fail("python_output_contract_mismatch");
  const auto reportPort = ports.at(0).toObject(), tablePort = ports.at(1).toObject(), imagePort = ports.at(2).toObject();
  qint64 reportSize = 0, tableSize = 0, imageSize = 0;
  const auto expectedImageSize = width * height * 4;
  if (!descriptor(reportPort, "REPORT", "utf8-json", ReportBytes, {"id", "encoding", "size", "sha256"}, reportSize) ||
      !descriptor(tablePort, "TABLE", "paleo.numeric-columns.v1", TableBytes, {"id", "encoding", "size", "sha256", "rows", "columns"}, tableSize) ||
      !descriptor(imagePort, "IMAGE", "rgba8", RgbaBytes, {"id", "encoding", "size", "sha256", "width", "height"}, imageSize) ||
      imagePort.value("width") != imageDeclaration.value("width") || imagePort.value("height") != imageDeclaration.value("height") ||
      imageSize != expectedImageSize || reportSize + tableSize + imageSize != frame.size() - Prefix - headerSize)
    return fail("python_output_contract_mismatch");
  const QByteArrayView report(frame.constData() + Prefix + headerSize, reportSize);
  const QByteArrayView table(frame.constData() + Prefix + headerSize + reportSize, tableSize);
  const QByteArrayView image(frame.constData() + Prefix + headerSize + reportSize + tableSize, imageSize);
  if (!authenticated(report, reportPort) || !authenticated(table, tablePort) || !authenticated(image, imagePort))
    return fail("python_output_integrity_failed");
  if (stopped()) return fail("cancelled");
  const auto numeric = validateNumericOutput(numericFrame(ports, {report.data(), reportSize + tableSize}),
      tableDeclaration, policy, validateReport, cancelled);
  if (!numeric.ok()) return fail(numeric.code);
  const auto groupBudget = qMin(NumericOutputFrameBytes, policy.outputBytes);
  const auto pngBudget = qMin(ImageOutputPngBytes, groupBudget - numeric.report.size() - numeric.csv.size());
  QByteArray png; code = encodePng(image, width, height, pngBudget, png, cancelled); if (!code.isEmpty()) return fail(code);
  if (numeric.report.size() + numeric.csv.size() + png.size() > groupBudget) return fail("python_output_budget_exceeded");
  if (stopped()) return fail("cancelled");
  ImageOutputValidation result; result.report = numeric.report; result.csv = numeric.csv; result.png = std::move(png);
  result.tableMetadata = numeric.tableMetadata;
  result.imageMetadata = {{"contract", "paleo.image-output.v1"}, {"width", width}, {"height", height},
      {"encoding", "host-png.rgba8.v1"}, {"spatial", false}, {"alpha", "straight"},
      {"claims", "agent_generated_pixels_not_independently_recomputed"},
      {"provenance", QJsonObject{{"encoder", frozenEncoder}, {"sourceRgbaSha256", imagePort.value("sha256")},
          {"declaration", imageDeclaration}}}};
  result.summary = numeric.summary; result.summary["imageWidth"] = width; result.summary["imageHeight"] = height;
  result.summary["pngBytes"] = result.png.size(); result.summary["pngEncoder"] = frozenEncoder;
  result.hostReservedBytes = reservation; result.summary["hostOutputReservedBytes"] = QString::number(reservation);
  result.code = "ok"; return result;
}
}
