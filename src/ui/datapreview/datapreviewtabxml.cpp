// 层：视图
#include "datapreviewtabs.h"
#include "datapreviewtabs_internal.h"
#include "../../workflow/xmlpreviewsession.h"
#include "../wellcomposite/wellcompositepanel.h"
#include "../paleoviewport.h"
#include <QStackedWidget>

using namespace paleo::datapreview_detail;

QWidget *DataPreviewTabs::buildAuxiliaryXmlContent(const QString &abs, const QString &assetId,
                                                   QWidget *host, QVBoxLayout *lay, const QString &expectedSha)
{
  auto *chart = new WellComposite::WellCompositePanel(host);
  chart->setObjectName("wellCompositePanel");
  auto *hint = new QLabel(tr("正在后台读取井道图与数据列表…"), host);
  hint->setObjectName("xmlPendingHint");
  PaleoTheme::applyThemedStyleSheet(hint, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  auto *tableHost = new QWidget(host);
  auto *tableLayout = new QVBoxLayout(tableHost);
  tableLayout->setContentsMargins(0, 0, 0, 0);
  tableLayout->setSpacing(PaleoTheme::tokens().spacingSm);
  auto *tableHint = new QLabel(tr("正在后台读取数据列表…"), tableHost);
  tableLayout->addWidget(tableHint);
  auto *stack = new QStackedWidget(host);
  stack->setObjectName("xmlViewStack"); stack->addWidget(chart); stack->addWidget(tableHost);
  auto *bar = new QWidget(host);
  auto *buttons = new QHBoxLayout(bar);
  buttons->setContentsMargins(0, 0, 0, 0); buttons->setSpacing(PaleoTheme::tokens().spacingSm);
  styleViewSwitchBar(bar);
  auto *chartButton = new QToolButton(bar);
  chartButton->setObjectName("btnXmlCompositeView"); chartButton->setText(tr("井道图"));
  chartButton->setCheckable(true); chartButton->setChecked(true);
  auto *tableButton = new QToolButton(bar);
  tableButton->setObjectName("btnXmlTableView"); tableButton->setText(tr("数据列表")); tableButton->setCheckable(true);
  const auto select = [stack, chartButton, tableButton](int index) {
    stack->setCurrentIndex(index); chartButton->setChecked(index == 0); tableButton->setChecked(index == 1);
  };
  connect(chartButton, &QToolButton::clicked, host, [select] { select(0); });
  connect(tableButton, &QToolButton::clicked, host, [select] { select(1); });
  buttons->addWidget(caption8(tr("呈现模式:"), bar)); buttons->addWidget(chartButton);
  buttons->addWidget(tableButton); buttons->addStretch(1);
  lay->addWidget(new PaleoToolRow(bar, host)); lay->addWidget(hint); lay->addWidget(stack, 1);
  auto *session = new XmlPreviewSession(host);
  connect(session, &XmlPreviewSession::ready, host,
      [this, abs, assetId, host, stack, chart, hint, tableHint, tableHost, tableLayout](const XmlPreviewData &data) {
    hint->hide(); delete tableHint;
    buildWorkbookTable(data.table, tableHost, tableLayout);
    if (data.chartOk) chart->loadWellData(data.chart, abs, true);
    else {
      const int selected = stack->currentIndex();
      stack->removeWidget(chart); chart->deleteLater();
      auto *failure = failureState(assetId, tr("井道图无法读取：%1").arg(data.chartError), host);
      stack->insertWidget(0, failure); stack->setCurrentIndex(selected);
    }
  });
  session->open(abs, expectedSha);
  return host;
}
