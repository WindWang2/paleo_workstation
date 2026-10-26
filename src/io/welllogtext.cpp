// 层：数据
#include "welllogtext_internal.h"

#include "encodingdetect.h"

#include <QCoreApplication>
#include <QFile>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QXmlStreamReader>

namespace WellLogText::Detail
{
Column column(const QString &text)
{
  static const QRegularExpression pattern(QStringLiteral("^\\s*([^\\[\\]()]+?)\\s*(?:\\[([^\\]]*)\\]|\\(([^)]*)\\))?\\s*$"));
  const auto match = pattern.match(text);
  if (!match.hasMatch()) return {};
  return {match.captured(1).trimmed(), (match.captured(2) + match.captured(3)).trimmed()};
}

bool isIndex(const QString &name)
{
  static const QStringList names = {QStringLiteral("DEPTH"), QStringLiteral("DEPT"),
      QStringLiteral("MD"), QStringLiteral("TVD"), QStringLiteral("TVDSS"),
      QStringLiteral("TIME"), QStringLiteral("深度"), QStringLiteral("井深")};
  return names.contains(name.toUpper());
}

// 表头行分隔符判定：tab → 逗号 → 分号（与 projectclassifier 的 [,;\t] 词表
// 对齐——分类器提议过的分隔符读取器必须读得回来）→ 空白。
QChar pickDelimiter(const QString &headerLine)
{
  return headerLine.contains(QLatin1Char('\t')) ? QLatin1Char('\t')
      : headerLine.contains(QLatin1Char(',')) ? QLatin1Char(',')
      : headerLine.contains(QLatin1Char(';')) ? QLatin1Char(';')
                                               : QChar();
}

bool fail(const QString &path, int line, const QString &message, QString *error,
          QList<LasIssue> *issues, LasIssue::Category category)
{
  const QString text = QCoreApplication::translate("WellLogText", "%1，第 %2 行：%3。请检查文件后重试。")
      .arg(path).arg(line).arg(message);
  if (error) *error = text;
  if (issues) issues->append({LasIssue::Severity::Error, category, line, text});
  return false;
}

bool split(const QString &line, QChar delimiter, QStringList &cells)
{
  if (delimiter.isNull())
  {
    // 空白分隔逐 token 收集：与分隔符分支同列预算（kMaxColumns）早退，
    // 不先物化整行 QStringList——病态超宽单行在 256 列处即失败。
    const QString trimmed = line.trimmed();
    const int n = trimmed.size();
    int i = 0;
    while (i < n)
    {
      while (i < n && trimmed.at(i).isSpace()) ++i;
      const int start = i;
      while (i < n && !trimmed.at(i).isSpace()) ++i;
      if (i > start)
      {
        if (cells.size() >= kMaxColumns) return false;
        cells.append(trimmed.mid(start, i - start));
      }
    }
    return true;
  }
  QString cell;
  bool quoted = false;
  bool closed = false;
  for (int i = 0; i < line.size(); ++i)
  {
    const QChar ch = line.at(i);
    if (quoted)
    {
      if (ch != QLatin1Char('"')) cell += ch;
      else if (i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) { cell += ch; ++i; }
      else { quoted = false; closed = true; }
    }
    else if (ch == delimiter) { cells.append(cell.trimmed()); cell.clear(); closed = false; }
    else if (ch == QLatin1Char('"') && cell.trimmed().isEmpty() && !closed) { cell.clear(); quoted = true; }
    else if (ch == QLatin1Char('"') || (closed && !ch.isSpace())) return false;
    else cell += ch;
    if (cells.size() > kMaxColumns) return false;
  }
  cells.append(cell.trimmed());
  return !quoted;
}

bool readText(const QByteArray &bytes, Table &table, QString &wellName, QString &error, int &errorLine)
{
  // 与井表生态（encodingdetect.h）同口径：UTF-8 优先、GB18030 兜底——
  // 工区里含中文井名/表头的 GB 文本曲线与井口/分层文件一样常见。
  QString content = EncodingDetect::decodeText(bytes);
  if (content.startsWith(QChar(0xfeff))) content.remove(0, 1);
  const QStringList lines = content.split(QLatin1Char('\n'));
  QChar delimiter;
  qint64 cells = 0;
  for (int i = 0; i < lines.size(); ++i)
  {
    errorLine = i + 1;
    QString line = lines.at(i);
    if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
    if (line.trimmed().isEmpty()) continue;
    if (line.trimmed().startsWith(QLatin1Char('#')))
    {
      static const QRegularExpression well(QStringLiteral("^#\\s*Well\\s*:\\s*(.+)$"), QRegularExpression::CaseInsensitiveOption);
      const auto match = well.match(line.trimmed());
      if (match.hasMatch())
      {
        const QString name = match.captured(1).trimmed();
        if (!wellName.isEmpty() && wellName != name) { error = QCoreApplication::translate("WellLogText", "井名元数据互相冲突"); return false; }
        wellName = name;
      }
      continue;
    }
    if (table.headers.isEmpty())
      delimiter = Detail::pickDelimiter(line);
    QStringList values;
    if (!split(line, delimiter, values)) { error = QCoreApplication::translate("WellLogText", "引号未闭合或列数超过上限"); return false; }
    if (!table.headers.isEmpty() && values.size() != table.headers.size())
    { error = QCoreApplication::translate("WellLogText", "数据列数与表头不一致，请保留空值分隔符"); return false; }
    cells += values.size();
    if (cells > kMaxCells) { error = QCoreApplication::translate("WellLogText", "数据超过 400 万单元格预算，请拆分文件"); return false; }
    if (table.headers.isEmpty()) table.headers = values;
    else { table.rows.append(values); table.lines.append(i + 1); }
  }
  return true;
}
}

namespace WellLogText
{
bool sniffText(const QByteArray &content)
{
  QString text = EncodingDetect::decodeText(content);
  if (text.startsWith(QChar(0xfeff))) text.remove(0, 1);
  for (const QString &raw : text.split(QLatin1Char('\n')))
  {
    const QString line = raw.trimmed();
    if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
    QStringList cells;
    if (!Detail::split(line, Detail::pickDelimiter(line), cells) || cells.size() < 2) return false;
    for (const auto &cell : cells)
      if (Detail::isIndex(Detail::column(cell).name)) return true;
    return false;
  }
  return false;
}

bool sniffSpreadsheetXml(const QByteArray &content)
{
  QXmlStreamReader reader(content);
  return reader.readNextStartElement() && reader.name() == QLatin1String("Workbook") &&
      reader.namespaceUri() == QLatin1String("urn:schemas-microsoft-com:office:spreadsheet");
}

bool parse(const QString &path, bool spreadsheetXml, LasHeaderInfo &header,
           QList<LasCurve> &curves, QString *error, QList<LasIssue> *issues)
{
  header = {};
  curves.clear();
  if (error) error->clear();
  if (spreadsheetXml) {
    LasHeaderInfo parsedHeader;
    QList<LasCurve> parsedCurves;
    if (!Detail::parseXml(path, parsedHeader, parsedCurves, error, issues)) return false;
    header = std::move(parsedHeader);
    curves = std::move(parsedCurves);
    return true;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return Detail::fail(path, 0, file.errorString(), error, issues, LasIssue::Category::Io);
  if (file.size() > Detail::kMaxBytes)
    return Detail::fail(path, 0, QCoreApplication::translate("WellLogText", "文件超过 64 MB，请拆分文件"), error, issues, LasIssue::Category::Oversize);
  const QByteArray bytes = file.read(Detail::kMaxBytes + 1);
  if (bytes.size() > Detail::kMaxBytes || file.error() != QFileDevice::NoError)
    return Detail::fail(path, 0, QCoreApplication::translate("WellLogText", "文件读取失败或超过大小预算"), error, issues, LasIssue::Category::Io);
  QVector<Detail::Table> tables;
  LasHeaderInfo parsedHeader;
  {
    Detail::Table table;
    QString detail;
    int line = 0;
    if (!Detail::readText(bytes, table, parsedHeader.wellName, detail, line))
      return Detail::fail(path, line, detail, error, issues);
    tables.append(table);
  }
  QList<LasCurve> parsedCurves;
  if (!Detail::convert(path, tables, parsedHeader, parsedCurves, error, issues)) return false;
  header = parsedHeader;
  curves = parsedCurves;
  return true;
}
}
