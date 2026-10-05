// 层：数据
#include "wellfileparsers.h"

#include "../domain/welltopsedit.h"
#include "../domain/wellnumeric.h"
#include <QCoreApplication>

#include "encodingdetect.h"

#include <QFile>
#include <QDebug>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>

#include <cmath>
#include <limits>

namespace
{
  using paleo::wellnumeric::NumberKind;

  QStringList splitTokens(const QString &line)
  {
    // 明确的列分隔符保留空列；空格分隔旧式 SMI 维持原有规则。
    if (line.contains(QLatin1Char('\t')))
      return line.split(QLatin1Char('\t'), Qt::KeepEmptyParts);
    if (line.contains(QLatin1Char(',')))
      return line.split(QLatin1Char(','), Qt::KeepEmptyParts);
    return line.trimmed().split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
  }

  struct ParseContext
  {
    WellParseReport report;
    int line = 0;
    bool column(const QStringList &tokens, int index, const char *name,
                double *out, bool reject)
    {
      const QString token = tokens.value(index).trimmed();
      const NumberKind kind = paleo::wellnumeric::parse(token, out);
      if (kind == NumberKind::Value)
        return true;
      QString reason;
      switch (kind)
      {
      case NumberKind::Empty:
        ++report.blankCells;
        reason = QCoreApplication::translate("WellFileParsers", "空白或缺列");
        break;
      case NumberKind::NullSentinel:
        ++report.sentinelHits;
        reason = QCoreApplication::translate("WellFileParsers", "空值哨兵");
        break;
      case NumberKind::NonFinite:
        ++report.invalidCells;
        reason = QCoreApplication::translate("WellFileParsers", "非有限数值");
        break;
      case NumberKind::NonNumeric:
        ++report.invalidCells;
        reason = QCoreApplication::translate("WellFileParsers", "非数值");
        break;
      case NumberKind::Value: break;
      }
      report.issues.append(QCoreApplication::translate(
          "WellFileParsers", "第 %1 行 · %2 列（%3）：%4（原值“%5”），%6")
          .arg(line).arg(index + 1).arg(QLatin1String(name), reason, token,
          reject ? QCoreApplication::translate("WellFileParsers", "拒收该行")
                 : QCoreApplication::translate("WellFileParsers", "该列置空")));
      return false;
    }
    bool textColumn(const QStringList &tokens, int index, const char *name, QString *out)
    {
      *out = tokens.value(index).trimmed();
      if (!out->isEmpty()) return true;
      ++report.blankCells;
      report.issues.append(QCoreApplication::translate("WellFileParsers",
          "第 %1 行 · %2 列（%3）：名称为空或缺列，拒收该行")
          .arg(line).arg(index + 1).arg(QLatin1String(name)));
      return false;
    }
    void finish(WellParseReport *out) const
    {
      if (out)
        *out = report;
      else if (!report.issues.isEmpty())
        qWarning().noquote() << wellParseSummary(report);
    }
  };
} // namespace

QString wellParseSummary(const WellParseReport &report)
{
  if (report.issues.isEmpty())
    return {};
  return QCoreApplication::translate("WellFileParsers",
      "井表解析：接收 %1 行，拒收 %2 行；哨兵 %3，空白/缺列 %4，非数值/非有限 %5；%6")
      .arg(report.acceptedRows).arg(report.rejectedRows).arg(report.sentinelHits)
      .arg(report.blankCells).arg(report.invalidCells)
      .arg(report.issues.join(QStringLiteral("；")));
}

// plan §3：井口文件带 UTF-8 BOM；U+FEFF 不是空白，trimmed() 去不掉。
// 若首行是表头/数据行，BOM 会粘上第一个 token（井名失配），统一先剥。
static QString withoutBom(const QByteArray &text)
{
  // D7.2：编码统一收口——GB18030 井名文件不再静默变乱码（EncodingDetect：
  // BOM 剥除 + UTF-8/GB 嗅探；纯 ASCII 三种口径结果一致，零回归面）。
  return EncodingDetect::decodeText(text);
}

QVector<WellHeadRecord> parseWellHeadText(const QByteArray &text, WellParseReport *report)
{
  QVector<WellHeadRecord> rows;
  ParseContext ctx;
  for (const QString &raw : withoutBom(text).split(QRegularExpression(QStringLiteral("\\r\\n|\\n|\\r"))))
  {
    ++ctx.line;
    if (raw.trimmed().isEmpty() || raw.trimmed().startsWith(QLatin1Char('#')))
      continue;
    const QStringList t = splitTokens(raw);
    WellHeadRecord r;
    bool valid = ctx.textColumn(t, 0, "Name", &r.name);
    valid &= ctx.column(t, 1, "X", &r.x, true);
    valid &= ctx.column(t, 2, "Y", &r.y, true);
    valid &= ctx.column(t, 3, "KB", &r.kb, true);
    valid &= ctx.column(t, 4, "TotalDepth", &r.td, true);
    if (!valid || r.name.isEmpty())
    {
      ++ctx.report.rejectedRows;
      continue;
    }
    if (t.size() > 5)
    {
      r.hasBottomX = ctx.column(t, 5, "BottomX", &r.bottomX, false);
      r.hasBottomY = ctx.column(t, 6, "BottomY", &r.bottomY, false);
    }
    if (t.size() >= 8) r.wellType = t.at(7).trimmed();
    rows.append(r);
    ++ctx.report.acceptedRows;
  }
  ctx.finish(report);
  return rows;
}

QVector<WellTopRecord> parseWellTopsText(const QByteArray &text, WellParseReport *report)
{
  QVector<WellTopRecord> tops;
  ParseContext ctx;
  for (const QString &raw : withoutBom(text).split(QRegularExpression(QStringLiteral("\\r\\n|\\n|\\r"))))
  {
    ++ctx.line;
    if (raw.trimmed().isEmpty() || raw.trimmed().startsWith(QLatin1Char('#')))
      continue;
    const QStringList t = splitTokens(raw);
    WellTopRecord r;
    bool namesValid = ctx.textColumn(t, 0, "WellName", &r.wellName);
    namesValid &= ctx.textColumn(t, 1, "Name", &r.topName);
    r.hasMd = ctx.column(t, 2, "MD", &r.md, true);
    if (!r.hasMd || !namesValid)
    {
      ++ctx.report.rejectedRows;
      continue;
    }
    if (t.size() > 3)
    {
      r.hasX = ctx.column(t, 3, "X", &r.x, false);
      r.hasY = ctx.column(t, 4, "Y", &r.y, false);
      if (!ctx.column(t, 5, "Z", &r.z, false))
        r.z = std::numeric_limits<double>::quiet_NaN();
    }
    if (t.size() >= 7) r.hasTvd = ctx.column(t, 6, "TVD", &r.tvd, false);
    if (t.size() >= 8) r.hasTime = ctx.column(t, 7, "Time(ms)", &r.timeMs, false);
    tops.append(r);
    ++ctx.report.acceptedRows;
  }
  ctx.finish(report);
  return tops;
}

QByteArray writeWellTopsText(const QVector<WellTopRecord> &tops)
{
  const QString kNull = QString::number(paleo::wellnumeric::kSmiNull, 'f', 3);
  const int kNumW = 14, kNameW = 13; // 列宽对齐 fixture 风格（解析按空白切，仅美观）
  auto padNum = [](const QString &s) {
    // 超宽值（如 'g',17 长串）不再负数补空——至少留一个空格分隔。
    return QString(s.size() >= 14 ? 1 : 14 - s.size(), QLatin1Char(' ')) + s;
  };
  auto padName = [&](const QString &s) {
    return s + QString(qMax(1, kNameW - s.size()), QLatin1Char(' '));
  };
  auto num = [&](bool has, double v) {
    return padNum(has ? WellTopsEdit::formatDepth(v) : kNull);
  };

  QString out;
  out.reserve(tops.size() * 96 + 128);
  out += QStringLiteral("#WellTops File From Paleo\r\n");
  out += QStringLiteral("#WellName    Name         MD           X            Y            Z            TVD          Time(ms)\r\n");
  for (const WellTopRecord &r : tops)
  {
    // Z 列无判空标志（解析器 t.size()>=6 组内独立解析）：X/Y 任一有效即写出 z
    // ——半坐标组（X 有效 Y 哨兵）的 z 是真实值，整组写哨兵会静默丢（轮 3 M2）。
    const bool hasXy = r.hasX || r.hasY;
    out += padName(r.wellName) + padName(r.topName) + num(r.hasMd, r.md) +
           num(r.hasX, r.x) + num(r.hasY, r.y) + num(hasXy && paleo::wellnumeric::isUsable(r.z), r.z) +
           num(r.hasTvd, r.tvd) + num(r.hasTime, r.timeMs) + QStringLiteral("\r\n");
  }
  return out.toUtf8();
}

TimeDepthTable parseTimeDepthText(const QByteArray &text, WellParseReport *report)
{
  TimeDepthTable table;
  ParseContext ctx;
  for (const QString &raw : withoutBom(text).split(QRegularExpression(QStringLiteral("\\r\\n|\\n|\\r"))))
  {
    ++ctx.line;
    const QString line = raw.trimmed();
    if (line.startsWith(QLatin1Char('#')))
    {
      const int idx = line.indexOf(QStringLiteral("Well :"), Qt::CaseInsensitive);
      if (idx >= 0 && table.wellName.isEmpty()) table.wellName = line.mid(idx + 6).trimmed();
      continue;
    }
    if (line.isEmpty()) continue;
    const QStringList t = splitTokens(raw);
    TdRow row;
    const bool timeOk = ctx.column(t, 0, "TIME(ms)", &row.timeMs, true);
    const bool sOk = ctx.column(t, 1, "TVDSS", &row.tvdss, true);
    if (!timeOk || !sOk)
    {
      ++ctx.report.rejectedRows;
      continue;
    }
    row.hasTvd = ctx.column(t, 2, "TVD", &row.tvd, false);
    row.hasMd = ctx.column(t, 3, "MD", &row.md, false);
    table.rows.append(row);
    ++ctx.report.acceptedRows;
  }
  ctx.finish(report);
  return table;
}

// 保留旧式单参数入口（包括最小链接测试壳的前向声明）。
QVector<WellHeadRecord> parseWellHeadText(const QByteArray &text)
{
  return parseWellHeadText(text, nullptr);
}
QVector<WellTopRecord> parseWellTopsText(const QByteArray &text)
{
  return parseWellTopsText(text, nullptr);
}
TimeDepthTable parseTimeDepthText(const QByteArray &text)
{
  return parseTimeDepthText(text, nullptr);
}
DeviationTable parseDeviationText(const QByteArray &text)
{
  return parseDeviationText(text, nullptr);
}

DeviationTable parseDeviationText(const QByteArray &text, WellParseReport *report)
{
  DeviationTable table;
  ParseContext ctx;
  for (const QString &raw : withoutBom(text).split(QRegularExpression(QStringLiteral("\\r\\n|\\n|\\r"))))
  {
    ++ctx.line;
    const QString line = raw.trimmed();
    if (line.startsWith(QLatin1Char('#')))
    {
      const int idx = line.indexOf(QStringLiteral("Well :"), Qt::CaseInsensitive);
      if (idx >= 0 && table.wellName.isEmpty()) table.wellName = line.mid(idx + 6).trimmed();
      continue;
    }
    if (line.isEmpty()) continue;
    const QStringList t = splitTokens(raw);
    DeviationStationRecord r;
    bool valid = true;
    valid &= ctx.column(t, 0, "MD", &r.md, true);
    valid &= ctx.column(t, 1, "INCL", &r.inclinationDeg, true);
    valid &= ctx.column(t, 2, "AZI", &r.azimuthDeg, true);
    if (!valid)
    {
      ++ctx.report.rejectedRows;
      continue;
    }
    table.stations.append(r);
    ++ctx.report.acceptedRows;
  }
  ctx.finish(report);
  return table;
}
