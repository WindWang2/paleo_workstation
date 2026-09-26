#include "wellfileparsers.h"

#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>

#include <cmath>

namespace
{
  const double kNullSentinel = -99999.0; // SMI 空值（plan §1）

  QStringList splitTokens(const QString &line)
  {
    return line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
  }

  // 数值列：-99999 视为空。
  bool parseColumn(const QString &token, double *out)
  {
    bool ok = false;
    const double v = token.toDouble(&ok);
    if (!ok || !std::isfinite(v) || v <= kNullSentinel + 0.5) // 非有限值或命中 -99999 哨兵邻域
      return false;
    *out = v;
    return true;
  }
} // namespace

// plan §3：井口文件带 UTF-8 BOM；U+FEFF 不是空白，trimmed() 去不掉。
// 若首行是表头/数据行，BOM 会粘上第一个 token（井名失配），统一先剥。
static QString withoutBom(const QByteArray &text)
{
  QString s = QString::fromUtf8(text);
  if (s.startsWith(u'\uFEFF'))
    s.remove(0, 1);
  return s;
}

QVector<WellHeadRecord> parseWellHeadText(const QByteArray &text)
{
  QVector<WellHeadRecord> rows;
  const QString data = withoutBom(text);
  for (const QString &rawLine : data.split(QRegularExpression(QStringLiteral("[\r\n]")),
                                            Qt::SkipEmptyParts))
  {
    const QString line = rawLine.trimmed();
    if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
      continue;
    const QStringList t = splitTokens(line);
    if (t.size() < 5) // Name X Y KB TotalDepth
      continue;
    WellHeadRecord r;
    r.name = t.at(0);
    bool xOk = false, yOk = false, kbOk = false, tdOk = false;
    r.x = t.at(1).toDouble(&xOk);
    r.y = t.at(2).toDouble(&yOk);
    r.kb = t.at(3).toDouble(&kbOk);
    r.td = t.at(4).toDouble(&tdOk);
    if (!xOk || !yOk || !tdOk)
      continue;
    Q_UNUSED(kbOk);
    // 可选列：BottomX BottomY WellType（-99999/缺列 → has* false / 空串）。
    if (t.size() >= 7)
    {
      if (parseColumn(t.at(5), &r.bottomX))
        r.hasBottomX = true;
      if (parseColumn(t.at(6), &r.bottomY))
        r.hasBottomY = true;
    }
    if (t.size() >= 8)
      r.wellType = t.at(7);
    rows.append(r);
  }
  return rows;
}

QVector<WellTopRecord> parseWellTopsText(const QByteArray &text)
{
  QVector<WellTopRecord> tops;
  const QString data = withoutBom(text);
  for (const QString &rawLine : data.split(QRegularExpression(QStringLiteral("[\r\n]")),
                                            Qt::SkipEmptyParts))
  {
    const QString line = rawLine.trimmed();
    if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
      continue;
    const QStringList t = splitTokens(line);
    if (t.size() < 3) // 井名 层名 MD
      continue;
    WellTopRecord r;
    r.wellName = t.at(0);
    r.topName = t.at(1);
    if (parseColumn(t.at(2), &r.md))
      r.hasMd = true;
    if (t.size() >= 6)
    {
      double v = 0;
      if (parseColumn(t.at(3), &v))
      {
        r.x = v;
        r.hasX = true;
      }
      if (parseColumn(t.at(4), &v))
      {
        r.y = v;
        r.hasY = true;
      }
      if (parseColumn(t.at(5), &v))
        r.z = v;
    }
    if (t.size() >= 7 && parseColumn(t.at(6), &r.tvd))
      r.hasTvd = true;
    if (t.size() >= 8 && parseColumn(t.at(7), &r.timeMs))
      r.hasTime = true;
    tops.append(r);
  }
  return tops;
}

TimeDepthTable parseTimeDepthText(const QByteArray &text)
{
  TimeDepthTable table;
  const QString data = withoutBom(text);
  for (const QString &rawLine : data.split(QRegularExpression(QStringLiteral("[\r\n]")),
                                            Qt::SkipEmptyParts))
  {
    const QString line = rawLine.trimmed();
    if (line.startsWith(QLatin1Char('#')))
    {
      // '# Well : A1'
      const int idx = line.indexOf(QStringLiteral("Well :"), Qt::CaseInsensitive);
      if (idx >= 0 && table.wellName.isEmpty())
        table.wellName = line.mid(idx + 6).trimmed();
      continue;
    }
    if (line.isEmpty())
      continue;
    const QStringList t = splitTokens(line);
    if (t.size() < 4) // TIME TVDSS TVD MD
      continue;
    TdRow row;
    bool sOk = false;
    row.tvdss = t.at(1).toDouble(&sOk);
    // 参与规则（audit #39/T18）：TIME(ms) 命中 -99999/非数值/非有限的行
    // 整行不进时深表——否则 sentinel 时间值会混进插值污染标定。TVDSS 只
    // 要求可解析且有限（它不是查找列，-99999 不逐行）；TVD/MD 列沿用
    // -99999→空。
    if (!sOk || !std::isfinite(row.tvdss) || !parseColumn(t.at(0), &row.timeMs) ||
        !std::isfinite(row.timeMs))
      continue;
    if (parseColumn(t.at(2), &row.tvd))
      row.hasTvd = true;
    if (parseColumn(t.at(3), &row.md))
      row.hasMd = true;
    // 末列可能是 Well 名（'# Well' 已给出，冗余忽略）
    table.rows.append(row);
  }
  return table;
}

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
