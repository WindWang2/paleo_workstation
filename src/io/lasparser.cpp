// 层：数据
#include "lasparser.h"
#include "../domain/wellnumeric.h"
#include "ioerrors_internal.h"
#include "../domain/wellnumeric.h"

#include "cachebudget.h"
#include "encodingdetect.h"

#include <QFile>
#include <QBuffer>
#include <QFileInfo>
#include <QRegularExpression>

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

// #466：旧入口薄封装 parseDoc——防护与编码口径单源化（此前旧入口用
// QTextStream 逐行全量装载：无 CacheBudgetManager 登记、无 GB18030 头段
// 嗅探，同一文件与 parseDoc 解出不同曲线名/井名）。签名与返回语义不变：
// 成功填 curveNames/curves（含 unit/descr，与旧实现一致），失败置 *error
// 并透传 parseDoc 的拒因（cannot open / Oversize 大文件闸 / WRAP YES /
// 无 ~C / 无 ~A 文案同源）。已知口径变化（病态输入）：头段含 0x1A（DOS
// Ctrl-Z）不再按 EOF 截停头扫描——parseDoc 只在 ~A 数据区认 0x1A。
bool LasParser::parse(const QString &path, QStringList &curveNames,
                      QList<LasCurve> &curves, QString *error)
{
  curveNames.clear();
  curves.clear();
  const LasDoc doc = parseDoc(path);
  if (!doc.ok)
  {
    setError(error, doc.error);
    return false;
  }
  curveNames = doc.curveNames;
  curves = doc.curves;
  return true;
}

namespace
{
  // PW-016/017：行式头部读取（parseHeader/readWellInfo）的统一入口——
  // 文件级嗅探一次编码（不再逐行嗅探）；UTF-16 BOM 文件整体转码为 UTF-8
  // 后再按行读（逐字节 readLine 会把 UTF-16 切碎）。
  struct LasLineSource
  {
    QIODevice *dev = nullptr;
    QBuffer buffer;
    TextEncoding enc = TextEncoding::Utf8;
  };

  bool openLasLineSource(QFile &f, LasLineSource *src, qint64 sizeLimit, QString *err)
  {
    const QByteArray sample = f.peek(64 * 1024);
    if (EncodingDetect::hasUtf16Bom(sample))
    {
      if (f.size() > sizeLimit)
      {
        *err = QStringLiteral("UTF-16 LAS exceeds full-transcode guard: %1").arg(f.fileName());
        return false;
      }
      src->buffer.setData(EncodingDetect::stripBom(f.readAll())); // 转码为 UTF-8
      src->buffer.open(QIODevice::ReadOnly);
      src->dev = &src->buffer;
      src->enc = TextEncoding::Utf8;
      return true;
    }
    src->dev = &f;
    src->enc = EncodingDetect::detect(EncodingDetect::stripBom(sample));
    return true;
  }
} // namespace

bool LasParser::parseHeader(const QString &path, LasHeaderInfo &out,
                            QString *error)
{
  out = LasHeaderInfo{};
  out.indexBasis = QStringLiteral("MD"); // LAS 不声明基准，行业惯例 MD（lasparser.h）
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
  {
    setError(error, QStringLiteral("cannot open %1").arg(path));
    return false;
  }
  LasLineSource src;
  {
    QString err;
    if (!openLasLineSource(f, &src, s_fileSizeLimit, &err))
    {
      setError(error, err);
      return false;
    }
  }

  // 与 parse() 同源的段状态机（Version/Well/Curves）；见到 ~A 段头即停——
  // 数据行（文件体的大头）一个都不读，代价与头部行数成正比。语义对齐：
  // WRAP YES 拒绝、无 ~C 拒绝；~A 缺失不算失败（sawAscii 如实报 false）。
  enum class Section { None, Version, Well, Curves, Other };
  Section section = Section::None;
  bool sawCurves = false;
  // #588：逐行原始字节 + EncodingDetect 解码（与 parseDoc 的 scanHeaderBytes
  // 同口径）。QTextStream 默认按 UTF-8 解，GB18030 的 LAS 在这里解出的井名
  // 是乱码——wellsMatchingName 零匹配，井曲线行全落「未决」链接。
  while (!src.dev->atEnd())
  {
    const QString line = EncodingDetect::decodeAs(src.dev->readLine(), src.enc).trimmed();
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
        // PW-069：WRAP YES 由数据解析支持，头信息如实记录。
        if (it.mnem == QStringLiteral("WRAP") &&
            it.value.startsWith(QStringLiteral("YES"), Qt::CaseInsensitive))
          out.wrap = true;
        else if (it.mnem == QStringLiteral("DLM") &&
                 it.value.trimmed().compare(QStringLiteral("COMMA"), Qt::CaseInsensitive) == 0)
          out.delimiter = ',';
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
  if (!f.open(QIODevice::ReadOnly))
  {
    setError(error, QStringLiteral("cannot open %1").arg(path));
    return false;
  }
  LasLineSource src;
  {
    QString err;
    if (!openLasLineSource(f, &src, s_fileSizeLimit, &err))
    {
      setError(error, err);
      return false;
    }
  }

  // 只需 ~W 段的 WELL item——读到 ~C 即止。
  bool inWell = false;
  // #588：同 parseHeader/parseDoc——QTextStream 默认 UTF-8 会把 GB18030
  // 井名解成乱码（wellsMatchingName 随之零匹配）。逐行原始字节 +
  // EncodingDetect 解码，口径单源化。
  while (!src.dev->atEnd())
  {
    const QString line = EncodingDetect::decodeAs(src.dev->readLine(), src.enc).trimmed();
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
      QStringList curveDescrs;                       // ~C 行 descr（与 curveNameUnits 同序）
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
    // PW-017：UTF-16 下 stripBom 会转码，pos 与 raw 字节偏移不再同一坐标系——
    // 调用方须先整体转码（parseDoc）或拒绝（区间流式读）。此处兜底拒绝。
    if (EncodingDetect::hasUtf16Bom(raw))
    {
      out.error = QStringLiteral("UTF-16 encoded LAS must be transcoded before header scan");
      return out;
    }
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

      // PW-016：用文件级 enc 解码，不逐行再嗅探。
      const QString trimmed = EncodingDetect::decodeAs(lineBytes, enc).trimmed();
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
          // PW-069：WRAP YES / DLM 进头信息，数据段按其解析（不再拒绝）。
          if (it.mnem == QStringLiteral("WRAP") &&
              it.value.startsWith(QStringLiteral("YES"), Qt::CaseInsensitive))
          {
            out.sawWrapYes = true;
            out.header.wrap = true;
          }
          else if (it.mnem == QStringLiteral("DLM"))
          {
            const QString d = it.value.trimmed().toUpper();
            if (d == QStringLiteral("COMMA"))
              out.header.delimiter = ',';
            else if (d == QStringLiteral("TAB") || d == QStringLiteral("SPACE"))
              out.header.delimiter = 0; // 空白类与默认切分同口径
            else if (!d.isEmpty())
            {
              out.error = QStringLiteral("unsupported LAS delimiter DLM=%1").arg(it.value);
              return out;
            }
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
          // #466：descr 与 name/unit 同源捕获——旧 parse() 的返回语义含 descr，
          // 薄封装 parseDoc 后此处是 descr 的唯一来源。
          out.curveDescrs.append(it.descr);
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
  // 委托统一词表（PW-029）；空/未知单位 = 0.0（无效），语义不变。
  return paleo::wellnumeric::depthScaleToMetres(unit).value_or(0.0);
}

QList<LasCurve> LasParser::parseAsciiRows(const QByteArray &raw, qint64 asciiOffset,
                                          const QStringList &names, double nullValue,
                                          qint64 rowFrom, qint64 rowTo, qint64 *rowsTotal,
                                          QList<LasIssue> *issues, bool wrap, char delimiter)
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
    // L-24：外推只按「首 64KB 密度 × 全文件长」估计——病态文件（前密后疏
    // + 数十条曲线）可对每桶 reserve 512MB（Windows 大块堆分配即
    // MEM_COMMIT），桶数相乘达数十 GB 提交。按全部桶合计 ≤1GiB 封顶；
    // 真实行数超估计时 QVector 自然扩容，只多几次 realloc。
    if (estimated > 0 && !buckets.empty())
      estimated = qMin<qint64>(
          estimated, qMax<qint64>(1024, (qint64(1) << 30) / (8 * buckets.size())));
    if (estimated > 0)
      for (QVector<double> &bucket : buckets)
        bucket.reserve(static_cast<qsizetype>(estimated));
  }

  // 行字节扫描走裸指针：QByteArray::at() 每次都要过一层 Q_ASSERT 包装，
  // 5M token × 若干次比较的放大不划算。
  const char *lineBase = (n > 0) ? raw.constData() : nullptr;

  auto keep = [&](qint64 r) { return wantAll || (r >= from && r < to); };

  // PW-069：WRAP YES——一行数据可折成多物理行，token 流连续收满 nCurves 才成
  // 一行；DLM=COMMA——逗号切列（空字段 = NaN）；非 wrap 行多余 token 计数告警。
  const bool comma = (delimiter == ',');
  qint64 extraTokenRows = 0;
  int col = 0; // wrap 模式跨物理行延续
  auto pushToken = [&](const char *b, const char *e) {
    bool ok = false;
    const double v = (b < e) ? tokenToDouble(b, e, &ok) : nan();
    if (ok && v == v && v != nullValue)
    {
      sawFinite[col] = true;
      if (keep(row))
        buckets[static_cast<std::size_t>(col)].append(v);
    }
    else if (keep(row))
      buckets[static_cast<std::size_t>(col)].append(nan());
    ++col;
  };
  auto isWs = [](char c) { return c == ' ' || c == '\t'; };

  while (i < n)
  {
    qint64 eol = i;
    while (eol < n && lineBase[eol] != '\n' && lineBase[eol] != '\r')
      ++eol;
    qint64 firstTok = i;
    while (firstTok < eol && isWs(lineBase[firstTok]))
      ++firstTok;
    // #167：纯空白行与空行同义（不出数据行）——否则补满 NaN 成「深度也是 NaN」的幽灵行。
    if (firstTok < eol && nCurves > 0)
    {
      if (!wrap)
        col = 0;
      bool extra = false;
      qint64 p = firstTok;
      while (p < eol)
      {
        qint64 b, e;
        if (comma)
        {
          qint64 q = p;
          while (q < eol && lineBase[q] != ',')
            ++q;
          b = p;
          e = q;
          while (b < e && isWs(lineBase[b])) ++b;
          while (e > b && isWs(lineBase[e - 1])) --e;
          p = (q < eol) ? q + 1 : eol;
        }
        else
        {
          while (p < eol && isWs(lineBase[p]))
            ++p;
          if (p >= eol)
            break;
          qint64 q = p;
          while (q < eol && !isWs(lineBase[q]))
            ++q;
          b = p;
          e = q;
          p = q;
        }
        if (col >= nCurves)
        {
          extra = true; // 非 wrap：多余 token（旧实现静默丢弃）
          break;
        }
        pushToken(lineBase + b, lineBase + e);
        if (wrap && col == nCurves)
        {
          ++row;
          col = 0;
        }
      }
      if (extra)
        ++extraTokenRows;
      if (!wrap)
      {
        if (col < nCurves)
        {
          ++truncatedRows;
          if (keep(row))
            for (int c = col; c < nCurves; ++c)
              buckets[static_cast<std::size_t>(c)].append(nan());
        }
        // #386：row 只计数据行——物理空行不计。流式 parseRange 的
        // globalRow = row + 数据值下标依赖同一单位口径，空行计数会让
        // 之后每个行窗偏移（整读/流式两路径结果不一致）。
        ++row;
      }
    }
    if (eol >= n)
      break; // 尾行无行界符——处理完即结束
    i = eol + 1;
    if (lineBase[eol] == '\r' && i < n && lineBase[i] == '\n')
      ++i;
    if (!wantAll && row >= to)
      break; // 区间查询：越过 to 即停（D1.4）
  }
  if (wrap && col > 0)
  {
    // 折行数据在文件尾不足一整行：补 NaN 记一截断行。
    ++truncatedRows;
    if (keep(row))
      for (int c = col; c < nCurves; ++c)
        buckets[static_cast<std::size_t>(c)].append(nan());
    ++row;
  }
  if (extraTokenRows > 0)
    addIssue(issues, LasIssue::Severity::Warning, LasIssue::Category::Format, 0,
             QStringLiteral("%1 行 token 多于曲线数（多余值已忽略）").arg(extraTokenRows));
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
  QByteArray raw = f.readAll();
  if (EncodingDetect::hasUtf16Bom(raw))
  {
    // PW-017：UTF-16 整体转码为 UTF-8，头扫描与数据行同一缓冲/同一偏移系。
    raw = EncodingDetect::stripBom(raw);
    addIssue(issues, LasIssue::Severity::Warning, LasIssue::Category::Encoding, 0,
             QStringLiteral("UTF-16 文件已整体转码为 UTF-8 解析"));
  }
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
                              header.header.nullValue, 0, -1, &rowsTotal, issues,
                              header.header.wrap, header.header.delimiter);
  for (int i = 0; i < doc.curves.size(); ++i)
  {
    doc.curves[i].unit =
        i < header.curveNameUnits.size() ? header.curveNameUnits.at(i).second : QString();
    // #466：descr 同源填充——旧 parse() 语义含 descr，薄封装后由此带出。
    doc.curves[i].descr =
        i < header.curveDescrs.size() ? header.curveDescrs.at(i) : QString();
  }
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
    if (EncodingDetect::hasUtf16Bom(head))
    {
      // PW-017：区间读按源文件字节偏移寻址，UTF-16 无法与转码缓冲对齐——明确拒绝。
      if (error)
        *error = QStringLiteral("%1: UTF-16 encoded LAS is not supported by range reads; "
                                "use parseDoc or re-save as UTF-8").arg(path);
      return false;
    }
    *header = scanHeaderBytes(head, issues);
    if (!header->error.isEmpty())
    {
      if (error)
        *error = QStringLiteral("%1: %2").arg(path, header->error);
      return false;
    }
    // L-24：头扫描截断（4MB 上限读满仍未命中）与「文件真的没有」必须分
    // 开归因——否则 ~A 位于 4MB 之后的合法长头文件被报成「无 ~A」，而
    // parseDoc（500MB 预算）又能成功，两入口对同一文件结论矛盾。
    const bool headScanTruncated =
        head.size() >= 4 * 1024 * 1024 && f.size() > head.size();
    if (!header->sawCurves)
    {
      if (error)
        *error = headScanTruncated
                     ? QStringLiteral("header scan hit the 4 MB limit in %1 before curve "
                                      "definitions (~C); use whole-file parse or re-save")
                           .arg(path)
                     : QStringLiteral("no curve definitions (~C) found in %1").arg(path);
      return false;
    }
    if (header->asciiDataOffset < 0)
    {
      if (error)
        *error = headScanTruncated
                     ? QStringLiteral("header scan hit the 4 MB limit in %1 before the ASCII "
                                      "data section (~A); use whole-file parse or re-save")
                           .arg(path)
                     : QStringLiteral("no ASCII data section (~A) found in %1").arg(path);
      return false;
    }
    return true;
  }

  void fillUnits(QList<LasCurve> *curves, const HeaderScanResult &header)
  {
    for (int i = 0; i < curves->size(); ++i)
    {
      (*curves)[i].unit =
          i < header.curveNameUnits.size() ? header.curveNameUnits.at(i).second : QString();
      (*curves)[i].descr =
          i < header.curveDescrs.size() ? header.curveDescrs.at(i) : QString();
    }
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

  // PW-069：折行/逗号分隔的数据行不能按物理行分块——走整读（受整读上限约束）。
  const bool needsWhole = header.header.wrap || header.header.delimiter != 0;
  if (needsWhole && f.size() > s_fileSizeLimit)
  {
    if (error)
      *error = QStringLiteral("%1: WRAP/DLM LAS larger than full-read limit").arg(path);
    addIssue(issues, LasIssue::Severity::Error, LasIssue::Category::WrapMode, 0,
             QStringLiteral("WRAP/DLM LAS larger than full-read limit"));
    return false;
  }
  if (needsWhole || f.size() <= 8 * 1024 * 1024)
  {
    // 小文件：整读（一次盘 IO 比流式拼块省）。头部扫描后位置在 EOF——回卷。
    f.seek(0);
    const QByteArray raw = f.readAll();
    qint64 rowsTotal = 0;
    curves = parseAsciiRows(raw, asciiOff, names, header.header.nullValue,
                            rowFrom, rowTo, &rowsTotal, issues,
                            header.header.wrap, header.header.delimiter);
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
      {
        // 单行超过 8MB——病态，放弃后续（如实返回已收行），但不静默：
        // 与 parseDepthRange 同口径落 Truncated 留痕（#477）。
        addIssue(issues, LasIssue::Severity::Warning, LasIssue::Category::Truncated, 0,
                 QStringLiteral("数据行超过 %1 字节，区间读段在偏移 %2 处中止")
                     .arg(8 * 1024 * 1024)
                     .arg(pos));
        usable = 0;
      }
      else
        usable = lastSep + 1;
    }
    // #167 同口径：DOS Ctrl-Z（0x1A）= EOF——整读路径在其后截停，流式
    // 路径此前完全不认（同一文件两入口行集不一致）。块内命中即截去其后
    // 字节并停止推进后续块。
    bool sawCtrlZ = false;
    if (const void *eofMarker = std::memchr(chunk.constData(), 0x1A, static_cast<std::size_t>(usable)))
    {
      usable = static_cast<const char *>(eofMarker) - chunk.constData();
      sawCtrlZ = true;
    }
    if (usable <= 0)
      break;
    const QByteArray complete = chunk.left(static_cast<int>(usable));
    qint64 rowsInChunk = 0;
    // #477：流式路径不再把 issues 传 nullptr——每块内「token 少于曲线数」
    // 的 Truncated 告警必须如实透传，>8MB 文件的截断行不再静默丢失。只
    // 转发 Truncated：块级 parseAsciiRows 以 wantAll 口径运行，会按块误报
    // MissingCurve（该类汇总告警由本函数末尾按全文件 sawFinite 统一口径
    // 发出，见下）。
    QList<LasIssue> chunkIssues;
    const QList<LasCurve> piece = parseAsciiRows(complete, 0, names, header.header.nullValue,
                                                 0, -1, &rowsInChunk, &chunkIssues);
    for (const LasIssue &issue : chunkIssues)
      if (issue.category == LasIssue::Category::Truncated)
        addIssue(issues, issue.severity, issue.category, issue.line, issue.message);
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
    if (sawCtrlZ)
      break;
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

  // PW-069：WRAP/DLM 走整读 + 共用行解析核，再按深度筛行。
  if (header.header.wrap || header.header.delimiter != 0)
  {
    if (f.size() > s_fileSizeLimit)
    {
      if (error)
        *error = QStringLiteral("%1: WRAP/DLM LAS larger than full-read limit").arg(path);
      return false;
    }
    f.seek(0);
    const QByteArray raw = f.readAll();
    qint64 rowsTotal = 0;
    const QList<LasCurve> all =
        parseAsciiRows(raw, header.asciiDataOffset, names, nullValue, 0, -1, &rowsTotal,
                       issues, header.header.wrap, header.header.delimiter);
    const QVector<double> &depths = all.at(0).values;
    for (int r = 0; r < depths.size(); ++r)
    {
      const double d = depths.at(r);
      if (d == d && d >= fromDepth && d <= toDepth)
        for (int c = 0; c < nCurves; ++c)
          cols[c].values.append(all.at(c).values.value(r, nan()));
    }
    curves = cols;
    fillUnits(&curves, header);
    curveNames = names;
    return true;
  }

  // 流式逐行：DEPT 落在 [from,to] 内的行收；DEPT 单调递增时越过 to 即停。
  const qint64 fileSize = f.size();
  // asciiDataOffset 已是含 BOM 补偿的绝对偏移——勿再 peek+加。
  qint64 pos = header.asciiDataOffset;
  // #78：块尾半行不留 carry——下一块直接从 pos + usable（即半行起点）重读。
  // 旧实现 carry + 从 base+usable 重读，会把半行字节拼两次：块边界行错列，
  // 截断点落在深度 token 中间时还会拼出越界深度、触发「越界即停」静默截断。
  constexpr qint64 kChunk = 4 * 1024 * 1024;
  bool stopped = false;
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
