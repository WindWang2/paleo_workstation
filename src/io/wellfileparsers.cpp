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
#include <QStringConverter>
#include <QStringDecoder>
#include <QXmlStreamReader>

#include <cmath>
#include <algorithm>
#include <functional>
#include <limits>

// plan §3：井口文件带 UTF-8 BOM；U+FEFF 不是空白，trimmed() 去不掉。
// 若首行是表头/数据行，BOM 会粘上第一个 token（井名失配），统一先剥。
static QString withoutBom(const QByteArray &text)
{
  // D7.2：编码统一收口——GB18030 井名文件不再静默变乱码（EncodingDetect：
  // BOM 剥除 + UTF-8/GB 嗅探；纯 ASCII 三种口径结果一致，零回归面）。
  return EncodingDetect::decodeText(text);
}

bool wellHeaderMatchesAlias(const QString &header, const QString &alias)
{
  const bool ascii = !alias.contains(QRegularExpression(QStringLiteral("[^A-Za-z0-9_]")));
  if (!ascii)
    return header.contains(alias, Qt::CaseInsensitive);
  // 单字母只认整词。更长的别名再认 _NAME，不放宽 \b，避免 MD 命中 BASE_MD。
  const QString pattern = alias.size() <= 1
                              ? QStringLiteral("\\b%1\\b")
                              : QStringLiteral("\\b%1(?:_NAME)?\\b");
  const QRegularExpression word(pattern.arg(QRegularExpression::escape(alias)),
                                QRegularExpression::CaseInsensitiveOption);
  return word.match(header).hasMatch();
}

namespace
{
  using paleo::wellnumeric::NumberKind;

  // PW-072：RFC 4180 引号规则——"a,b" 为一列，"" 为字面引号，引号外分隔。
  // 单行解析（跨行引号字段按行截断，原样保留已读内容）。
  QStringList splitQuoted(const QString &line, QChar delim)
  {
    QStringList out;
    QString cur;
    bool inQuotes = false;
    bool fieldWasQuoted = false;
    for (int i = 0; i < line.size(); ++i)
    {
      const QChar c = line.at(i);
      if (inQuotes)
      {
        if (c == QLatin1Char('"'))
        {
          if (i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"'))
          {
            cur.append(QLatin1Char('"'));
            ++i;
          }
          else
            inQuotes = false;
        }
        else
          cur.append(c);
      }
      else if (c == delim)
      {
        out.append(cur);
        cur.clear();
        fieldWasQuoted = false;
      }
      else if (c == QLatin1Char('"') && !fieldWasQuoted && cur.trimmed().isEmpty())
      {
        cur.clear();
        inQuotes = true;
        fieldWasQuoted = true;
      }
      else
        cur.append(c);
    }
    out.append(cur);
    return out;
  }

  QStringList splitTokens(const QString &line)
  {
    // 明确的列分隔符保留空列；空格分隔旧式 SMI 维持原有规则。
    if (line.contains(QLatin1Char('\t')))
      return splitQuoted(line, QLatin1Char('\t'));
    if (line.contains(QLatin1Char(',')))
      return splitQuoted(line, QLatin1Char(','));
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

  // ---- 分层文本头驱动列映射（工区惯例兼容）----
  // '#' 注释头行列名齐全时按名取列（如 "wellName DepthMD topName topVerName"，
  // 道号/层名列序与 SMI 位置约定不同）；列名不全 → 返回 false，调用方维持
  // 既有位置约定（井名 层名 MD [X Y Z TVD Time]）。
  // 列名与井表第二轮同一口径（词边界 + <别名>_NAME）。仅有 BASE_MD /
  // BOTTOM_MD 时不回退成位置上的顶深。
  struct TopsColumnMap
  {
    int well = -1, top = -1, md = -1, x = -1, y = -1, z = -1, tvd = -1, time = -1;
    int maxIndex() const
    {
      return qMax(qMax(qMax(well, top), qMax(md, x)),
                  qMax(qMax(qMax(y, z), tvd), time));
    }
  };

  bool tokenMatches(const QString &token, const QStringList &names)
  {
    for (const QString &n : names)
      if (wellHeaderMatchesAlias(token, n))
        return true;
    return false;
  }

  // 底深列：整词 BASE_MD / BOTTOM_MD（及垂深、中文底界）。不占顶深角色。
  bool isExplicitBaseDepth(const QString &token)
  {
    const QRegularExpression ascii(QStringLiteral("\\b(?:BASE|BOTTOM)_(?:MD|TVD)\\b"),
                                   QRegularExpression::CaseInsensitiveOption);
    if (ascii.match(token).hasMatch()) return true;
    for (const QString &name : {QStringLiteral("底界测深"), QStringLiteral("底测深"),
                                QStringLiteral("底界垂深"), QStringLiteral("底垂深")})
      if (token.contains(name)) return true;
    return false;
  }

  bool sniffTopsColumnMap(const QStringList &tokens, TopsColumnMap *map)
  {
    TopsColumnMap m;
    bool sawBaseDepth = false;
    for (int i = 0; i < tokens.size(); ++i)
    {
      const QString &t = tokens.at(i);
      if (isExplicitBaseDepth(t)) { sawBaseDepth = true; continue; }
      if (m.well < 0 && tokenMatches(t, {QStringLiteral("wellname"),
                                          QStringLiteral("well"),
                                          QStringLiteral("井名"), QStringLiteral("井号")}))
        m.well = i;
      else if (m.top < 0 && tokenMatches(t, {QStringLiteral("topname"),
                                             QStringLiteral("top"),
                                             QStringLiteral("层名"), QStringLiteral("层位")}))
        m.top = i;
      else if (m.md < 0 && tokenMatches(t, {QStringLiteral("depthmd"),
                                            QStringLiteral("md"),
                                            QStringLiteral("顶深"), QStringLiteral("深度")}))
        m.md = i;
      else if (m.x < 0 && tokenMatches(t, {QStringLiteral("x")}))
        m.x = i;
      else if (m.y < 0 && tokenMatches(t, {QStringLiteral("y")}))
        m.y = i;
      else if (m.z < 0 && tokenMatches(t, {QStringLiteral("z")}))
        m.z = i;
      else if (m.tvd < 0 && tokenMatches(t, {QStringLiteral("tvd")}))
        m.tvd = i;
      else if (m.time < 0 && tokenMatches(t, {QStringLiteral("time")}))
        m.time = i;
    }
    // 井名+层名已认出、但只有底深列：按名取列且不把底深当成顶深。
    // 否则会退回位置约定，第三列仍被写成 MD。
    if (m.well < 0 || m.top < 0 || (m.md < 0 && m.tvd < 0 && m.time < 0 && !sawBaseDepth))
      return false;
    *map = m;
    return true;
  }

  // ---- #561 逐行解析核（整读/流式两路共用；行体与原整读循环逐字节同口径）----
  void feedWellHeadLine(const QString &raw, ParseContext &ctx,
                        QVector<WellHeadRecord> &rows)
  {
    ++ctx.line;
    if (raw.trimmed().isEmpty() || raw.trimmed().startsWith(QLatin1Char('#')))
      return;
    const QStringList t = splitTokens(raw);
    WellHeadRecord r;
    bool valid = ctx.textColumn(t, 0, "Name", &r.name);
    valid &= ctx.column(t, 1, "X", &r.x, true);
    valid &= ctx.column(t, 2, "Y", &r.y, true);
    if (!ctx.column(t, 3, "KB", &r.kb, false)) r.kb = qQNaN();
    if (!ctx.column(t, 4, "TotalDepth", &r.td, false)) r.td = qQNaN();
    if (!valid || r.name.isEmpty())
    {
      ++ctx.report.rejectedRows;
      return;
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

  // 头驱动列映射的跨行状态：数据行之前的首条 '#' 注释行若含齐全列名
  //（井名/层名/深度，如 "wellName DepthMD topName topVerName"），按名取列；
  // 否则维持位置约定。
  struct TopsLineState
  {
    bool headerChecked = false;
    bool explicitDepthBasis = false;
    TopsColumnMap cmap;
    bool byName = false;
  };

  void feedWellTopsLine(const QString &raw, ParseContext &ctx, TopsLineState &st,
                        QVector<WellTopRecord> &tops)
  {
    ++ctx.line;
    const QString line = raw.trimmed();
    if (line.isEmpty())
      return;
    if (line.startsWith(QLatin1Char('#')))
    {
      if (line == QLatin1String("# PaleoDepthBasis: explicit")) st.explicitDepthBasis = true;
      if (!st.headerChecked)
      {
        st.headerChecked = true;
        st.byName = sniffTopsColumnMap(splitTokens(line.mid(1)), &st.cmap);
        if (st.byName && st.cmap.md < 0) st.explicitDepthBasis = true;
      }
      return;
    }
    st.headerChecked = true;
    const QStringList t = splitTokens(raw);
    WellTopRecord r;
    if (st.byName)
    {
      // 列名驱动按名取列：可选列缺席（索引 -1）跳过不记 issue。
      if (t.size() <= st.cmap.maxIndex())
      {
        ++ctx.report.rejectedRows; // 列不全的行不硬猜
        return;
      }
      bool namesValid = ctx.textColumn(t, st.cmap.well, "WellName", &r.wellName);
      namesValid &= ctx.textColumn(t, st.cmap.top, "Name", &r.topName);
      if (st.cmap.md >= 0) r.hasMd = ctx.column(t, st.cmap.md, "MD", &r.md, !st.explicitDepthBasis);
      if (!namesValid || (!r.hasMd && !st.explicitDepthBasis))
      {
        ++ctx.report.rejectedRows;
        return;
      }
      if (st.cmap.x >= 0) r.hasX = ctx.column(t, st.cmap.x, "X", &r.x, false);
      if (st.cmap.y >= 0) r.hasY = ctx.column(t, st.cmap.y, "Y", &r.y, false);
      if (st.cmap.z >= 0 && !ctx.column(t, st.cmap.z, "Z", &r.z, false))
        r.z = std::numeric_limits<double>::quiet_NaN();
      if (st.cmap.tvd >= 0) r.hasTvd = ctx.column(t, st.cmap.tvd, "TVD", &r.tvd, false);
      if (st.cmap.time >= 0) r.hasTime = ctx.column(t, st.cmap.time, "Time(ms)", &r.timeMs, false);
      if (!r.hasMd && !r.hasTvd && !r.hasTime) { ++ctx.report.rejectedRows; return; }
      tops.append(r);
      ++ctx.report.acceptedRows;
      return;
    }
    bool namesValid = ctx.textColumn(t, 0, "WellName", &r.wellName);
    namesValid &= ctx.textColumn(t, 1, "Name", &r.topName);
    r.hasMd = ctx.column(t, 2, "MD", &r.md, !st.explicitDepthBasis);
    if (!namesValid || (!r.hasMd && !st.explicitDepthBasis))
    {
      ++ctx.report.rejectedRows;
      return;
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
    if (!r.hasMd && !r.hasTvd && !r.hasTime) { ++ctx.report.rejectedRows; return; }
    tops.append(r);
    ++ctx.report.acceptedRows;
  }

  // 时深行级状态：Petrel checkshots 导出（工区数据服务的原生格式，列序
  // X Y Z "TWT picked" MD Well…与本表 TIME/TVDSS/TVD/MD 固定列序不兼容）
  // 嗅探即整表拒收——按固定列序硬吞会把 X 坐标当时间、TWT 当 MD 产出伪表。
  struct TdLineState
  {
    bool petrel = false;
  };

  void feedTimeDepthLine(const QString &raw, ParseContext &ctx, TdLineState &st,
                         TimeDepthTable &table)
  {
    ++ctx.line;
    const QString line = raw.trimmed();
    if (line.startsWith(QLatin1Char('#')))
    {
      if (!st.petrel && line.contains(QStringLiteral("Petrel checkshots format"),
                                      Qt::CaseInsensitive))
      {
        st.petrel = true;
        ctx.report.issues.append(QCoreApplication::translate(
            "WellFileParsers",
            "第 %1 行声明 Petrel checkshots 格式：本解析器的固定列序"
            "（TIME/TVDSS/TVD/MD）不适用，整表拒收，请经工区数据服务导入解析")
            .arg(ctx.line));
      }
      const int idx = line.indexOf(QStringLiteral("Well :"), Qt::CaseInsensitive);
      if (idx >= 0 && table.wellName.isEmpty()) table.wellName = line.mid(idx + 6).trimmed();
      return;
    }
    if (line.isEmpty()) return;
    if (st.petrel)
    {
      // 表头块（VERSION/BEGIN HEADER/列名行）与数据行一并拒收，不再逐行报
      //「没有可用深度」噪声——上面那条 Petrel 诊断已交代整表去向。
      ++ctx.report.rejectedRows;
      return;
    }
    const QStringList t = splitTokens(raw);
    TdRow row;
    row.tvdss = std::numeric_limits<double>::quiet_NaN();
    const bool timeOk = ctx.column(t, 0, "TIME(ms)", &row.timeMs, true);
    // TVDSS 不是 MD/TVD 查找列；缺失时仍保留其它真实深度，不能连带拒收。
    const bool sOk = ctx.column(t, 1, "TVDSS", &row.tvdss, false);
    row.hasTvd = ctx.column(t, 2, "TVD", &row.tvd, false);
    row.hasMd = ctx.column(t, 3, "MD", &row.md, false);
    if (!timeOk || (!sOk && !row.hasTvd && !row.hasMd))
    {
      if (!sOk && !row.hasTvd && !row.hasMd)
        ctx.report.issues.append(QCoreApplication::translate("WellFileParsers",
            "第 %1 行没有可用深度（TVDSS/TVD/MD），拒收该行").arg(ctx.line));
      ++ctx.report.rejectedRows;
      return;
    }
    table.rows.append(row);
    ++ctx.report.acceptedRows;
  }

  void feedDeviationLine(const QString &raw, ParseContext &ctx, DeviationTable &table)
  {
    ++ctx.line;
    const QString line = raw.trimmed();
    if (line.startsWith(QLatin1Char('#')))
    {
      const int idx = line.indexOf(QStringLiteral("Well :"), Qt::CaseInsensitive);
      if (idx >= 0 && table.wellName.isEmpty()) table.wellName = line.mid(idx + 6).trimmed();
      return;
    }
    if (line.isEmpty()) return;
    const QStringList t = splitTokens(raw);
    DeviationStationRecord r;
    bool valid = true;
    valid &= ctx.column(t, 0, "MD", &r.md, true);
    valid &= ctx.column(t, 1, "INCL", &r.inclinationDeg, true);
    valid &= ctx.column(t, 2, "AZI", &r.azimuthDeg, true);
    if (!valid)
    {
      ++ctx.report.rejectedRows;
      return;
    }
    table.stations.append(r);
    ++ctx.report.acceptedRows;
  }

  // ---- #561 流式骨架：全文件编码嗅探（O(64KB) 内存）→ 逐行收割喂核 ----
  // 嗅探决策与 EncodingDetect::detect(stripBom(整文件)) 逐条对齐：BOM →
  // Utf8；严格 UTF-8（EOF 半截尾序列按 detect 的 trimIncompleteUtf8Tail
  // 同款容差）→ Utf8；无高位字节 → Utf8；GB18030 全文件合法 → GB18030；
  // 否则 Latin1。
  struct StrictUtf8Stream
  {
    bool ok = true;
    int rem = 0;              // 剩余续字节
    quint32 cp = 0, minCp = 0, maxCp = 0;
    void feed(uchar b)
    {
      if (!ok) return;
      if (rem > 0)
      {
        if ((b & 0xC0) != 0x80) { ok = false; return; }
        cp = (cp << 6) | (b & 0x3F);
        if (--rem == 0 && (cp < minCp || cp > maxCp || (cp >= 0xD800 && cp <= 0xDFFF)))
          ok = false; // 过长编码 / 码点越界 / 代理区
        return;
      }
      if (b < 0x80) return;
      if ((b & 0xE0) == 0xC0) { rem = 1; cp = b & 0x1F; minCp = 0x80; maxCp = 0x7FF; }
      else if ((b & 0xF0) == 0xE0) { rem = 2; cp = b & 0x0F; minCp = 0x800; maxCp = 0xFFFF; }
      else if ((b & 0xF8) == 0xF0) { rem = 3; cp = b & 0x07; minCp = 0x10000; maxCp = 0x10FFFF; }
      else ok = false; // 0x80-0xBF 作首字节 / 0xF8+ 均非法
    }
    // EOF：rem>0 是尾部半截序列——detect 对整段剥尾后再验，同款容差。
    bool strictAtEof() const { return ok; }
  };

  // GB18030 字节合法性状态机（等价 QStringDecoder("GB18030") 无 U+FFFD 的
  // 判定面；unassigned 码位这类映射层角落按合法论处——井表字符面到不了）。
  struct Gb18030ValidStream
  {
    bool ok = true;
    int st = 0; // 0=期望首字节 1=双字节第二/四字节分岔 3=四字节第3 4=四字节第4
    void feed(uchar b)
    {
      if (!ok) return;
      const bool ascii = b < 0x80;
      switch (st)
      {
        case 0:
          if (ascii) return;
          if (b >= 0x81 && b <= 0xFE) { st = 1; return; }
          ok = false; return;
        case 1: // 首字节后：0x30-0x39 走四字节，其余合法双字节第二字节
          if (b >= 0x30 && b <= 0x39) { st = 3; return; }
          if ((b >= 0x40 && b <= 0x7E) || (b >= 0x80 && b <= 0xFE)) { st = 0; return; }
          ok = false; return;
        case 3:
          if (b >= 0x81 && b <= 0xFE) { st = 4; return; }
          ok = false; return;
        case 4:
          if (b >= 0x30 && b <= 0x39) { st = 0; return; }
          ok = false; return;
      }
    }
    bool validAtEof() const { return ok && st == 0; } // 悬置 = 尾部半截序列 → U+FFFD
  };

  struct StreamSniff
  {
    TextEncoding enc = TextEncoding::Utf8;
    bool utf16 = false;
  };

  bool sniffStreamEncoding(QIODevice *dev, StreamSniff *out, QString *ioError)
  {
    dev->seek(0);
    StrictUtf8Stream utf8;
    Gb18030ValidStream gb;
    bool hasHighByte = false, bomChecked = false;
    char buf[64 << 10];
    qint64 n = 0;
    while ((n = dev->read(buf, sizeof buf)) > 0)
    {
      const uchar *p = reinterpret_cast<const uchar *>(buf);
      qsizetype off = 0;
      if (!bomChecked)
      {
        bomChecked = true;
        if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
        {
          off = 3; // UTF-8 BOM：剥后嗅探（stripBom 同序；行级 BOM 在收割侧再剥）
        }
        else if (n >= 2 && ((p[0] == 0xFF && p[1] == 0xFE) || (p[0] == 0xFE && p[1] == 0xFF)))
        {
          out->utf16 = true; // UTF-16：stripBom 要全文重编码，调用方整读兜底
          return true;
        }
      }
      for (qsizetype i = off; i < n; ++i)
      {
        const uchar b = p[i];
        if (b >= 0x80) hasHighByte = true;
        utf8.feed(b);
        gb.feed(b);
      }
    }
    if (n < 0)
    {
      if (ioError) *ioError = dev->errorString();
      return false;
    }
    // 决策序与 detect() 一致：严格 UTF-8 →（无高位字节必属前者，不单列）→
    // GB18030 → Latin1。空文件走严格 UTF-8 分支（Utf8）。
    if (utf8.strictAtEof())
      out->enc = TextEncoding::Utf8;
    else if (gb.validAtEof() && hasHighByte)
      out->enc = TextEncoding::GB18030;
    else
      out->enc = TextEncoding::Latin1;
    return true;
  }

  QString decodeStreamLine(const QByteArray &raw, TextEncoding enc)
  {
    switch (enc)
    {
      case TextEncoding::GB18030:
      {
        // 行界（\n/\r 是 ASCII）不会落进 GB 多字节序列中间——逐行无状态解
        // 与整段一次解逐字符等价。
        QStringDecoder dec(QStringLiteral("GB18030"), QStringConverter::Flag::Stateless);
        return dec.decode(raw);
      }
      case TextEncoding::Latin1:
        return QString::fromLatin1(raw);
      case TextEncoding::Utf8:
        break;
    }
    return QString::fromUtf8(raw);
  }

  // 逐行收割：\r\n / \n / \r 与整读版的 split 正则同界（\r\n 记一次）。
  // 64KB 块滚动 + 至多一行 carry，内存与文件大小无关。终结符不进行体
  //（split 同款）；尾随终结符后的幻影空行被各解析核跳过，不产出。
  bool feedStreamLines(QIODevice *dev, QCryptographicHash *runningHash,
                       TextEncoding enc,
                       const std::function<void(const QString &)> &feed,
                       QString *ioError)
  {
    dev->seek(0);
    QByteArray carry;     // 跨块未终结行
    bool afterCr = false; // 上一块末字节 '\r'（\r\n 前半悬置，下块首 '\n' 吞掉）
    bool first = true;
    char buf[64 << 10];
    qint64 n = 0;
    const auto emitRaw = [&](QByteArray line) {
      if (first)
      {
        first = false;
        // BOM 只可能出现在首行（嗅探已识别编码；UTF-8 BOM 在此剥除）。
        if (line.startsWith("\xEF\xBB\xBF"))
          line.remove(0, 3);
      }
      feed(decodeStreamLine(line, enc));
    };
    while ((n = dev->read(buf, sizeof buf)) > 0)
    {
      if (runningHash)
        runningHash->addData(QByteArrayView(buf, static_cast<qsizetype>(n)));
      const char *p = buf;
      qsizetype start = 0, i = 0;
      if (afterCr)
      {
        afterCr = false;
        if (p[0] == '\n') { start = i = 1; } // \r\n 合一界（行已随 '\r' 发出）
      }
      for (; i < n; ++i)
      {
        if (p[i] == '\n')
        {
          emitRaw(carry + QByteArray(p + start, int(i - start)));
          carry.clear();
          start = i + 1;
        }
        else if (p[i] == '\r')
        {
          emitRaw(carry + QByteArray(p + start, int(i - start)));
          carry.clear();
          if (i + 1 == n)
          {
            afterCr = true; // 可能是 \r\n 前半：下块首字节 '\n' 才吞，行不重发
            start = i + 1;
          }
          else if (p[i + 1] == '\n')
          {
            ++i;
            start = i + 1;
          }
          else
            start = i + 1;
        }
      }
      if (start < n)
        carry.append(p + start, int(n - start));
    }
    if (n < 0)
    {
      if (ioError) *ioError = dev->errorString();
      return false;
    }
    if (!carry.isEmpty())
      emitRaw(carry); // EOF 未终结的尾行
    return true;
  }

  // 流式驱动：嗅探 → 逐行喂核 → finish。UTF-16 / 顺序设备退整读兜底
  //（stripBom 重编码 / 嗅探需整文件回看；调用方守大文件闸，与旧 readAll
  // 同界）。整读兜底同样喂 runningHash（等价 readAll 后整段哈希）。
  bool driveWellTextStream(QIODevice *dev, QCryptographicHash *runningHash,
                           WellParseReport *report, QString *ioError,
                           const std::function<void(const QString &, ParseContext &)> &feed)
  {
    ParseContext ctx;
    const auto wholeReadFallback = [&]() -> bool {
      // 分块收拢（read<0 判读错误——QIODevice 无 error() 枚举）；内存等同
      // 整读（该兜底仅 UTF-16 / 顺序设备罕见面，调用方守大文件闸）。
      QByteArray all;
      char b[64 << 10];
      qint64 r = 0;
      while ((r = dev->read(b, sizeof b)) > 0)
        all.append(b, int(r));
      if (r < 0)
      {
        if (ioError) *ioError = dev->errorString();
        return false;
      }
      if (runningHash) runningHash->addData(all);
      for (const QString &raw : withoutBom(all).split(QRegularExpression(QStringLiteral("\\r\\n|\\n|\\r"))))
        feed(raw, ctx);
      return true;
    };
    if (!dev->isSequential())
    {
      StreamSniff sniff;
      if (!sniffStreamEncoding(dev, &sniff, ioError))
        return false;
      if (!sniff.utf16)
      {
        if (!feedStreamLines(dev, runningHash, sniff.enc,
                             [&](const QString &raw) { feed(raw, ctx); }, ioError))
          return false;
        ctx.finish(report);
        return true;
      }
      if (!dev->seek(0))
      {
        if (ioError) *ioError = dev->errorString();
        return false;
      }
    }
    if (!wholeReadFallback())
      return false;
    ctx.finish(report);
    return true;
  }
} // namespace

QString wellParseSummary(const WellParseReport &report)
{
  if (report.issues.isEmpty())
    return {};
  QStringList excerpts;
  for(const auto &issue:report.issues.mid(0,16))excerpts.append(issue.left(512));
  if(report.issues.size()>16)excerpts.append(QCoreApplication::translate("WellFileParsers","其余 %1 条见逐行报告").arg(report.issues.size()-16));
  return QCoreApplication::translate("WellFileParsers",
      "井表解析：接收 %1 行，拒收 %2 行；哨兵 %3，空白/缺列 %4，非数值/非有限 %5；%6")
      .arg(report.acceptedRows).arg(report.rejectedRows).arg(report.sentinelHits)
      .arg(report.blankCells).arg(report.invalidCells)
      .arg(excerpts.join(QStringLiteral("；")));
}

QVector<WellHeadRecord> parseWellHeadText(const QByteArray &text, WellParseReport *report)
{
  QVector<WellHeadRecord> rows;
  ParseContext ctx;
  for (const QString &raw : withoutBom(text).split(QRegularExpression(QStringLiteral("\\r\\n|\\n|\\r"))))
    feedWellHeadLine(raw, ctx, rows);
  ctx.finish(report);
  return rows;
}

QVector<WellTopRecord> parseWellTopsText(const QByteArray &text, WellParseReport *report)
{
  QVector<WellTopRecord> tops;
  ParseContext ctx;
  TopsLineState st;
  for (const QString &raw : withoutBom(text).split(QRegularExpression(QStringLiteral("\\r\\n|\\n|\\r"))))
    feedWellTopsLine(raw, ctx, st, tops);
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
  if (std::any_of(tops.begin(), tops.end(), [](const auto &r) { return !r.hasMd && (r.hasTvd || r.hasTime); }))
    out += QStringLiteral("# PaleoDepthBasis: explicit\r\n");
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
  TdLineState st;
  for (const QString &raw : withoutBom(text).split(QRegularExpression(QStringLiteral("\\r\\n|\\n|\\r"))))
    feedTimeDepthLine(raw, ctx, st, table);
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
    feedDeviationLine(raw, ctx, table);
  ctx.finish(report);
  return table;
}

// ---- #561 流式逐行入口（驱动骨架见上；行为/报告口径与整读版共用同一核）----
std::optional<QVector<WellHeadRecord>>
parseWellHeadDevice(QIODevice *dev, QCryptographicHash *runningHash,
                    WellParseReport *report, QString *ioError)
{
  QVector<WellHeadRecord> rows;
  if (!driveWellTextStream(dev, runningHash, report, ioError,
                           [&rows](const QString &raw, ParseContext &ctx) {
                             feedWellHeadLine(raw, ctx, rows);
                           }))
    return std::nullopt;
  return rows;
}

std::optional<QVector<WellTopRecord>>
parseWellTopsDevice(QIODevice *dev, QCryptographicHash *runningHash,
                    WellParseReport *report, QString *ioError)
{
  QVector<WellTopRecord> tops;
  TopsLineState st;
  if (!driveWellTextStream(dev, runningHash, report, ioError,
                           [&tops, &st](const QString &raw, ParseContext &ctx) {
                             feedWellTopsLine(raw, ctx, st, tops);
                           }))
    return std::nullopt;
  return tops;
}

std::optional<TimeDepthTable>
parseTimeDepthDevice(QIODevice *dev, QCryptographicHash *runningHash,
                     WellParseReport *report, QString *ioError)
{
  TimeDepthTable table;
  TdLineState st;
  if (!driveWellTextStream(dev, runningHash, report, ioError,
                           [&table, &st](const QString &raw, ParseContext &ctx) {
                             feedTimeDepthLine(raw, ctx, st, table);
                           }))
    return std::nullopt;
  return table;
}

std::optional<DeviationTable>
parseDeviationDevice(QIODevice *dev, QCryptographicHash *runningHash,
                     WellParseReport *report, QString *ioError)
{
  DeviationTable table;
  if (!driveWellTextStream(dev, runningHash, report, ioError,
                           [&table](const QString &raw, ParseContext &ctx) {
                             feedDeviationLine(raw, ctx, table);
                           }))
    return std::nullopt;
  return table;
}
