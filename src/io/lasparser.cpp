// 层：数据
#include "lasparser.h"
#include "ioerrors_internal.h"
#include "../domain/wellnumeric.h"

#include "cachebudget.h"
#include "encodingdetect.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

qint64 LasParser::s_fileSizeLimit = 500LL * 1024 * 1024;

// ---------------------------------------------------------------------------
// LAS 2.x item lines have the form "MNEM.UNIT VALUE : DESCRIPTION":
//   mnemonic    — up to the first '.'
//   unit        — between '.' and the first whitespace (may be empty)
//   value       — between unit and ':' (may be empty, e.g. ~C lines)
//   description — everything after ':'
// Section headers start with '~' followed by a code letter (V/W/C/A/…);
// comment lines start with '#'. ~A is the only section whose body is data
// rows rather than item lines.
// ---------------------------------------------------------------------------
namespace
{
using paleo::io_detail::setError;

  struct LasItem
  {
    QString mnem;
    QString unit;
    QString value;
    QString descr;
  };

  bool parseItemLine(const QString &line, LasItem &item)
  {
    const int colon = line.indexOf(QLatin1Char(':'));
    const QString left = (colon >= 0 ? line.left(colon) : line).trimmed();
    item.descr = (colon >= 0 ? line.mid(colon + 1) : QString()).trimmed();

    const int dot = left.indexOf(QLatin1Char('.'));
    if (dot < 0)
      return false; // every LAS item carries "MNEM." at minimum

    item.mnem = left.left(dot).trimmed().toUpper();
    const QString rest = left.mid(dot + 1);
    const int sp = rest.indexOf(QRegularExpression(QStringLiteral("\\s")));
    item.unit = (sp < 0 ? rest : rest.left(sp)).trimmed();
    item.value = (sp < 0 ? QString() : rest.mid(sp + 1)).trimmed();
    return !item.mnem.isEmpty();
  }

  double nan()
  {
    return std::numeric_limits<double>::quiet_NaN();
  }

  // -------------------------------------------------------------------------
  // 方向 21：数据段 token → double 的快速路径。
  //
  // 背景：整卷 LAS 的解析时间几乎全部花在「每个 token 一次
  // QByteArray::fromRawData(...).toDouble()」上——实测量级 ~42ns/token
  // （本机 4M token 微基准；详见 docs/progress/data-perf.md）。数据节的 token
  // 形态却高度受限（十进制小数，偶带指数），因此走一条认得出的快路有意义。
  //
  // 快路只接受   [+-]? digits [. digits]? ( [eE] [+-]? digits )?
  // 且额外要求两条**正确性不变量**：
  //   1. 有效数字 ≤ 15 位 ⇒ 尾数 < 2^53，作为 double 精确无误差；
  //   2. 合成后的十进制指数落在 [-22, 22] ⇒ 所需的 10 的幂自身可精确表示。
  // 两条同时成立时，结果由**一次 IEEE 运算**作用于两个精确可表示的操作数得到，
  // 即 IEEE 正确舍入结果，与任何正确舍入的 strtod/from_chars 逐位相同。
  // 不满足任一条（含 nan/inf/十六进制/千分位/指数溢出/尾随垃圾/超长位数）
  // 一律回落到原来的 QByteArray::toDouble——**快路永远不会改变取值**。
  //
  // 等价性证据（可复现，非口头保证）：400 万条合成 token + 25 个对抗用例
  // （nan/inf/-inf/1e400/0x10/1,5/1e/..5/1.2.3/空串/前后空格/1e+9999/
  // 1e-22 以下/超 19 位整数/DBL_MAX/.5/5./+7/0.1/0.2/0.3）与
  // QByteArray::toDouble 逐位比对零差异。
  // -------------------------------------------------------------------------
  const double kPow10Table[] = { 1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                 1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
  constexpr quint64 kMaxExactMantissa = 999999999999999ull; // 15 个 9

  double tokenToDouble(const char *begin, const char *end, bool *ok)
  {
    const char *p = begin;
    quint64 mantissa = 0;
    long exp10 = 0;
    int fracDigits = 0;
    int digits = 0;
    bool fallback = false;
    bool negative = false;
    if (p >= end)
    {
      fallback = true;
    }
    else
    {
      if (*p == '-' || *p == '+')
      {
        negative = (*p == '-');
        ++p;
      }
      bool tooBig = false;
      while (p < end && *p >= '0' && *p <= '9')
      {
        mantissa = mantissa * 10ull + static_cast<quint64>(*p - '0');
        ++p;
        ++digits;
        if (mantissa > kMaxExactMantissa)
        {
          tooBig = true;
          break;
        }
      }
      if (!tooBig && p < end && *p == '.')
      {
        ++p;
        while (p < end && *p >= '0' && *p <= '9')
        {
          mantissa = mantissa * 10ull + static_cast<quint64>(*p - '0');
          ++p;
          ++fracDigits;
          ++digits;
          if (mantissa > kMaxExactMantissa)
          {
            tooBig = true;
            break;
          }
        }
      }
      if (tooBig || digits == 0)
        fallback = true;
    }
    if (!fallback && p < end && (*p == 'e' || *p == 'E'))
    {
      ++p;
      bool expNegative = false;
      if (p < end && (*p == '-' || *p == '+'))
      {
        expNegative = (*p == '-');
        ++p;
      }
      if (p >= end || *p < '0' || *p > '9')
      {
        fallback = true;
      }
      else
      {
        long magnitude = 0;
        while (p < end && *p >= '0' && *p <= '9')
        {
          magnitude = magnitude * 10 + (*p - '0');
          if (magnitude > 10000)
          {
            fallback = true;
            break;
          }
          ++p;
        }
        if (!fallback)
          exp10 = expNegative ? -magnitude : magnitude;
      }
    }
    if (!fallback && p != end)
      fallback = true; // 尾随垃圾：交给原转换器裁定
    if (!fallback)
    {
      const long adjusted = exp10 - static_cast<long>(fracDigits);
      if (adjusted >= -22 && adjusted <= 22)
      {
        double value = static_cast<double>(mantissa);
        const double scale = kPow10Table[adjusted < 0 ? -adjusted : adjusted];
        value = (adjusted < 0) ? (value / scale) : (value * scale);
        if (ok)
          *ok = true;
        return negative ? -value : value;
      }
    }
    return QByteArray::fromRawData(begin, static_cast<int>(end - begin)).toDouble(ok);
  }
} // namespace

bool LasParser::parse(const QString &path, QStringList &curveNames,
                      QList<LasCurve> &curves, QString *error)
{
  curveNames.clear();
  curves.clear();

  QFile f(path);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
  {
    setError(error, QStringLiteral("cannot open %1").arg(path));
    return false;
  }

  enum class Section { None, Version, Well, Curves, Ascii, Other };
  Section section = Section::None;

  double nullValue = paleo::wellnumeric::kLasDefaultNull; // CWLS default when ~W has no usable NULL item
  bool sawAscii = false;
  QStringList names;
  QList<LasCurve> cols;

  QTextStream in(&f);
  bool hitEof = false;
  while (!hitEof && !in.atEnd())
  {
    QString line = in.readLine();
    // #167：DOS 文件尾 Ctrl-Z（0x1A）= EOF，与 parseAsciiRows 同口径。
    if (const qsizetype sub = line.indexOf(QChar(0x1A)); sub >= 0)
    {
      line.truncate(sub);
      hitEof = true;
    }
    line = line.trimmed();
    if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
      continue;

    if (line.startsWith(QLatin1Char('~')))
    {
      const QChar code = line.size() > 1 ? line.at(1).toUpper() : QChar();
      if (code == QLatin1Char('V'))      section = Section::Version;
      else if (code == QLatin1Char('W')) section = Section::Well;
      else if (code == QLatin1Char('C')) section = Section::Curves;
      else if (code == QLatin1Char('A')) { section = Section::Ascii; sawAscii = true; }
      else                               section = Section::Other;
      continue;
    }

    switch (section)
    {
      case Section::Version:
      {
        LasItem it;
        if (parseItemLine(line, it) && it.mnem == QStringLiteral("WRAP") &&
            it.value.startsWith(QStringLiteral("YES"), Qt::CaseInsensitive))
        {
          setError(error, QStringLiteral("wrap mode (WRAP YES) is not supported: %1").arg(path));
          return false;
        }
        break;
      }
      case Section::Well:
      {
        LasItem it;
        if (parseItemLine(line, it) && it.mnem == QStringLiteral("NULL"))
        {
          bool ok = false;
          const double v = it.value.toDouble(&ok);
          if (ok)
            nullValue = v;
        }
        break;
      }
      case Section::Curves:
      {
        LasItem it;
        if (parseItemLine(line, it))
        {
          names.append(it.mnem);
          cols.append({it.mnem, it.unit, it.descr, {}});
        }
        break;
      }
      case Section::Ascii:
      {
        // Whitespace-separated floats in ~C column order; missing/trailing
        // tokens and unparseable cells land as NaN.
        const QStringList tokens =
            line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (tokens.isEmpty())
          break;
        for (int i = 0; i < cols.size(); ++i)
        {
          double v = nan();
          if (i < tokens.size())
          {
            bool ok = false;
            const double t = tokens.at(i).toDouble(&ok);
            if (ok && t != nullValue)
              v = t;
          }
          cols[i].values.append(v);
        }
        break;
      }
      default:
        break;
    }
  }

  if (cols.isEmpty())
  {
    setError(error, QStringLiteral("no curve definitions (~C) found in %1").arg(path));
    return false;
  }
  if (!sawAscii)
  {
    setError(error, QStringLiteral("no ASCII data section (~A) found in %1").arg(path));
    return false;
  }

  curveNames = names;
  curves = cols;
  return true;
}

bool LasParser::parseHeader(const QString &path, LasHeaderInfo &out,
                            QString *error)
{
  out = LasHeaderInfo{};
  out.indexBasis = QStringLiteral("MD"); // LAS 不声明基准，行业惯例 MD（lasparser.h）
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
  {
    setError(error, QStringLiteral("cannot open %1").arg(path));
    return false;
  }

  // 与 parse() 同源的段状态机（Version/Well/Curves）；见到 ~A 段头即停——
  // 数据行（文件体的大头）一个都不读，代价与头部行数成正比。语义对齐：
  // WRAP YES 拒绝、无 ~C 拒绝；~A 缺失不算失败（sawAscii 如实报 false）。
  enum class Section { None, Version, Well, Curves, Other };
  Section section = Section::None;
  bool sawCurves = false;
  QTextStream in(&f);
  while (!in.atEnd())
  {
    const QString line = in.readLine().trimmed();
    if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
      continue;
    if (line.startsWith(QLatin1Char('~')))
    {
      const QChar code = line.size() > 1 ? line.at(1).toUpper() : QChar();
      if (code == QLatin1Char('A'))
      {
        out.sawAscii = true;
        break; // 数据节从此开始——header-only 到此为止
      }
      if (code == QLatin1Char('V'))      section = Section::Version;
      else if (code == QLatin1Char('W')) section = Section::Well;
      else if (code == QLatin1Char('C')) section = Section::Curves;
      else                               section = Section::Other;
      continue;
    }
    LasItem it;
    if (!parseItemLine(line, it))
      continue;
    switch (section)
    {
      case Section::Version:
        if (it.mnem == QStringLiteral("WRAP") &&
            it.value.startsWith(QStringLiteral("YES"), Qt::CaseInsensitive))
        {
          setError(error, QStringLiteral("wrap mode (WRAP YES) is not supported: %1").arg(path));
          return false;
        }
        break;
      case Section::Well:
        if (it.mnem == QStringLiteral("NULL"))
        {
          bool ok = false;
          const double v = it.value.toDouble(&ok);
          if (ok)
            out.nullValue = v;
        }
        else if (it.mnem == QStringLiteral("WELL") && out.wellName.isEmpty())
          out.wellName = it.value;
        break;
      case Section::Curves:
        out.curveNames.append(it.mnem);
        sawCurves = true;
        break;
      default:
        break;
    }
  }
  if (!sawCurves)
  {
    setError(error, QStringLiteral("no curve definitions (~C) found in %1").arg(path));
    return false;
  }
  return true;
}

bool LasParser::readWellInfo(const QString &path, QString &wellName, QString *error)
{
  wellName.clear();

  QFile f(path);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
  {
    setError(error, QStringLiteral("cannot open %1").arg(path));
    return false;
  }

  // 只需 ~W 段的 WELL item——读到 ~C 即止。
  bool inWell = false;
  QTextStream in(&f);
  while (!in.atEnd())
  {
    const QString line = in.readLine().trimmed();
    if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
      continue;
    if (line.startsWith(QLatin1Char('~')))
    {
      const QChar code = line.size() > 1 ? line.at(1).toUpper() : QChar();
      if (code == QLatin1Char('W'))
        inWell = true;
      else if (inWell)
        break; // 离开 ~W，身份字段已收齐
      continue;
    }
    if (!inWell)
      continue;
    LasItem it;
    if (!parseItemLine(line, it))
      continue;
    if (it.mnem == QStringLiteral("WELL") && wellName.isEmpty())
      wellName = it.value;
  }
  return true;
}

// ===========================================================================
// wave/io-perf-cache D1.4-D1.9：字节级快解析 / 区间查询 / 错误分类 / 大文件防护
// ===========================================================================
namespace
{
  void addIssue(QList<LasIssue> *issues, LasIssue::Severity s, LasIssue::Category c,
                int line, const QString &msg)
  {
    if (!issues)
      return;
    LasIssue issue;
    issue.severity = s;
    issue.category = c;
    issue.line = line;
    issue.message = msg;
    issues->append(issue);
  }

  // raw 含 BOM 时 asciiDataOffset 要加回 BOM 长度（stripBom 前的坐标系）。
  qint64 bomAdjustment(const QByteArray &raw)
  {
    if (raw.size() >= 3 && static_cast<uchar>(raw[0]) == 0xEF &&
        static_cast<uchar>(raw[1]) == 0xBB && static_cast<uchar>(raw[2]) == 0xBF)
      return 3;
    return 0;
  }

  struct HeaderScanResult
  {
      LasHeaderInfo header;
      qint64 asciiDataOffset = -1; // ~A 段头行之后首数据行字节偏移（对齐源文件/raw 绝对偏移）；无 ~A = -1
      int bomBytes = 0;            // UTF-8 BOM 长度（0 或 3）
      bool sawWrapYes = false;
      bool sawCurves = false;
      QList<QPair<QString, QString>> curveNameUnits; // (name, unit) 有序
      QString error;                                  // 非空 = 致命
  };

  // 逐原始字节行扫段结构：字节偏移全程对 raw（头段含 GB18030 井名时解码后
  // 字符数 != 字节数，~A 偏移必须按原始字节记——这是 D1.4 区间查询的锚点）。
  HeaderScanResult scanHeaderBytes(const QByteArray &raw, QList<LasIssue> *issues)
  {
    HeaderScanResult out;
    out.header.indexBasis = QStringLiteral("MD"); // LAS 惯例（lasparser.h）
    out.bomBytes = static_cast<int>(bomAdjustment(raw));
    enum class Section { None, Version, Well, Curves, Other };
    Section section = Section::None;

    // 头部编码统一嗅探（GB18030 井名不再乱码，D7.2 联动）。
    const QByteArray noBom = EncodingDetect::stripBom(raw);
    const TextEncoding enc =
        EncodingDetect::detect(noBom.left(qMin<qsizetype>(64 * 1024, noBom.size())));
    if (enc == TextEncoding::GB18030)
      addIssue(issues, LasIssue::Severity::Warning, LasIssue::Category::Encoding, 0,
               QStringLiteral("文件非 UTF-8，按 GB18030 解码头段"));
    else if (enc == TextEncoding::Latin1)
      addIssue(issues, LasIssue::Severity::Warning, LasIssue::Category::Encoding, 0,
               QStringLiteral("文件编码无法识别为 UTF-8/GB18030，按 Latin1 保底解码"));

    const qint64 n = noBom.size();
    qint64 pos = 0;
    int lineNo = 0;
    while (pos < n)
    {
      ++lineNo;
      qint64 eol = pos;
      while (eol < n && noBom.at(eol) != '\n' && noBom.at(eol) != '\r')
        ++eol;
      QByteArray lineBytes = noBom.mid(static_cast<int>(pos), static_cast<int>(eol - pos));
      pos = (eol >= n) ? n : eol + 1;
      if (eol < n && noBom.at(eol) == '\r' && pos < n && noBom.at(pos) == '\n')
        ++pos;

      const QString trimmed = EncodingDetect::decodeText(lineBytes).trimmed();
      if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#')))
        continue;
      if (trimmed.startsWith(QLatin1Char('~')))
      {
        const QChar code = trimmed.size() > 1 ? trimmed.at(1).toUpper() : QChar();
        if (code == QLatin1Char('V'))      section = Section::Version;
        else if (code == QLatin1Char('W')) section = Section::Well;
        else if (code == QLatin1Char('C')) section = Section::Curves;
        else if (code == QLatin1Char('A'))
        {
          out.header.sawAscii = true;
          out.asciiDataOffset = pos + out.bomBytes; // 直接调整为源文件/raw 的绝对字节偏移
          return out;                // 数据节开始——头部扫描到此为止
        }
        else                               section = Section::Other;
        continue;
      }
      LasItem it;
      if (!parseItemLine(trimmed, it))
        continue;
      switch (section)
      {
        case Section::Version:
          if (it.mnem == QStringLiteral("WRAP") &&
              it.value.startsWith(QStringLiteral("YES"), Qt::CaseInsensitive))
          {
            out.sawWrapYes = true;
            out.error = QStringLiteral("wrap mode (WRAP YES) is not supported");
            return out;
          }
          break;
        case Section::Well:
          if (it.mnem == QStringLiteral("NULL"))
          {
            bool ok = false;
            const double v = it.value.toDouble(&ok);
            if (ok)
              out.header.nullValue = v;
          }
          else if (it.mnem == QStringLiteral("WELL") && out.header.wellName.isEmpty())
            out.header.wellName = it.value;
          break;
        case Section::Curves:
        {
          out.header.curveNames.append(it.mnem);
          out.curveNameUnits.append({it.mnem, it.unit});
          out.sawCurves = true;
          // D1.6 单位缺失分级：~C 行没有单位不算错（CWLS 允许），值得提示。
          if (it.unit.isEmpty())
            addIssue(issues, LasIssue::Severity::Info, LasIssue::Category::MissingUnit, lineNo,
                     QStringLiteral("曲线 %1 未声明单位").arg(it.mnem));
          break;
        }
        default:
          break;
      }
    }
    return out;
  }

} // namespace

bool LasParser::scanSections(const QByteArray &raw, SectionMap *out, QList<LasIssue> *issues)
{
  out->ok = false;
  out->error.clear();
  const HeaderScanResult h = scanHeaderBytes(raw, issues);
  out->header = h.header;
  out->asciiDataOffset = h.asciiDataOffset;
  out->sawWrapYes = h.sawWrapYes;
  if (!h.error.isEmpty())
  {
    out->error = h.error;
    addIssue(issues, LasIssue::Severity::Error, LasIssue::Category::WrapMode, 0, h.error);
    return false;
  }
  if (!h.sawCurves)
  {
    out->error = QStringLiteral("no curve definitions (~C) found");
    addIssue(issues, LasIssue::Severity::Error, LasIssue::Category::Format, 0, out->error);
    return false;
  }
  out->ok = true;
  return true;
}

double LasParser::depthUnitToMeters(const QString &unit)
{
  const QString u = unit.trimmed().toUpper();
  if (u == QLatin1String("M") || u == QLatin1String("METER") || u == QLatin1String("METRE") ||
      u == QLatin1String("METERS") || u == QLatin1String("METRES"))
    return 1.0;
  if (u == QLatin1String("FT") || u == QLatin1String("F") || u == QLatin1String("FEET") ||
      u == QLatin1String("FOOT"))
    return 0.3048;
  return 0.0;
}

QList<LasCurve> LasParser::parseAsciiRows(const QByteArray &raw, qint64 asciiOffset,
                                          const QStringList &names, double nullValue,
                                          qint64 rowFrom, qint64 rowTo, qint64 *rowsTotal,
                                          QList<LasIssue> *issues)
{
  QList<LasCurve> cols;
  cols.reserve(names.size());
  for (const QString &n : names)
    cols.append({n, QString(), QString(), {}});

  const int nCurves = cols.size();
  qint64 i = qMax<qint64>(0, asciiOffset);
  // #167：DOS 文件尾 Ctrl-Z（0x1A）= EOF——其后（含该行）不是数据。
  qint64 n = raw.size();
  if (i < n)
    if (const void *sub = std::memchr(raw.constData() + i, 0x1A, static_cast<std::size_t>(n - i)))
      n = static_cast<const char *>(sub) - raw.constData();
  qint64 row = 0;
  QVector<bool> sawFinite(nCurves, false);
  qint64 truncatedRows = 0;
  const bool wantAll = rowFrom <= 0 && rowTo <= 0;
  const qint64 from = rowFrom < 0 ? 0 : rowFrom;
  const qint64 to = rowTo < 0 ? std::numeric_limits<qint64>::max() : rowTo;

  // 列桶：逐值走 cols[col].values.append() 会先做一次
  // QList<LasCurve>::operator[] → data() → detach 检查（每 token 一次，1M 行
  // 5 列即 5M 次不必要的成员间接 +=）；换成定长 vector 直接下标后，末尾一次性
  // 搬回结果。QVector<double> 仍是结果类型，语义与 QList 完全一致。
  std::vector<QVector<double>> buckets(static_cast<std::size_t>(std::max(0, nCurves)));
  {
    // 行数估计（前 64KB 的行密度外推）→ 预留；估偏只会多用虚拟地址，不改结果。
    qint64 estimated = 0;
    const qint64 sampleEnd = qMin<qint64>(n, i + 65536);
    qint64 sampleRows = 0;
    for (qint64 s = i; s < sampleEnd; ++s)
      if (raw.at(s) == '\n')
        ++sampleRows;
    if (sampleRows > 0 && sampleEnd > i)
      estimated = static_cast<qint64>(static_cast<double>(n - i) *
                                      (static_cast<double>(sampleRows) /
                                       static_cast<double>(sampleEnd - i)));
    if (wantAll && estimated > 0)
      estimated = qMin<qint64>(estimated + 16, 64 * 1000 * 1000);
    else if (!wantAll)
      estimated = qMin<qint64>(to - from + 1, 64 * 1000 * 1000);
    if (estimated > 0)
      for (QVector<double> &bucket : buckets)
        bucket.reserve(static_cast<qsizetype>(estimated));
  }

  // 行字节扫描走裸指针：QByteArray::at() 每次都要过一层 Q_ASSERT 包装，
  // 5M token × 若干次比较的放大不划算。
  const char *lineBase = (n > 0) ? raw.constData() : nullptr;

  auto keep = [&](qint64 r) { return wantAll || (r >= from && r < to); };

  while (i < n)
  {
    qint64 eol = i;
    while (eol < n && lineBase[eol] != '\n' && lineBase[eol] != '\r')
      ++eol;
    qint64 firstTok = i;
    while (firstTok < eol && (lineBase[firstTok] == ' ' || lineBase[firstTok] == '\t'))
      ++firstTok;
    // #167：纯空白行与空行同义（不出数据行）——否则补满 NaN 成「深度也是 NaN」的幽灵行。
    if (firstTok < eol)
    {
      int col = 0;
      qint64 p = firstTok;
      while (p < eol && col < nCurves)
      {
        while (p < eol && (lineBase[p] == ' ' || lineBase[p] == '\t'))
          ++p;
        if (p >= eol)
          break;
        qint64 q = p;
        while (q < eol && lineBase[q] != ' ' && lineBase[q] != '\t')
          ++q;
        bool ok = false;
        const double v = tokenToDouble(lineBase + p, lineBase + q, &ok);
        if (ok && v == v && v != nullValue)
        {
          sawFinite[col] = true;
          if (keep(row))
            buckets[static_cast<std::size_t>(col)].append(v);
        }
        else if (keep(row))
        {
          buckets[static_cast<std::size_t>(col)].append(nan());
        }
        ++col;
        p = q;
      }
      if (col < nCurves)
      {
        ++truncatedRows;
        if (keep(row))
          for (; col < nCurves; ++col)
            buckets[static_cast<std::size_t>(col)].append(nan());
      }
    }
    ++row;
    if (eol >= n)
      break; // 尾行无行界符——处理完即结束
    i = eol + 1;
    if (lineBase[eol] == '\r' && i < n && lineBase[i] == '\n')
      ++i;
    if (!wantAll && row >= to)
      break; // 区间查询：越过 to 即停（D1.4）
  }
  for (int c = 0; c < nCurves; ++c)
    cols[c].values = std::move(buckets[static_cast<std::size_t>(c)]);
  *rowsTotal = row;
  if (truncatedRows > 0)
    addIssue(issues, LasIssue::Severity::Warning, LasIssue::Category::Truncated, 0,
             QStringLiteral("%1 行 token 少于曲线数（缺失列记 NaN）").arg(truncatedRows));
  // D1.9 空曲线统一策略：保留全 NaN 列（列数恒等于曲线数），发 Info。
  if (wantAll)
    for (int c = 0; c < nCurves; ++c)
      if (!sawFinite[c])
        addIssue(issues, LasIssue::Severity::Info, LasIssue::Category::MissingCurve, 0,
                 QStringLiteral("曲线 %1 全列无有效值（空曲线保留为 NaN 列）").arg(names.at(c)));
  return cols;
}

LasDoc LasParser::parseDoc(const QString &path, QList<LasIssue> *issues)
{
  LasDoc doc;
  QFileInfo info(path);
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
  {
    doc.error = QStringLiteral("cannot open %1").arg(path);
    addIssue(issues, LasIssue::Severity::Error, LasIssue::Category::Io, 0, doc.error);
    return doc;
  }
  const qint64 size = f.size();
  if (size > s_fileSizeLimit)
  {
    // D1.8 大文件防护：拒绝整读并点名流式入口。
    doc.error = QStringLiteral("file %1 is %2 bytes — exceeds the %3 MB full-parse guard; "
                               "use parseRange/parseDepthRange streaming instead")
                    .arg(path)
                    .arg(size)
                    .arg(s_fileSizeLimit / (1024 * 1024));
    addIssue(issues, LasIssue::Severity::Error, LasIssue::Category::Oversize, 0, doc.error);
    return doc;
  }
  CacheBudgetManager::instance()->noteLargeAllocation(
      QStringLiteral("LasParser::parseDoc(%1)").arg(info.fileName()), size);
  const QByteArray raw = f.readAll();
  HeaderScanResult header = scanHeaderBytes(raw, issues);
  if (!header.error.isEmpty())
  {
    doc.error = QStringLiteral("%1: %2").arg(path, header.error);
    addIssue(issues, LasIssue::Severity::Error, LasIssue::Category::WrapMode, 0, header.error);
    return doc;
  }
  if (!header.sawCurves)
  {
    doc.error = QStringLiteral("no curve definitions (~C) found in %1").arg(path);
    addIssue(issues, LasIssue::Severity::Error, LasIssue::Category::Format, 0, doc.error);
    return doc;
  }
  if (header.asciiDataOffset < 0)
  {
    doc.error = QStringLiteral("no ASCII data section (~A) found in %1").arg(path);
    addIssue(issues, LasIssue::Severity::Error, LasIssue::Category::Format, 0, doc.error);
    return doc;
  }
  qint64 rowsTotal = 0;
  const qint64 asciiOff = header.asciiDataOffset;
  doc.curves = parseAsciiRows(raw, asciiOff, header.header.curveNames,
                              header.header.nullValue, 0, -1, &rowsTotal, issues);
  for (int i = 0; i < doc.curves.size(); ++i)
    doc.curves[i].unit =
        i < header.curveNameUnits.size() ? header.curveNameUnits.at(i).second : QString();
  doc.curveNames = header.header.curveNames;
  doc.ok = true;
  return doc;
}

namespace
{
  // parseRange/parseDepthRange 共用：头部扫描 + 公共拒因。
  bool scanHeaderFromFile(QFile &f, HeaderScanResult *header, QString *error,
                          QList<LasIssue> *issues, const QString &path)
  {
    // 头部按需读（不整读）：头段通常 < 64KB，最大扫 4MB 兜住病态长头。
    const QByteArray head = f.read(4 * 1024 * 1024);
    *header = scanHeaderBytes(head, issues);
    if (!header->error.isEmpty())
    {
      if (error)
        *error = QStringLiteral("%1: %2").arg(path, header->error);
      return false;
    }
    if (!header->sawCurves)
    {
      if (error)
        *error = QStringLiteral("no curve definitions (~C) found in %1").arg(path);
      return false;
    }
    if (header->asciiDataOffset < 0)
    {
      if (error)
        *error = QStringLiteral("no ASCII data section (~A) found in %1").arg(path);
      return false;
    }
    return true;
  }

  void fillUnits(QList<LasCurve> *curves, const HeaderScanResult &header)
  {
    for (int i = 0; i < curves->size(); ++i)
      (*curves)[i].unit =
          i < header.curveNameUnits.size() ? header.curveNameUnits.at(i).second : QString();
  }
} // namespace

bool LasParser::parseRange(const QString &path, qint64 rowFrom, qint64 rowTo,
                           QStringList &curveNames, QList<LasCurve> &curves,
                           QString *error, QList<LasIssue> *issues)
{
  curveNames.clear();
  curves.clear();
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
  {
    if (error)
      *error = QStringLiteral("cannot open %1").arg(path);
    addIssue(issues, LasIssue::Severity::Error, LasIssue::Category::Io, 0,
             QStringLiteral("cannot open %1").arg(path));
    return false;
  }
  HeaderScanResult header;
  if (!scanHeaderFromFile(f, &header, error, issues, path))
    return false;
  const QStringList &names = header.header.curveNames;
  // asciiDataOffset 在 scanHeader 内已含 BOM 补偿（绝对字节偏移）——勿再加。
  const qint64 asciiOff = header.asciiDataOffset;

  if (f.size() <= 8 * 1024 * 1024)
  {
    // 小文件：整读（一次盘 IO 比流式拼块省）。头部扫描后位置在 EOF——回卷。
    f.seek(0);
    const QByteArray raw = f.readAll();
    qint64 rowsTotal = 0;
    curves = parseAsciiRows(raw, asciiOff, names, header.header.nullValue,
                            rowFrom, rowTo, &rowsTotal, issues);
    fillUnits(&curves, header);
    curveNames = names;
    return true;
  }

  // 大文件流式：8MB 块推进；块尾半行留给下一块（D1.6 Truncated 口径不受污染）。
  const qint64 fileSize = f.size();
  qint64 pos = asciiOff;
  qint64 row = 0;
  const int nCurves = names.size();
  QList<LasCurve> cols;
  for (const QString &n : names)
    cols.append({n, QString(), QString(), {}});
  QVector<bool> sawFinite(nCurves, false);
  const qint64 from = qMax<qint64>(0, rowFrom);
  const qint64 to = rowTo < 0 ? std::numeric_limits<qint64>::max() : rowTo;
  while (pos < fileSize)
  {
    f.seek(pos);
    QByteArray chunk = f.read(8 * 1024 * 1024);
    if (chunk.isEmpty())
      break;
    qint64 usable = chunk.size();
    if (pos + usable < fileSize)
    {
      const qint64 lastSep = qMax<qint64>(chunk.lastIndexOf('\n'), chunk.lastIndexOf('\r'));
      if (lastSep < 0)
        usable = 0; // 单行超过 8MB——病态，放弃（如实返回已收行）
      else
        usable = lastSep + 1;
    }
    if (usable <= 0)
      break;
    const QByteArray complete = chunk.left(static_cast<int>(usable));
    qint64 rowsInChunk = 0;
    const QList<LasCurve> piece = parseAsciiRows(complete, 0, names, header.header.nullValue,
                                                 0, -1, &rowsInChunk, nullptr);
    for (int c = 0; c < nCurves; ++c)
    {
      const QVector<double> &vals = piece.at(c).values;
      for (int r = 0; r < vals.size(); ++r)
      {
        const qint64 globalRow = row + r;
        if (vals.at(r) == vals.at(r))
          sawFinite[c] = true;
        if (globalRow >= from && globalRow < to)
          cols[c].values.append(vals.at(r));
      }
    }
    row += rowsInChunk;
    pos += usable;
    if (row >= to)
      break;
  }
  for (int c = 0; c < nCurves; ++c)
    if (!sawFinite[c])
      addIssue(issues, LasIssue::Severity::Info, LasIssue::Category::MissingCurve, 0,
               QStringLiteral("曲线 %1 在请求区间内无有效值").arg(names.at(c)));
  curves = cols;
  fillUnits(&curves, header);
  curveNames = names;
  return true;
}

bool LasParser::parseDepthRange(const QString &path, double fromDepth, double toDepth,
                                QStringList &curveNames, QList<LasCurve> &curves,
                                QString *error, QList<LasIssue> *issues)
{
  curveNames.clear();
  curves.clear();
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
  {
    if (error)
      *error = QStringLiteral("cannot open %1").arg(path);
    return false;
  }
  HeaderScanResult header;
  if (!scanHeaderFromFile(f, &header, error, issues, path))
    return false;
  const QStringList &names = header.header.curveNames;
  if (names.isEmpty())
  {
    if (error)
      *error = QStringLiteral("no curves in %1").arg(path);
    return false;
  }
  const int nCurves = names.size();
  QList<LasCurve> cols;
  for (const QString &n : names)
    cols.append({n, QString(), QString(), {}});
  const double nullValue = header.header.nullValue;
  bool monotonicDepth = true;
  double lastDepth = -std::numeric_limits<double>::infinity();

  // 流式逐行：DEPT 落在 [from,to] 内的行收；DEPT 单调递增时越过 to 即停。
  const qint64 fileSize = f.size();
  // asciiDataOffset 已是含 BOM 补偿的绝对偏移——勿再 peek+加。
  qint64 pos = header.asciiDataOffset;
  // #78：块尾半行不留 carry——下一块直接从 pos + usable（即半行起点）重读。
  // 旧实现 carry + 从 base+usable 重读，会把半行字节拼两次：块边界行错列，
  // 截断点落在深度 token 中间时还会拼出越界深度、触发「越界即停」静默截断。
  constexpr qint64 kChunk = 4 * 1024 * 1024;
  bool stopped = false;
  std::vector<double> rowVals(nCurves);
  while (pos < fileSize && !stopped)
  {
    f.seek(pos);
    const QByteArray chunk = f.read(kChunk);
    if (chunk.isEmpty())
      break;
    qint64 usable = chunk.size();
    if (pos + chunk.size() < fileSize)
    {
      const qint64 lastSep = qMax<qint64>(chunk.lastIndexOf('\n'), chunk.lastIndexOf('\r'));
      if (lastSep < 0)
      {
        // 单行超过块长——病态输入：如实返回已收行，但留痕（不静默成功）。
        addIssue(issues, LasIssue::Severity::Warning, LasIssue::Category::Truncated, 0,
                 QStringLiteral("数据行超过 %1 字节，按深度读段在偏移 %2 处中止")
                     .arg(kChunk)
                     .arg(pos));
        usable = 0;
      }
      else
        usable = lastSep + 1;
    }
    if (usable <= 0)
      break;
    qint64 i = 0;
    while (i < usable)
    {
      qint64 eol = i;
      while (eol < usable && chunk.at(eol) != '\n' && chunk.at(eol) != '\r')
        ++eol;
      if (eol > i)
      {
        std::vector<double> rowVals(nCurves, nan());
        int col = 0;
        qint64 p = i;
        while (p < eol && col < nCurves)
        {
          while (p < eol && (chunk.at(p) == ' ' || chunk.at(p) == '\t'))
            ++p;
          if (p >= eol)
            break;
          qint64 q = p;
          while (q < eol && chunk.at(q) != ' ' && chunk.at(q) != '\t')
            ++q;
          bool ok = false;
          rowVals[col++] = tokenToDouble(chunk.constData() + p, chunk.constData() + q, &ok);
          if (!ok)
            rowVals[col - 1] = nan();
          p = q;
        }
        if (col > 0)
        {
          const double depth = rowVals[0];
          if (depth < lastDepth)
            monotonicDepth = false;
          lastDepth = depth;
          if (depth == depth && depth >= fromDepth && depth <= toDepth)
            for (int c = 0; c < nCurves; ++c)
            {
              const double v = c < col ? rowVals[c] : nan();
              cols[c].values.append(v == nullValue || v != v ? nan() : v);
            }
          else if (monotonicDepth && depth == depth && depth > toDepth)
          {
            stopped = true; // 越界即停——真·按需读段（D1.4）
            break;
          }
        }
      }
      i = eol + 1;
      if (eol < usable && chunk.at(eol) == '\r' && i < usable && chunk.at(i) == '\n')
        ++i;
    }
    pos += usable;
  }
  curves = cols;
  fillUnits(&curves, header);
  curveNames = names;
  return true;
}
