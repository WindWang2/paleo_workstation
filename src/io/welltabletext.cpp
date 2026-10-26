// 层：数据
#include "boundedread.h" // PW-076
#include "welltabletext.h"
#include "encodingdetect.h"
#include "sourceprobe.h"
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace paleo::io {
namespace {
QStringList fields(const QString &line, QChar delimiter)
{
  if (delimiter.isNull()) return line.simplified().split(' ', Qt::SkipEmptyParts);
  QStringList out; QString cell; bool quote = false;
  for (int i = 0; i < line.size(); ++i) {
    const QChar c = line[i];
    if (c == '"') { if (quote && i + 1 < line.size() && line[i + 1] == '"') { cell += '"'; ++i; } else quote = !quote; }
    else if (c == delimiter && !quote) { out << cell.trimmed(); cell.clear(); }
    else cell += c;
  }
  out << cell.trimmed(); return out;
}
WorkbookReadResult parseDelimited(const QString &path, const QByteArray &raw, int rowLimit, bool *limited)
{
  WorkbookReadResult out;
  const auto text = EncodingDetect::decodeText(raw);
  WorkbookSheet sheet; sheet.name = QFileInfo(path).completeBaseName(); QChar delimiter;
  int row = 0;
  for (const auto &line : text.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts)) {
    ++row; if (line.trimmed().isEmpty() || line.trimmed().startsWith('#')) continue;
    if (rowLimit >= 0 && sheet.rows.size() >= rowLimit) { if (limited) *limited = true; break; }
    if (sheet.headers.isEmpty()) {
      delimiter = line.contains('\t') ? QChar('\t') : line.contains(',') ? QChar(',') : line.contains(';') ? QChar(';') : QChar();
      sheet.headers = fields(line, delimiter); sheet.headerRowNumber = row;
      if (sheet.headers.size() >= 3) {
        bool x = false, y = false; sheet.headers[1].toDouble(&x); sheet.headers[2].toDouble(&y);
        if (x && y) { out.format = QStringLiteral("headerless"); sheet.rows << sheet.headers; sheet.rowNumbers << row; sheet.headers = {QStringLiteral("井名"), "X", "Y", "KB", "TD"}; }
      }
    } else { sheet.rows << fields(line, delimiter); sheet.rowNumbers << row; }
  }
  out.sheets << sheet; out.ok = !sheet.headers.isEmpty();
  if (!out.ok) out.error = QStringLiteral("井位文本没有可用表头");
  return out;
}
}
WorkbookReadResult readWellDelimitedTable(const QString &path)
{
  WorkbookReadResult out; QByteArray raw;
  if (!readBoundedFile(path, 64ll * 1024 * 1024, &raw, &out.error)) {
    if (out.error.startsWith(QLatin1String("file too large"))) out.error = QStringLiteral("井位文本超过 64 MB，请拆分后导入");
    return out;
  }
  return parseDelimited(path, raw, -1, nullptr);
}
WorkbookReadResult previewWellDelimitedTable(const QString &path, bool *limited)
{
  *limited = false; QFile file(path); WorkbookReadResult out;
  if (!file.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) { out.error = file.errorString(); return out; }
  auto raw = file.read(kScanPrefixBytes); *limited = !file.atEnd();
  if (*limited) raw.truncate(qMax(0, raw.lastIndexOf('\n'))); // Never invent a row from a cut prefix.
  return parseDelimited(path, raw, 200, limited);
}
}
