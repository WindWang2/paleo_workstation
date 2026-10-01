// 层：数据
#include "lasparser.h"

#include "cachebudget.h"
#include "encodingdetect.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>

#include <cmath>
#include <limits>

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
  void setError(QString *error, const QString &text)
  {
    if (error)
      *error = text;
  }

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

  double nullValue = -999.25; // CWLS default when ~W has no usable NULL item
  bool sawAscii = false;
  QStringList names;
  QList<LasCurve> cols;

  QTextStream in(&f);
  while (!in.atEnd())
  {
    const QString line = in.readLine().trimmed();
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
  const qint64 n = raw.size();
  qint64 i = qMax<qint64>(0, asciiOffset);
  qint64 row = 0;
  QVector<bool> sawFinite(nCurves, false);
  qint64 truncatedRows = 0;
  const bool wantAll = rowFrom <= 0 && rowTo <= 0;
  const qint64 from = rowFrom < 0 ? 0 : rowFrom;
  const qint64 to = rowTo < 0 ? std::numeric_limits<qint64>::max() : rowTo;

  auto keep = [&](qint64 r) { return wantAll || (r >= from && r < to); };

  while (i < n)
  {
    qint64 eol = i;
    while (eol < n && raw.at(eol) != '\n' && raw.at(eol) != '\r')
      ++eol;
    if (eol > i)
    {
      int col = 0;
      qint64 p = i;
      while (p < eol && col < nCurves)
      {
        while (p < eol && (raw.at(p) == ' ' || raw.at(p) == '\t'))
          ++p;
        if (p >= eol)
          break;
        qint64 q = p;
        while (q < eol && raw.at(q) != ' ' && raw.at(q) != '\t')
          ++q;
        bool ok = false;
        const double v =
            QByteArray::fromRawData(raw.constData() + p, static_cast<int>(q - p)).toDouble(&ok);
        if (ok && v == v && v != nullValue)
        {
          sawFinite[col] = true;
          if (keep(row))
            cols[col].values.append(v);
        }
        else if (keep(row))
        {
          cols[col].values.append(nan());
        }
        ++col;
        p = q;
      }
      if (col < nCurves)
      {
        ++truncatedRows;
        if (keep(row))
          for (; col < nCurves; ++col)
            cols[col].values.append(nan());
      }
    }
    ++row;
    if (eol >= n)
      break; // 尾行无行界符——处理完即结束
    i = eol + 1;
    if (raw.at(eol) == '\r' && i < n && raw.at(i) == '\n')
      ++i;
    if (!wantAll && row >= to)
      break; // 区间查询：越过 to 即停（D1.4）
  }
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
  qint64 pos = header.asciiDataOffset;
  constexpr qint64 kChunk = 4 * 1024 * 1024;
  bool stopped = false;
  std::vector<double> rowVals(nCurves);
  while (pos < fileSize && !stopped)
  {
    f.seek(pos);
    QByteArray chunk = f.read(kChunk);
    if (chunk.isEmpty())
      break;
    qint64 usable = chunk.size();
    if (pos + usable < fileSize)
    {
      const qint64 lastSep = qMax<qint64>(chunk.lastIndexOf('\n'), chunk.lastIndexOf('\r'));
      if (lastSep < 0)
        usable = 0;
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
          rowVals[col++] =
              QByteArray::fromRawData(chunk.constData() + p, static_cast<int>(q - p)).toDouble(&ok);
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
