// 层：数据
#include "pythonnumericoutput_internal.h"
#include <QtEndian>
#include <cmath>
#include <cstring>

namespace paleo::python::numeric_output {
namespace {
struct Column {
  QString dtype, role;
  qsizetype values = 0, nulls = 0;
};
bool missing(QByteArrayView bytes, const Column &column, qsizetype row) {
  return (quint8(bytes[column.nulls + row / 8]) & (quint8(1) << (row % 8))) != 0;
}
QString value(QByteArrayView bytes, const Column &column, qsizetype row, QByteArray &encoded, qint64 &previousRow) {
  const bool null = missing(bytes, column, row);
  if (column.role != "value" && null) return "python_output_axis_invalid";
  const auto offset = column.values + row * (column.dtype == "|b1" ? 1 : 8);
  if (column.dtype == "|b1") {
    const auto byte = quint8(bytes[offset]);
    if (byte > 1 || (null && byte)) return "python_output_value_invalid";
    encoded = null ? QByteArray() : byte ? QByteArray("true") : QByteArray("false"); return {};
  }
  const auto bits = qFromLittleEndian<quint64>(bytes.data() + offset);
  if (column.dtype == "<i8") {
    const auto integer = qFromLittleEndian<qint64>(bytes.data() + offset);
    if (integer < 1 || integer <= previousRow) return "python_output_axis_invalid";
    previousRow = integer; encoded = QByteArray::number(integer); return {};
  }
  double number; static_assert(sizeof(number) == sizeof(bits)); std::memcpy(&number, &bits, sizeof(number));
  if (!std::isfinite(number) || (null && bits != 0)) return "python_output_value_invalid";
  encoded = null ? QByteArray() : QByteArray::number(number, 'g', 17); return {};
}
}
QString encodeCsv(QByteArrayView payload, qint64 rows, const QJsonArray &columns, QByteArray &csv,
                  const std::function<bool()> &cancelled) {
  csv.clear(); QVector<Column> layout; qint64 bytes = 0;
  const auto nullBytes = (rows + 7) / 8;
  for (const auto &entry : columns) {
    const auto declaration = entry.toObject(); Column column;
    column.dtype = declaration.value("dtype").toString(); column.role = declaration.value("role").toString();
    column.values = bytes; bytes += rows * (column.dtype == "|b1" ? 1 : 8); column.nulls = bytes; bytes += nullBytes;
    if (bytes > payload.size()) return "python_output_length_invalid";
    if (rows % 8 && (quint8(payload[column.nulls + nullBytes - 1]) >> (rows % 8))) return "python_output_null_mask_invalid";
    layout.append(column);
  }
  if (bytes != payload.size()) return "python_output_length_invalid";
  const auto reservation = rows * (columns.size() * 28 + 2) + columns.size() * 65 + 2;
  if (reservation > NumericCsvBytes) return "python_output_budget_exceeded";
  QByteArray output; output.reserve(reservation);
  for (qsizetype index = 0; index < columns.size(); ++index) {
    if (index) output += ',';
    output += columns[index].toObject().value("id").toString().toLatin1();
  }
  output += "\r\n"; qint64 previousRow = 0;
  for (qsizetype row = 0; row < rows; ++row) {
    if (cancelled && cancelled()) return "cancelled";
    for (qsizetype column = 0; column < layout.size(); ++column) {
      if (column) output += ',';
      QByteArray encoded; const auto code = value(payload, layout[column], row, encoded, previousRow);
      if (!code.isEmpty()) return code;
      output += encoded;
    }
    output += "\r\n";
    if (output.size() > NumericCsvBytes) return "python_output_budget_exceeded";
  }
  csv = std::move(output); return {};
}
}
