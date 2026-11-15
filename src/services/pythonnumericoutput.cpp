// 层：数据
#include "pythonnumericoutput_internal.h"
#include "pythonoutputjson.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QtEndian>
#include <cmath>

namespace paleo::python::numeric_output {
bool fields(const QJsonObject &value, std::initializer_list<const char *> names) {
  if (value.size() != qsizetype(names.size())) return false;
  for (const auto *name : names) if (!value.contains(QLatin1String(name))) return false;
  return true;
}
bool integer(const QJsonValue &value, qint64 minimum, qint64 maximum, qint64 &out) {
  if (!value.isDouble()) return false;
  const auto number = value.toDouble();
  if (!std::isfinite(number) || number != std::floor(number) || number < double(minimum) || number > double(maximum)) return false;
  out = qint64(number); return true;
}
bool boundedText(const QJsonValue &value, int maximum) {
  if (!value.isString()) return false;
  const auto text = value.toString(); const auto encoded = text.toUtf8();
  if (text.trimmed().isEmpty() || text.size() > maximum || encoded.size() > maximum || QString::fromUtf8(encoded) != text) return false;
  for (const auto character : text) if (character.unicode() < 32 || character.unicode() == 127) return false;
  return true;
}
}
namespace paleo::python {
using namespace numeric_output;
QString validateNumericOutputDeclaration(const QJsonObject &declaration) {
  qint64 rows = 0;
  if (!fields(declaration, {"contract", "columns", "maxRows"}) ||
      declaration.value("contract") != "paleo.python-table-declaration.v1" ||
      !integer(declaration.value("maxRows"), 1, 8192, rows) || !declaration.value("columns").isArray())
    return "python_output_declaration_invalid";
  const auto columns = declaration.value("columns").toArray();
  if (columns.isEmpty() || columns.size() > 16) return "python_output_declaration_invalid";
  static const QRegularExpression id("^[A-Za-z][A-Za-z0-9_]{0,63}$");
  QSet<QString> ids;
  for (qsizetype index = 0; index < columns.size(); ++index) {
    if (!columns[index].isObject()) return "python_output_declaration_invalid";
    const auto column = columns[index].toObject(); const auto name = column.value("id").toString();
    const auto dtype = column.value("dtype").toString(), role = column.value("role").toString();
    const auto identity = id.match(name);
    if (!fields(column, {"id", "label", "unit", "dtype", "role"}) || !identity.hasMatch() || identity.capturedLength() != name.size() ||
        ids.contains(name) || !boundedText(column.value("label"), 256) || !boundedText(column.value("unit"), 64))
      return "python_output_declaration_invalid";
    const auto unit = column.value("unit").toString().trimmed().toCaseFolded();
    if (QStringList{"unknown", "unspecified", "undefined", "?", "n/a"}.contains(unit) ||
        (dtype == "|b1" && column.value("unit") != "1")) return "python_output_declaration_invalid";
    if (index == 0) {
      if (role == "source_row") {
        if (name != "source_row" || dtype != "<i8" || column.value("unit") != "1") return "python_output_declaration_invalid";
      } else if (role != "axis" || dtype != "<f8") return "python_output_declaration_invalid";
    } else if (role != "value" || (dtype != "<f8" && dtype != "|b1")) return "python_output_declaration_invalid";
    ids.insert(name);
  }
  return {};
}
namespace {
bool descriptor(const QJsonObject &port, const char *id, const char *encoding, qint64 maximum,
                bool table, qint64 &size) {
  static const QRegularExpression digest("^[0-9a-f]{64}$");
  const bool shape = table ? fields(port, {"id", "encoding", "size", "sha256", "rows", "columns"}) :
      fields(port, {"id", "encoding", "size", "sha256"});
  return shape && port.value("id") == QLatin1String(id) && port.value("encoding") == QLatin1String(encoding) &&
      integer(port.value("size"), 1, maximum, size) && port.value("sha256").toString().size() == 64 &&
      digest.match(port.value("sha256").toString()).hasMatch();
}
bool authenticated(QByteArrayView bytes, const QJsonObject &port) {
  return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() == port.value("sha256").toString().toLatin1();
}
}
NumericOutputValidation validateNumericOutput(const QByteArray &frame, const QJsonObject &declaration,
    const ExecutionPolicy &policy, const GeneratedReportValidator &validateReport, const std::function<bool()> &cancelled) {
  const auto fail = [](const QString &code) { NumericOutputValidation result; result.code = code; return result; };
  const auto stopped = [&] { return cancelled && cancelled(); };
  auto code = validateNumericOutputDeclaration(declaration);
  if (!code.isEmpty()) return fail(code);
  if (!policy.validate().isEmpty()) return fail("python_policy_invalid");
  if (!validateReport) return fail("python_output_validation_unavailable");
  if (stopped()) return fail("cancelled");
  constexpr qsizetype prefix = 14; // ten magic bytes followed by uint32 little endian.
  if (frame.size() > qMin(NumericOutputFrameBytes, policy.outputBytes)) return fail("python_output_budget_exceeded");
  if (frame.size() < prefix || !frame.startsWith("PALEOPYO2\n")) return fail("python_output_frame_invalid");
  const auto columns = declaration.value("columns").toArray();
  const qint64 csvReservation = qint64(declaration.value("maxRows").toInt()) * (columns.size() * 28 + 2) + columns.size() * 65 + 2;
  // Bound the retained stream/copies, CSV, report DOM/identity repacking and
  // small manifest structures independently of the child's OS memory counter.
  const qint64 reservation = 2 * frame.size() + csvReservation + 8 * 1024 * 1024 + 128 * 1024;
  if (reservation > policy.memoryBytes) return fail("python_output_memory_budget");
  const auto headerSize = qFromLittleEndian<quint32>(frame.constData() + 10);
  if (headerSize < 1 || headerSize > 16 * 1024 || headerSize > frame.size() - prefix) return fail("python_output_frame_invalid");
  QJsonObject header;
  code = parsePythonOutputObject(frame.mid(prefix, headerSize), 16 * 1024, 4096, header);
  if (!code.isEmpty()) return fail(code);
  if (!fields(header, {"contract", "ports"}) || header.value("contract") != "paleo.python-output-frame.v2" ||
      !header.value("ports").isArray() || header.value("ports").toArray().size() != 2) return fail("python_output_contract_mismatch");
  const auto ports = header.value("ports").toArray();
  if (!ports[0].isObject() || !ports[1].isObject()) return fail("python_output_contract_mismatch");
  const auto reportPort = ports[0].toObject(), tablePort = ports[1].toObject();
  qint64 reportSize = 0, tableSize = 0, rows = 0;
  if (!descriptor(reportPort, "REPORT", "utf8-json", 256 * 1024, false, reportSize) ||
      !descriptor(tablePort, "TABLE", "paleo.numeric-columns.v1", 2 * 1024 * 1024, true, tableSize) ||
      !integer(tablePort.value("rows"), 1, declaration.value("maxRows").toInt(), rows) ||
      tablePort.value("columns") != declaration.value("columns") ||
      reportSize + tableSize != frame.size() - prefix - headerSize) return fail("python_output_contract_mismatch");
  const QByteArrayView reportBytes(frame.constData() + prefix + headerSize, reportSize);
  const QByteArrayView tableBytes(frame.constData() + prefix + headerSize + reportSize, tableSize);
  if (!authenticated(reportBytes, reportPort) || !authenticated(tableBytes, tablePort)) return fail("python_output_integrity_failed");
  if (stopped()) return fail("cancelled");
  const auto report = validateReport(QByteArray(reportBytes.data(), reportBytes.size()));
  if (!report.ok()) return fail(report.code);
  if (report.bytes.isEmpty() || report.bytes.size() > qMin<qint64>(256 * 1024, policy.outputBytes)) return fail("python_output_budget_exceeded");
  QByteArray csv; code = encodeCsv(tableBytes, rows, declaration.value("columns").toArray(), csv, cancelled);
  if (!code.isEmpty()) return fail(code);
  if (stopped()) return fail("cancelled");
  NumericOutputValidation result; result.report = report.bytes; result.csv = std::move(csv);
  result.tableMetadata = {{"contract", "paleo.numeric-table-output.v1"}, {"rows", rows},
      {"columns", declaration.value("columns")}, {"encoding", "host-csv.numeric.v1"},
      {"claims", "agent_generated_values_not_independently_recomputed"}};
  result.summary = report.summary; result.summary["tableRows"] = rows;
  result.summary["tableColumns"] = declaration.value("columns").toArray().size();
  result.summary["csvBytes"] = result.csv.size(); result.hostReservedBytes = reservation;
  result.summary["hostOutputReservedBytes"] = QString::number(reservation); result.code = "ok"; return result;
}
}
