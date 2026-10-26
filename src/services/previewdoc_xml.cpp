// 层：数据
#include "previewdoc.h"
#include "../io/outsourceworkbook.h"
#include <QFile>
#include <QXmlStreamReader>

PreviewDocService::WorkbookPreview PreviewDocService::xmlDataListAt(const QString &path)
{
  WorkbookPreview result;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) { result.error = file.errorString(); return result; }
  QXmlStreamReader reader(&file);
  if (!reader.readNextStartElement()) { result.error = reader.errorString(); return result; }
  if (reader.name() == QLatin1String("Workbook") &&
      reader.namespaceUri() == QLatin1String("urn:schemas-microsoft-com:office:spreadsheet")) {
    file.seek(0);
    QVector<int> rowCounts;
    const auto workbook = paleo::io::streamSpreadsheetMlWorkbook(&file, path,
        [&](const QString &name, const QStringList &headers, const QStringList &row, int, bool isHeader) {
          if (isHeader) {
            WorkbookSheetPreview sheet; sheet.name = name; sheet.headers = headers;
            result.sheets.append(sheet); rowCounts.append(0);
          } else if (!result.sheets.isEmpty()) {
            if (++rowCounts.last() <= 1000) result.sheets.last().rows.append(row);
          }
          return true;
        });
    result.ok = workbook.ok; result.error = workbook.error; result.format = workbook.format;
    result.issues = workbook.issues;
    if (!result.ok) { result.sheets.clear(); return result; }
    for (int i = 0; i < rowCounts.size(); ++i)
      if (rowCounts[i] > 1000) result.issues << tr("工作表「%1」仅预览前 1000 行").arg(result.sheets[i].name);
    return result;
  }
  WorkbookSheetPreview sheet;
  sheet.name = tr("XML 数据"); sheet.headers = {tr("路径"), tr("内容")};
  QStringList elements, values;
  qint64 rows = 0;
  const auto append = [&](const QString &name, const QString &value) {
    if (++rows <= 1000) sheet.rows.append({name, value.left(4096)});
  };
  do {
    if (reader.isStartElement()) {
      if (elements.size() >= 512) { result.error = tr("XML 嵌套层级过深"); return result; }
      elements << reader.qualifiedName().toString(); values << QString();
      for (const auto &attr : reader.attributes())
        append("/" + elements.join('/') + "/@" + attr.qualifiedName().toString(), attr.value().toString());
    } else if (reader.isCharacters() && !values.isEmpty()) {
      values.last() += reader.text().toString().left(qMax(0, 4096 - values.last().size()));
    } else if (reader.isEndElement() && !elements.isEmpty()) {
      if (!values.last().trimmed().isEmpty()) append("/" + elements.join('/'), values.last().trimmed());
      elements.removeLast(); values.removeLast();
    }
    reader.readNext();
  } while (!reader.atEnd());
  if (reader.hasError()) { result.error = reader.errorString(); return result; }
  result.ok = true; result.format = "xml"; result.sheets << sheet;
  if (rows > 1000) result.issues << tr("XML 数据列表仅预览前 1000 行");
  return result;
}
