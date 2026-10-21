// 层：数据
#include "welllogread.h"

#include "dlisparser.h"
#include "lisparser.h"
#include "lasparser.h" // LAS 路径分派目标（解析入口）
#include "welllogtext.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <utility>

namespace
{
bool parseAdditional(const QString &path, WellLogFormat format, LasHeaderInfo &header,
                     QList<LasCurve> &curves, QString *error, QList<LasIssue> *issues)
{
  if (format != WellLogFormat::Unknown)
    return WellLogText::parse(path, format == WellLogFormat::SpreadsheetXml, header, curves, error, issues);
  header = {};
  curves.clear();
  QFile file(path);
  const QString detail = !file.open(QIODevice::ReadOnly) ? file.errorString() :
      QCoreApplication::translate("WellLogRead", "无法识别测井格式。请提供 LAS/DLIS/LIS、综合柱状图 XML，或带明确索引表头的 CSV/TXT/TSV。");
  const QString message = QCoreApplication::translate("WellLogRead", "%1：%2").arg(path, detail);
  if (error) *error = message;
  if (issues) issues->append({LasIssue::Severity::Error, LasIssue::Category::Format, 0, message});
  return false;
}
}

WellLogFormat WellLogRead::detect(const QString &path)
{
  // 内容嗅探优先：SUL/磁带标头是格式自声明，扩展名只是弱信号。
  if (DlisParser::sniff(path))
    return WellLogFormat::Dlis;
  if (LisParser::sniff(path))
    return WellLogFormat::Lis;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return WellLogFormat::Unknown;
  const QByteArray content = file.read(64 * 1024);
  if (WellLogText::sniffSpreadsheetXml(content)) return WellLogFormat::SpreadsheetXml;
  QString text = QString::fromUtf8(content);
  if (text.startsWith(QChar(0xfeff))) text.remove(0, 1);
  if (text.trimmed().startsWith(QLatin1Char('<'))) return WellLogFormat::Unknown;
  static const QRegularExpression lasHeader(QStringLiteral("(?:^|[\\r\\n])\\s*~[VWCAPO]"), QRegularExpression::CaseInsensitiveOption);
  if (lasHeader.match(text).hasMatch()) return WellLogFormat::Las;
  if (WellLogText::sniffText(content)) return WellLogFormat::Text;
  const QString ext = QFileInfo(path).suffix().toLower();
  if (ext == QLatin1String("dlis"))
    return WellLogFormat::Dlis;
  if (ext == QLatin1String("lis"))
    return WellLogFormat::Lis;
  if (ext == QLatin1String("las")) return WellLogFormat::Las;
  if (ext == QLatin1String("csv") || ext == QLatin1String("txt") || ext == QLatin1String("tsv")) return WellLogFormat::Text;
  return WellLogFormat::Unknown;
}

QString WellLogRead::formatTag(WellLogFormat f)
{
  switch (f)
  {
    case WellLogFormat::Dlis: return QStringLiteral("dlis");
    case WellLogFormat::Lis: return QStringLiteral("lis");
    case WellLogFormat::Las: break;
    case WellLogFormat::SpreadsheetXml: return QStringLiteral("spreadsheetml");
    case WellLogFormat::Text: return QStringLiteral("text");
    case WellLogFormat::Unknown: return QStringLiteral("unknown");
  }
  return QStringLiteral("las");
}

bool WellLogRead::readWellInfo(const QString &path, QString &wellName,
                               QString *error)
{
  if (error)
    error->clear();
  wellName.clear();
  const auto format = detect(path);
  switch (format)
  {
    case WellLogFormat::Dlis:
    {
      LasHeaderInfo header;
      QList<LasIssue> issues;
      QString err;
      // 头扫描拿 ORIGIN.WELL-NAME：只走到首个 FDATA，代价与头部成正比
      if (!DlisParser::parseHeader(path, header, &err))
      {
        // 读不出头（截断/坏段）：井名留空走未决链接，错误如实带出
        if (error)
          *error = err;
        return false;
      }
      wellName = header.wellName;
      return true;
    }
    case WellLogFormat::Lis:
    {
      LasHeaderInfo header;
      QString err;
      if (!LisParser::parseHeader(path, header, &err))
      {
        if (error)
          *error = err;
        return false;
      }
      wellName = header.wellName;
      return true;
    }
    case WellLogFormat::Las:
      break;
    case WellLogFormat::SpreadsheetXml:
    case WellLogFormat::Text:
    case WellLogFormat::Unknown:
    {
      LasHeaderInfo header;
      QList<LasCurve> curves;
      if (!parseAdditional(path, format, header, curves, error, nullptr)) return false;
      wellName = header.wellName;
      return true;
    }
  }
  return LasParser::readWellInfo(path, wellName, error);
}

bool WellLogRead::parseHeader(const QString &path, LasHeaderInfo &out,
                              QString *error, QList<LasIssue> *issues)
{
  if (error)
    error->clear();
  const auto format = detect(path);
  switch (format)
  {
    case WellLogFormat::Dlis:
      return DlisParser::parseHeader(path, out, error, issues);
    case WellLogFormat::Lis:
      return LisParser::parseHeader(path, out, error, issues);
    case WellLogFormat::Las:
      break;
    case WellLogFormat::SpreadsheetXml:
    case WellLogFormat::Text:
    case WellLogFormat::Unknown:
    {
      QList<LasCurve> curves;
      return parseAdditional(path, format, out, curves, error, issues);
    }
  }
  return LasParser::parseHeader(path, out, error);
}

bool WellLogRead::parse(const QString &path, LasHeaderInfo &header,
                        QList<LasCurve> &curves, QString *error,
                        QList<LasIssue> *issues)
{
  header = {};
  curves.clear();
  if (error) error->clear();
  LasHeaderInfo parsedHeader;
  QList<LasCurve> parsedCurves;
  bool ok = false;
  const auto format = detect(path);
  switch (format)
  {
    case WellLogFormat::Dlis:
      ok = DlisParser::parse(path, parsedHeader, parsedCurves, error, issues);
      break;
    case WellLogFormat::Lis:
      ok = LisParser::parse(path, parsedHeader, parsedCurves, error, issues);
      break;
    case WellLogFormat::Las:
    {
      if (!LasParser::parseHeader(path, parsedHeader, error)) return false;
      LasDoc doc = LasParser::parseDoc(path, issues);
      if (!doc.ok)
      {
        if (error) *error = doc.error;
        return false;
      }
      parsedCurves = std::move(doc.curves);
      ok = true;
      break;
    }
    case WellLogFormat::SpreadsheetXml:
    case WellLogFormat::Text:
    case WellLogFormat::Unknown:
      ok = parseAdditional(path, format, parsedHeader, parsedCurves, error, issues);
      break;
  }
  if (!ok) return false;
  header = std::move(parsedHeader);
  curves = std::move(parsedCurves);
  return true;
}

bool WellLogRead::parseCurves(const QString &path, QStringList &curveNames,
                              QList<LasCurve> &curves, QString *error,
                              QList<LasIssue> *issues)
{
  if (error)
    error->clear();
  curveNames.clear();
  curves.clear();
  const auto format = detect(path);
  switch (format)
  {
    case WellLogFormat::Dlis:
    {
      LasHeaderInfo header;
      if (!DlisParser::parse(path, header, curves, error, issues))
        return false;
      curveNames = header.curveNames;
      return true;
    }
    case WellLogFormat::Lis:
    {
      LasHeaderInfo header;
      if (!LisParser::parse(path, header, curves, error, issues))
        return false;
      curveNames = header.curveNames;
      return true;
    }
    case WellLogFormat::Las:
      break;
    case WellLogFormat::SpreadsheetXml:
    case WellLogFormat::Text:
    case WellLogFormat::Unknown:
    {
      LasHeaderInfo header;
      if (!parseAdditional(path, format, header, curves, error, issues)) return false;
      curveNames = header.curveNames;
      return true;
    }
  }
  return LasParser::parse(path, curveNames, curves, error);
}

LasDoc WellLogRead::parseDoc(const QString &path, QList<LasIssue> *issues)
{
  const auto format = detect(path);
  switch (format)
  {
    case WellLogFormat::Dlis:
    {
      LasHeaderInfo header;
      QList<LasCurve> curves;
      QString error;
      const bool ok = DlisParser::parse(path, header, curves, &error, issues);
      LasDoc doc;
      doc.ok = ok;
      doc.error = error;
      doc.curveNames = header.curveNames;
      doc.curves = curves;
      return doc;
    }
    case WellLogFormat::Lis:
    {
      LasHeaderInfo header;
      QList<LasCurve> curves;
      QString error;
      const bool ok = LisParser::parse(path, header, curves, &error, issues);
      LasDoc doc;
      doc.ok = ok;
      doc.error = error;
      doc.curveNames = header.curveNames;
      doc.curves = curves;
      return doc;
    }
    case WellLogFormat::Las:
      break;
    case WellLogFormat::SpreadsheetXml:
    case WellLogFormat::Text:
    case WellLogFormat::Unknown:
    {
      LasDoc doc;
      LasHeaderInfo header;
      doc.ok = parseAdditional(path, format, header, doc.curves, &doc.error, issues);
      doc.curveNames = header.curveNames;
      return doc;
    }
  }
  return LasParser::parseDoc(path, issues);
}
