#pragma once
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

namespace ImportUxFixtures
{
inline bool write(const QString &path, const QByteArray &bytes)
{
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

inline QByteArray csv(const QByteArray &well = "A1", const QByteArray &curve = "RES[ohm.m]",
                      const QByteArray &rows = "1000,10\n1001,20\n", const QByteArray &axis = "MD[m]")
{
  return (well.isEmpty() ? QByteArray() : "# Well: " + well + '\n') + axis + ',' + curve + '\n' + rows;
}

inline QString row(const QStringList &cells)
{
  QString result = QStringLiteral("<Row>");
  for (const auto &cell : cells)
    result += QStringLiteral("<Cell><Data ss:Type=\"String\">%1</Data></Cell>").arg(cell.toHtmlEscaped());
  return result + QStringLiteral("</Row>\n");
}

// Complete SpreadsheetML, including a non-curve worksheet. The optional well
// column makes content identity independent of the source filename and sheet name.
inline QByteArray xml(const QString &well = QStringLiteral("A1"), bool wellColumn = true)
{
  QString result = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<Workbook xmlns=\"urn:schemas-microsoft-com:office:spreadsheet\" "
      "xmlns:ss=\"urn:schemas-microsoft-com:office:spreadsheet\">\n"
      "<Worksheet ss:Name=\"说明\"><Table>");
  result += row({QStringLiteral("项目"), QStringLiteral("内容")});
  result += row({QStringLiteral("来源"), QStringLiteral("可复现的综合测井样例")});
  result += QStringLiteral("</Table></Worksheet>\n<Worksheet ss:Name=\"测井曲线-%1井\"><Table>\n")
      .arg(well.toHtmlEscaped());
  QStringList headers{QStringLiteral("MD[m]"), QStringLiteral("GR[API]"),
                      QStringLiteral("RHOB[g/cm3]"), QStringLiteral("NPHI[%]")};
  if (wellColumn) headers.prepend(QStringLiteral("井名"));
  result += row(headers);
  for (const auto &cells : QList<QStringList>{{"1000", "40", "2.4", "12"},
                                             {"1000.5", "45", "2.5", "13"},
                                             {"1001", "50", "2.6", "14"}})
  {
    QStringList data = cells;
    if (wellColumn) data.prepend(well);
    result += row(data);
  }
  return (result + QStringLiteral("</Table></Worksheet>\n</Workbook>\n")).toUtf8();
}

inline QByteArray paddedXml(const QString &sheetName = QStringLiteral("测井曲线-A1井"))
{
  QByteArray content = xml();
  content.replace(QStringLiteral("可复现的综合测井样例").toUtf8(), QByteArray(70 * 1024, 'x'));
  content.replace(QStringLiteral("测井曲线-A1井").toUtf8(), sheetName.toUtf8());
  return content;
}

// Additional comprehensive export mode; the older xml() fixture stays intact.
inline QByteArray comprehensiveXml()
{
  QString result = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<Workbook xmlns=\"urn:schemas-microsoft-com:office:spreadsheet\" "
      "xmlns:ss=\"urn:schemas-microsoft-com:office:spreadsheet\">"
      "<Worksheet ss:Name=\"测井曲线\"><Table>");
  result += row({"井号", "深度", "TVD", "TVDSS", "HXN_POR"});
  result += row({"A1", "1000", "990", "", "8"});
  result += row({"A1", "1001", "991", "", "9"});
  return (result + QStringLiteral("</Table></Worksheet></Workbook>")).toUtf8();
}
}
