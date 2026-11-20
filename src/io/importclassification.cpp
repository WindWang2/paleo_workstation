// 层：数据
#include "importclassification.h"
#include "welllogtext.h"
#include "welltable.h"
#include "wellfactorfile.h"
#include <QFileInfo>
#include <QFile>
#include <QXmlStreamReader>

ImportClassificationDetail classifyImportFileDetailed(const QString &path, const std::function<bool()> &cancel)
{
  ImportClassificationDetail detail;
  ProjectClassification &classification = detail.cls;
  const auto cancelled = [&detail, &path]() {
    detail.cancelled = true;
    detail.containerError = QStringLiteral("已取消：%1").arg(path);
    return detail;
  };
  if (cancel && cancel())
    return cancelled();
  if (paleo::io::isWellFactorFile(path)) { detail.cls = {QStringLiteral("tabular"), QStringLiteral("csv"), QStringLiteral("input")}; return detail; }
  if (paleo::io::isWellTableContainer(path) && QFileInfo(path).size() <= 64ll * 1024 * 1024) {
    // issue-810：分类期先做有界预览——readWellTable 会把整表读进来（百万行
    // 矢量/工作簿在分类阶段全量解析，扫大目录时直接卡死）。previewWellTable
    // 同口径识别 type/mapping，行数与层数都受限。预览被预算截断且仍定不了型
    // （超大 xlsx/矢量）时才回落整读：分类结论要覆盖大表，不能因为预览配额
    // 把井表降级成 outsource_workbook。
    auto table = paleo::io::previewWellTable(path);
    if (table.type.isEmpty() && table.scanLimited) {
      if (cancel && cancel())
        return cancelled(); // P1-W5：整读回退前先看取消，不做无谓的全量解析
      table = paleo::io::readWellTable(path, {}, cancel);
    }
    if (!table.type.isEmpty() && !table.mapping.value(QStringLiteral("headerless")).toBool()) {
      detail.cls = {table.type, QFileInfo(path).suffix().toLower(), QStringLiteral("input")};
      return detail;
    }
    // P1-W6：井表容器「读成功但不是井表」照旧走通用分类（合法外委工作簿/
    // 非井表矢量）；「格式已识别但内容损坏」才把真实失败原因带回给导入侧
    //——这类文件若静默按通用文件导入会获得「有效」状态。两类不算损坏：
    //   · ZIP 结构完好但缺 xl/workbook.xml（如 docx 字节冒充 .xlsx）——
    //     「不是 OOXML 工作簿」，基线即按外委簿收；
    //   · GDAL/OGR 打不开的坏 shp/dbf/gpkg——「不是井表」，族导入与参考
    //     导入仍要能收它们（基线语义）。
    // 判据只认内容级失败文案（XML 解析错误/DTD 拒收/预算超限/读取中途
    // 出错），不认「没有条目」这类结构性缺失。
    if (table.cancelled)
      return cancelled();
    if (table.type.isEmpty() && table.containerBroken && !table.error.isEmpty() &&
        (table.error.startsWith(QStringLiteral("工作簿")) ||
         table.error.startsWith(QStringLiteral("井表")) ||
         table.error.startsWith(QStringLiteral("矢量图层")) ||
         table.error.startsWith(QStringLiteral("矢量层读取")) ||
         (table.error.contains(QStringLiteral("解析错误")) ||
          table.error.contains(QStringLiteral("DTD")) ||
          table.error.contains(QStringLiteral("超预算")))))
      detail.containerError = table.error;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) { detail.cls = classifyProjectPath(path); return detail; }
  const QByteArray prefix = file.read(65536);
  classification = classifyProjectImport(path, prefix);
  if ((classification.type != QLatin1String("unknown") && classification.type != QLatin1String("well_deviation")) ||
      !WellLogText::sniffSpreadsheetXml(prefix) || file.size() <= prefix.size() ||
      file.size() > 1024ll * 1024 * 1024) { detail.cls = classification; return detail; }
  // 大型说明工作表可能遮住后面的曲线表。流式走工作表名，完整解析器仍负责数据校验。
  file.seek(0);
  QXmlStreamReader xml(&file);
  while (!xml.atEnd())
  {
    const auto token = xml.readNext();
    // P1-W3（#1023）：流式分类扫描与完整读取同一 DTD 策略。
    if (token == QXmlStreamReader::DTD) break;
    if (!xml.isStartElement() || xml.name() != QLatin1String("Worksheet")) continue;
    for (const auto &attribute : xml.attributes())
    {
      if (attribute.name() != QLatin1String("Name")) continue;
      const QString name = attribute.value().toString().trimmed();
      if (name.startsWith(QStringLiteral("测井曲线")) || name.startsWith(QStringLiteral("离散曲线")))
      {
        // 曲线表优先命中即定（保持原 return 语义：后面的说明/井斜页不改判）
        detail.cls = {QStringLiteral("well_log"), QStringLiteral("xml"), QStringLiteral("input")};
        return detail;
      }
      if (name.startsWith(QStringLiteral("井斜")) || name.startsWith(QLatin1String("deviation"), Qt::CaseInsensitive))
        classification = {QStringLiteral("well_deviation"), QStringLiteral("xml"), QStringLiteral("input")};
    }
    xml.skipCurrentElement();
    if (cancel && cancel())
      return cancelled();
  }
  detail.cls = classification;
  return detail;
}

ProjectClassification classifyImportFile(const QString &path)
{
  return classifyImportFileDetailed(path).cls;
}
