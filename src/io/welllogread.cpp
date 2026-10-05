// 层：数据
#include "welllogread.h"

#include "dlisparser.h"
#include "lisparser.h"

#include <QFileInfo>

WellLogFormat WellLogRead::detect(const QString &path)
{
  // 内容嗅探优先：SUL/磁带标头是格式自声明，扩展名只是弱信号。
  if (DlisParser::sniff(path))
    return WellLogFormat::Dlis;
  if (LisParser::sniff(path))
    return WellLogFormat::Lis;
  const QString ext = QFileInfo(path).suffix().toLower();
  if (ext == QLatin1String("dlis"))
    return WellLogFormat::Dlis;
  if (ext == QLatin1String("lis"))
    return WellLogFormat::Lis;
  return WellLogFormat::Las;
}

QString WellLogRead::formatTag(WellLogFormat f)
{
  switch (f)
  {
    case WellLogFormat::Dlis: return QStringLiteral("dlis");
    case WellLogFormat::Lis: return QStringLiteral("lis");
    case WellLogFormat::Las: break;
  }
  return QStringLiteral("las");
}

bool WellLogRead::readWellInfo(const QString &path, QString &wellName,
                               QString *error)
{
  if (error)
    error->clear();
  wellName.clear();
  switch (detect(path))
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
  }
  return LasParser::readWellInfo(path, wellName, error);
}

bool WellLogRead::parseHeader(const QString &path, LasHeaderInfo &out,
                              QString *error, QList<LasIssue> *issues)
{
  if (error)
    error->clear();
  switch (detect(path))
  {
    case WellLogFormat::Dlis:
      return DlisParser::parseHeader(path, out, error, issues);
    case WellLogFormat::Lis:
      return LisParser::parseHeader(path, out, error, issues);
    case WellLogFormat::Las:
      break;
  }
  return LasParser::parseHeader(path, out, error);
}

bool WellLogRead::parseCurves(const QString &path, QStringList &curveNames,
                              QList<LasCurve> &curves, QString *error,
                              QList<LasIssue> *issues)
{
  if (error)
    error->clear();
  curveNames.clear();
  curves.clear();
  switch (detect(path))
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
  }
  return LasParser::parse(path, curveNames, curves, error);
}

LasDoc WellLogRead::parseDoc(const QString &path, QList<LasIssue> *issues)
{
  switch (detect(path))
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
  }
  return LasParser::parseDoc(path, issues);
}
