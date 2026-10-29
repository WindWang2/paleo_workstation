// 层：数据
#include "importclassification.h"
#include "welllogtext.h"
#include "welltable.h"
#include "wellfactorfile.h"
#include <QFileInfo>
#include <QFile>
#include <QXmlStreamReader>

ProjectClassification classifyImportFile(const QString &path)
{
  if (paleo::io::isWellFactorFile(path)) return {QStringLiteral("tabular"), QStringLiteral("csv"), QStringLiteral("input")};
  if (paleo::io::isWellTableContainer(path) && QFileInfo(path).size() <= 64ll * 1024 * 1024) {
    // issue-810：分类期先做有界预览——readWellTable 会把整表读进来（百万行
    // 矢量/工作簿在分类阶段全量解析，扫大目录时直接卡死）。previewWellTable
    // 同口径识别 type/mapping，行数与层数都受限。预览被预算截断且仍定不了型
    // （超大 xlsx/矢量）时才回落整读：分类结论要覆盖大表，不能因为预览配额
    // 把井表降级成 outsource_workbook。
    auto table = paleo::io::previewWellTable(path);
    if (table.type.isEmpty() && table.scanLimited)
      table = paleo::io::readWellTable(path);
    if (!table.type.isEmpty() && !table.mapping.value(QStringLiteral("headerless")).toBool())
      return {table.type, QFileInfo(path).suffix().toLower(), QStringLiteral("input")};
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return classifyProjectPath(path);
  const QByteArray prefix = file.read(65536);
  auto classification = classifyProjectImport(path, prefix);
  if ((classification.type != QLatin1String("unknown") && classification.type != QLatin1String("well_deviation")) ||
      !WellLogText::sniffSpreadsheetXml(prefix) || file.size() <= prefix.size() ||
      file.size() > 1024ll * 1024 * 1024) return classification;
  // 大型说明工作表可能遮住后面的曲线表。流式走工作表名，完整解析器仍负责数据校验。
  file.seek(0);
  QXmlStreamReader xml(&file);
  while (!xml.atEnd())
  {
    xml.readNext();
    if (!xml.isStartElement() || xml.name() != QLatin1String("Worksheet")) continue;
    for (const auto &attribute : xml.attributes())
    {
      if (attribute.name() != QLatin1String("Name")) continue;
      const QString name = attribute.value().toString().trimmed();
      if (name.startsWith(QStringLiteral("测井曲线")) || name.startsWith(QStringLiteral("离散曲线")))
        return {QStringLiteral("well_log"), QStringLiteral("xml"), QStringLiteral("input")};
      if (name.startsWith(QStringLiteral("井斜")) || name.startsWith(QLatin1String("deviation"), Qt::CaseInsensitive))
        classification = {QStringLiteral("well_deviation"), QStringLiteral("xml"), QStringLiteral("input")};
    }
    xml.skipCurrentElement();
  }
  return classification;
}
