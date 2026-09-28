// 层：数据
#include "wellrecords.h"

#include <QSet>
#include <QXmlStreamReader>

// 自 io/wellfileparsers.cpp 下沉（domain 纯函数——分类器的 XML 分流用）。
WellXmlKind sniffWellXml(const QByteArray &content)
{
  QXmlStreamReader xml(content);
  bool hasLogElement = false, hasCurveInfo = false, hasLogData = false;
  bool hasNamedSheet = false, rootSeen = false;
  bool rootIsWitsml = false;
  QString rootTag;
  QSet<QString> tags;
  static const QSet<QString> kSheetNames{
      QStringLiteral("测井曲线"), QStringLiteral("welllog"),
      QStringLiteral("well log"), QStringLiteral("log curves")};

  int elements = 0;
  while (!xml.atEnd() && elements < 100000)
  {
    xml.readNext();
    if (xml.isStartElement())
    {
      ++elements;
      const QString local = xml.name().toString().toLower();
      if (!rootSeen)
      {
        rootSeen = true;
        rootTag = local;
        // 参考实现按原始标签子串判 "witsml"——命名空间展开后等价于
        // 前缀或 URI 含 witsml。
        rootIsWitsml = local.contains(QLatin1String("witsml")) ||
                       xml.prefix().toString().contains(QLatin1String("witsml")) ||
                       xml.namespaceUri().toString().contains(QLatin1String("witsml"));
      }
      tags.insert(local);
      if (local == QLatin1String("log"))
        hasLogElement = true;
      if (local == QLatin1String("logcurveinfo") || local == QLatin1String("curveinfo"))
        hasCurveInfo = true;
      if (local == QLatin1String("logdata"))
        hasLogData = true;
      if (local == QLatin1String("worksheet"))
      {
        for (const QXmlStreamAttribute &a : xml.attributes())
        {
          const QString v = a.value().toString().trimmed().toLower();
          if (kSheetNames.contains(v))
            hasNamedSheet = true;
        }
      }
    }
  }
  if (xml.hasError() && elements == 0)
    return WellXmlKind::Unknown; // 非 XML 内容

  const bool isWellLog = (hasLogElement && hasCurveInfo && hasLogData) ||
                         (rootIsWitsml && hasCurveInfo && hasLogData) || hasNamedSheet;
  if (isWellLog)
    return WellXmlKind::WellLog;

  // 井口：出现 well/wellbore 元素并带 x/y（或经纬度）子元素/属性。
  if (tags.contains(QLatin1String("well")) || tags.contains(QLatin1String("wellbore")) ||
      tags.contains(QLatin1String("wellhead")))
  {
    if (tags.contains(QLatin1String("x")) || tags.contains(QLatin1String("y")) ||
        tags.contains(QLatin1String("latitude")) || tags.contains(QLatin1String("longitude")))
      return WellXmlKind::WellHead;
  }
  Q_UNUSED(rootTag);
  return WellXmlKind::Unknown;
}
