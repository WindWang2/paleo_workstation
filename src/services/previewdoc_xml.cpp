// 层：数据
#include "previewdoc.h"
#include "../io/outsourceworkbook.h"
#include <QFile>
#include <QXmlStreamReader>

PreviewDocService::WorkbookPreview PreviewDocService::xmlDataListAt(const QString &path)
{
  WorkbookPreview result;
  if (areaPreviewAt(path, &result, nullptr, nullptr, &result.error)) return result;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) { result.error = file.errorString(); return result; }
  QXmlStreamReader reader(&file);
  // L-07：预览的是导入的外委/第三方 XML——QXmlStreamReader 会展开 DTD 内
  // 声明的内部实体（billion laughs：数十 KB 声明展开成 GB 级字符串，GUI
  // 线程假死）。合法 SpreadsheetML/数据 XML 不带 DTD，见令牌即拒。
  // 令牌级检查（照 outsourceworkbook.cpp 口径）：DOCTYPE 是文档级前导令牌，
  // readNextStartElement() 会连它一起跳过再落到首个 StartElement——在它之后
  // 判 isDTD() 永不命中（旧守卫是死代码）。这里自己读令牌到首个元素，
  // 途中见到 DTD 即拒。
  for (;;) {
    const QXmlStreamReader::TokenType token = reader.readNext();
    if (token == QXmlStreamReader::Invalid ||
        token == QXmlStreamReader::EndDocument) {
      result.error = reader.errorString();
      return result;
    }
    if (token == QXmlStreamReader::DTD) {
      result.error = tr("XML 含 DTD/实体声明，为防实体扩展攻击已拒收");
      return result;
    }
    if (token == QXmlStreamReader::StartElement)
      break;
  }
  if (reader.name() == QLatin1String("Workbook") &&
      reader.namespaceUri() == QLatin1String("urn:schemas-microsoft-com:office:spreadsheet")) {
    file.seek(0);
    QVector<int> rowCounts;
    bool sheetCapped = false; // 64 表软上限后的整段跳过开关
    // 与通用 XML 分支同款单格口径（left(4096)）：格数预算不约束单格文本
    // 长度，恶意工作簿可用少数巨格把预览结构撑到 GB 级且驻留 GUI 线程。
    const auto cappedCells = [](const QStringList &cells) {
      QStringList out;
      out.reserve(cells.size());
      for (const QString &cell : cells)
        out << cell.left(4096);
      return out;
    };
    const auto workbook = paleo::io::streamSpreadsheetMlWorkbook(&file, path,
        [&](const QString &name, const QStringList &headers, const QStringList &row, int, bool isHeader) {
          if (isHeader) {
            // sheet 数软上限（同族：通用分支 512 层 / 1000 行）——预算到顶后
            // 跳过后续表头与数据行并 issues 留痕；visitor 返回 false 会被
            // io 层当「调用方拒绝」整预览硬失败且错误文案是内部实现语。
            if (result.sheets.size() >= 64) {
              sheetCapped = true;
              return true;
            }
            WorkbookSheetPreview sheet; sheet.name = name; sheet.headers = cappedCells(headers);
            result.sheets.append(sheet); rowCounts.append(0);
          } else if (!result.sheets.isEmpty()) {
            if (sheetCapped)
              return true; // 超限表的数据行不落格——否则会归到第 64 张表的行里
            if (++rowCounts.last() <= 1000) result.sheets.last().rows.append(cappedCells(row));
          }
          return true;
        });
    result.ok = workbook.ok; result.error = workbook.error; result.format = workbook.format;
    result.issues = workbook.issues;
    if (sheetCapped) result.issues << tr("工作簿超过 64 张工作表，仅预览前 64 张");
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
    // L-07：预览的是导入的外委/第三方 XML——QXmlStreamReader 会展开 DTD
    // 内声明的内部实体（billion laughs：数十 KB 声明展开成 GB 级字符串，
    // GUI 线程假死）。合法 SpreadsheetML/数据 XML 不带 DTD，见令牌即拒。
    if (reader.isDTD()) {
      result.error = tr("XML 含 DTD/实体声明，为防实体扩展攻击已拒收");
      return result;
    }
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
