// 层：数据
#include "sourceprobe.h"
#include "dlisparser.h"
#include "lisparser.h"
#include "lasparser.h"
#include "encodingdetect.h"
#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QXmlStreamReader>

namespace paleo::io {
namespace {
QStringList xmlRow(QXmlStreamReader &xml)
{
  QStringList cells;
  while (!xml.atEnd()) {
    xml.readNext();
    if (xml.isEndElement() && xml.name() == QLatin1String("Row")) break;
    if (!xml.isStartElement() || xml.name() != QLatin1String("Cell")) continue;
    int column = cells.size();
    for (const auto &a : xml.attributes()) if (a.name() == QLatin1String("Index")) {
      bool ok = false; const int index = a.value().toInt(&ok);
      if (!ok || index < 1 || index > 256) { xml.skipCurrentElement(); return {}; }
      column = index - 1;
    }
    if (column >= 256) return {};
    while (cells.size() <= column) cells << QString();
    while (!xml.atEnd()) {
      xml.readNext();
      if (xml.isEndElement() && xml.name() == QLatin1String("Cell")) break;
      if (xml.isStartElement() && xml.name() == QLatin1String("Data"))
        cells[column] = xml.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
    }
  }
  return cells;
}
void xmlIdentity(const QByteArray &prefix, SourceProbe &out)
{
  QXmlStreamReader xml(prefix);
  QString sheet; QStringList headers; bool curves = false;
  while (!xml.atEnd()) {
    xml.readNext();
    if (!xml.isStartElement()) continue;
    if (xml.name() == QLatin1String("Worksheet")) {
      sheet.clear(); headers.clear();
      for (const auto &a : xml.attributes()) if (a.name() == QLatin1String("Name")) sheet = a.value().toString().trimmed();
      curves = sheet.startsWith(QStringLiteral("测井曲线")) || sheet.startsWith(QStringLiteral("离散曲线"));
      if (curves) {
        out.classification = {"well_log", "xml", "input"};
        static const QRegularExpression named(QStringLiteral("^(?:测井曲线|离散曲线)-(.+)井$"));
        const auto match = named.match(sheet);
        if (match.hasMatch()) { out.wellName = match.captured(1).trimmed(); return; }
      }
    } else if (curves && xml.name() == QLatin1String("Row")) {
      const auto row = xmlRow(xml);
      if (headers.isEmpty()) { if (!row.join(QString()).trimmed().isEmpty()) headers = row; continue; }
      for (int c = 0; c < headers.size(); ++c)
        if (QStringList{"井号", "井名", "WELL", "WELLNAME"}.contains(headers[c].trimmed(), Qt::CaseInsensitive) && !row.value(c).isEmpty()) {
          out.wellName = row[c]; return;
        }
    }
  }
  // An incomplete prefix is expected: absence of </Workbook> is not validation.
}
void identify(const QByteArray &prefix, const QString &path, SourceProbe &out)
{
  out.classification = classifyProjectImport(path, prefix);
  QBuffer buffer; buffer.setData(prefix); buffer.open(QIODevice::ReadOnly);
  LasHeaderInfo header;
  if (DlisParser::sniff(buffer)) {
    out.classification = {"well_log", "dlis", "input"}; buffer.seek(0);
    if (DlisParser::parseHeader(buffer, header)) out.wellName = header.wellName;
    return;
  }
  buffer.seek(0);
  if (LisParser::sniff(buffer) || out.classification.format == "lis") {
    out.classification = {"well_log", "lis", "input"}; buffer.seek(0);
    if (LisParser::parseHeader(buffer, header)) out.wellName = header.wellName;
    return;
  }
  QXmlStreamReader xml(prefix);
  if (xml.readNextStartElement() && xml.name() == QLatin1String("Workbook") &&
      xml.namespaceUri() == QLatin1String("urn:schemas-microsoft-com:office:spreadsheet")) {
    xmlIdentity(prefix, out); return;
  }
  if (out.classification.type != QLatin1String("well_log")) return;
  if (out.classification.format == QLatin1String("las")) {
    LasParser::SectionMap sections;
    LasParser::scanSections(prefix, &sections, nullptr); out.wellName = sections.header.wellName; return;
  }
  // Delimited curves declare identity in a comment; values remain unread.
  static const QRegularExpression well(QStringLiteral("^\\s*#\\s*(?:WELL|井号|井名)\\s*[:=]\\s*(.+)$"), QRegularExpression::CaseInsensitiveOption);
  for (const auto &line : EncodingDetect::decodeText(prefix).split('\n')) {
    const auto match = well.match(line);
    if (match.hasMatch()) { out.wellName = match.captured(1).trimmed(); break; }
    if (!line.trimmed().isEmpty() && !line.trimmed().startsWith('#')) break;
  }
}
}
SourceProbe probeSource(QIODevice &source, const QString &path, const std::function<bool()> &cancelled)
{
  SourceProbe out; out.classification = classifyProjectPath(path);
  QByteArray prefix;
  while (prefix.size() < kScanPrefixBytes) {
    if (cancelled && cancelled()) { out.cancelled = true; return out; }
    const auto chunk = source.read(qMin<qint64>(16 * 1024, kScanPrefixBytes - prefix.size()));
    if (chunk.isEmpty()) break;
    prefix += chunk; out.bytesRead += chunk.size();
    identify(prefix, path, out);
    if (!out.wellName.isEmpty() || source.atEnd()) break;
    if (!prefix.trimmed().startsWith('<') && !prefix.startsWith("\xEF\xBB\xBF") && out.classification.format != "xml" &&
        out.classification.type != QLatin1String("well_log")) break;
    if (out.classification.type == QLatin1String("well_log") &&
        out.classification.format != "xml" && out.classification.format != "dlis" && out.classification.format != "lis") break;
  }
  out.limited = !source.atEnd();
  out.prefixSha256 = QString::fromLatin1(QCryptographicHash::hash(prefix, QCryptographicHash::Sha256).toHex());
  return out;
}
SourceProbe probeSource(const QString &path, const std::function<bool()> &cancelled)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
    SourceProbe out; out.classification = classifyProjectPath(path); out.error = file.errorString(); return out;
  }
  return probeSource(file, path, cancelled);
}
QVariantMap sourceSnapshot(const QString &path, const SourceProbe &probe)
{
  const QFileInfo file(path);
  return {{"canonical", file.canonicalFilePath()}, {"size", file.size()},
      {"modified", file.lastModified().toMSecsSinceEpoch()},
      {"prefixBytes", probe.bytesRead}, {"prefixSha256", probe.prefixSha256}};
}
bool matchesSourceSnapshot(const QString &path, const QVariantMap &snapshot)
{
  const QFileInfo file(path); const auto bytes = snapshot.value("prefixBytes").toLongLong();
  if (!file.isFile() || file.canonicalFilePath() != snapshot.value("canonical").toString() ||
      file.size() != snapshot.value("size").toLongLong() ||
      file.lastModified().toMSecsSinceEpoch() != snapshot.value("modified").toLongLong() ||
      bytes < 0 || bytes > kScanPrefixBytes) return false;
  QFile input(path); if (!input.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) return false;
  const auto prefix = input.read(bytes);
  return prefix.size() == bytes && QString::fromLatin1(QCryptographicHash::hash(prefix,
      QCryptographicHash::Sha256).toHex()) == snapshot.value("prefixSha256").toString();
}
}
