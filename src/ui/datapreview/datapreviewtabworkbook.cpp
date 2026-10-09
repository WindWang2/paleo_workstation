// 层：视图
#include "datapreviewtabs.h"
#include "datapreviewtabs_internal.h"

// 共享辅助来自内部头——与 datapreviewtabs.cpp 用同一 using 引入。
using namespace paleo::datapreview_detail;
#include "../paleotheme.h"
#include "../../services/previewdoc.h" // WorkbookPreview（唯一数据门面）

#include <QHeaderView>
#include <QLabel>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

// SpreadsheetML XML 工作簿的数据表视图。XLS/XLSX 原件在主分支直接交给
// Calligra 离线页面预览；此处只呈现 XML 的 sheet/表头/行。
namespace
{
// 预览截断阈值：与 geojson 要素表同一量级（大表防卡顿；表尾如实注明）。
constexpr int kMaxPreviewRows = 1000;

QTableWidget *makeSheetTable(const PreviewDocService::WorkbookSheetPreview &sheet,
                             QWidget *parent)
{
  auto *table = new QTableWidget(parent);
  table->setAlternatingRowColors(true);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setSelectionMode(QAbstractItemView::ContiguousSelection);
  PaleoTheme::applyThemedStyleSheet(table, [] {
    const PaleoTheme::ThemeTokens &t = PaleoTheme::tokens();
    return PaleoTheme::metricStyleSheet(QStringLiteral(
        "QTableWidget { background-color: %1; gridline-color: %2; border: 1px solid %2;"
        " font-size: {typography.body}pt; }"
        "QHeaderView::section { background-color: %3; color: %4; border: none;"
        " border-bottom: 1px solid %2; border-right: 1px solid %2; padding: {spacing.xs}px {spacing.sm}px;"
        " font-weight: 500; font-size: {typography.label}pt; }"))
        .arg(qssHex(t.surface), qssHex(t.border), qssHex(t.surfaceAlt),
             qssHex(t.textMuted));
  });

  // 表头缺失（空表头行的表）时按列号兜底；列数取表头与各行宽度的最大者。
  int colCount = sheet.headers.size();
  for (const QStringList &row : sheet.rows)
    colCount = qMax(colCount, row.size());
  QStringList headers = sheet.headers;
  while (headers.size() < colCount)
    headers.append(QObject::tr("列%1").arg(headers.size() + 1));
  table->setColumnCount(colCount);
  table->setHorizontalHeaderLabels(headers);

  const int rows = qMin(sheet.rows.size(), kMaxPreviewRows);
  table->setRowCount(rows);
  for (int r = 0; r < rows; ++r)
  {
    const QStringList &row = sheet.rows.at(r);
    for (int c = 0; c < colCount; ++c)
    {
      auto *item = new QTableWidgetItem(row.value(c));
      item->setFlags(item->flags() & ~Qt::ItemIsEditable);
      table->setItem(r, c, item);
    }
  }
  table->horizontalHeader()->setStretchLastSection(true);
  table->resizeColumnsToContents();
  return table;
}
} // namespace

// 表格预览内容本体：sheet 表 + 截断/issues 说明行（无页尾警告——调用方
// 各自管页尾）。返回工作簿是否解析成功；辅助 XML 的默认井道图与列表
// 切换由调用方保持。
bool DataPreviewTabs::buildWorkbookTable(const QString &abs, QWidget *host,
                                         QVBoxLayout *lay)
{
  const PreviewDocService::WorkbookPreview wb =
      PreviewDocService::workbookPreviewAt(abs);
  return buildWorkbookTable(wb, host, lay);
}
bool DataPreviewTabs::buildWorkbookTable(const PreviewDocService::WorkbookPreview &wb,
                                        QWidget *host, QVBoxLayout *lay)
{
  if (!wb.ok)
  {
    lay->addWidget(stateLabel(tr("无法读取工作簿：%1")
                                  .arg(wb.error.isEmpty() ? tr("未知错误")
                                                          : wb.error),
                               host, true),
                   1);
  }
  else if (wb.sheets.isEmpty())
  {
    lay->addWidget(stateLabel(tr("工作簿没有工作表"), host), 1);
  }
  else
  {
    // 单 sheet 直接铺表；多 sheet 用 QTabWidget 切换（页名 = sheet 名）。
    QWidget *sheetsHost;
    if (wb.sheets.size() == 1)
    {
      sheetsHost = makeSheetTable(wb.sheets.first(), host);
      lay->addWidget(sheetsHost, 1);
    }
    else
    {
      auto *tabs = new QTabWidget(host);
      tabs->setObjectName(QStringLiteral("workbookSheetTabs"));
      for (const PreviewDocService::WorkbookSheetPreview &s : wb.sheets)
        tabs->addTab(makeSheetTable(s, tabs), s.name);
      lay->addWidget(tabs, 1);
    }

    // 截断/问题如实注明——不静默丢行。
    for (const PreviewDocService::WorkbookSheetPreview &s : wb.sheets)
      if (s.rows.size() > kMaxPreviewRows)
        lay->addWidget(caption8(tr("%1：仅预览前 %2 行（共 %3 行）——完整内容经右键「用系统程序打开」")
                                    .arg(s.name)
                                    .arg(kMaxPreviewRows)
                                    .arg(s.rows.size()),
                                host));
    if (!wb.issues.isEmpty())
      lay->addWidget(caption8(tr("读取提示：%1").arg(wb.issues.join(QStringLiteral("；"))), host));
  }
  return wb.ok;
}

QWidget *DataPreviewTabs::buildOutsourceWorkbookContent(const QString &abs,
                                                        QWidget *host,
                                                        QVBoxLayout *lay)
{
  buildWorkbookTable(abs, host, lay);
  // 打开原件走数据列表右键菜单（「用系统程序打开」），预览页不驻留按钮。
  lay->addWidget(warnLabel(tr("未配准，不加入地图"), host)); // §4
  return host;
}
