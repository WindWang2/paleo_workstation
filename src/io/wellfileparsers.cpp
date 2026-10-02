// 层：数据
#include "wellfileparsers.h"

#include "encodingdetect.h"

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
  // D7.2：编码统一收口——GB18030 井名文件不再静默变乱码（EncodingDetect：
  // BOM 剥除 + UTF-8/GB 嗅探；纯 ASCII 三种口径结果一致，零回归面）。
  return EncodingDetect::decodeText(text);
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
    // All five columns are required.  Use the same finite/sentinel rules as
    // the optional columns below so a missing coordinate is never marked as
    // a valid surface position by the importer.
    if (!parseColumn(t.at(1), &r.x) || !parseColumn(t.at(2), &r.y) ||
        !parseColumn(t.at(3), &r.kb) || !parseColumn(t.at(4), &r.td))
      continue;
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

DeviationTable parseDeviationText(const QByteArray &text)
{
  DeviationTable table;
  const QString data = withoutBom(text);
  for (const QString &rawLine : data.split(QRegularExpression(QStringLiteral("[\r\n]")),
                                            Qt::SkipEmptyParts))
  {
    const QString line = rawLine.trimmed();
    if (line.startsWith(QLatin1Char('#')))
    {
      const int idx = line.indexOf(QStringLiteral("Well :"), Qt::CaseInsensitive);
      if (idx >= 0 && table.wellName.isEmpty())
        table.wellName = line.mid(idx + 6).trimmed();
      continue;
    }
    if (line.isEmpty())
      continue;
    const QStringList t = splitTokens(line);
    if (t.size() < 3) // MD 井斜角 方位角
      continue;
    DeviationStationRecord r;
    // 站点三元组缺一不可：任一列无效整行丢弃（区别于 tops 的可选列语义）。
    if (!parseColumn(t.at(0), &r.md) || !parseColumn(t.at(1), &r.inclinationDeg) ||
        !parseColumn(t.at(2), &r.azimuthDeg))
      continue;
    table.stations.append(r);
  }
  return table;
}

