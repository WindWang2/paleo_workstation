// 层：视图
#include "datapreviewtabs.h"
#include "datapreviewtabs_internal.h"
#include "../../workflow/xmlpreviewsession.h"
#include "../wellcomposite/wellcompositepanel.h"
#include "../paleoviewport.h"
#include <QStackedWidget>
#include <cmath>

using namespace paleo::datapreview_detail;

QWidget *DataPreviewTabs::buildAuxiliaryXmlContent(const QString &abs, const QString &assetId,
                                                   QWidget *host, QVBoxLayout *lay, const QString &expectedSha, bool reference)
{
  auto *chart = new WellComposite::WellCompositePanel(host);
  chart->setProjectName(catalogProjectDir(m_doc ? m_doc->catalog() : nullptr));
  chart->setObjectName("wellCompositePanel");
  auto *hint = new QLabel(tr("正在后台读取井道图与数据列表…"), host);
  hint->setWordWrap(true);
  hint->setObjectName("xmlPendingHint");
  PaleoTheme::applyThemedStyleSheet(hint, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  auto *tableHost = new QWidget(host);
  auto *tableLayout = new QVBoxLayout(tableHost);
  tableLayout->setContentsMargins(0, 0, 0, 0);
  tableLayout->setSpacing(PaleoTheme::tokens().spacingSm);
  auto *tableHint = new QLabel(tr("正在后台读取数据列表…"), tableHost);
  PaleoTheme::applyThemedStyleSheet(tableHint, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
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
  session->setTaskService(m_doc ? m_doc->taskService() : nullptr);
  connect(session, &XmlPreviewSession::ready, host,
      [this, abs, assetId, host, stack, chart, hint, tableHint, tableHost, tableLayout, reference](const XmlPreviewData &data) {
    hint->hide(); delete tableHint;
    buildWorkbookTable(data.table, tableHost, tableLayout);
    if (data.chartOk) {
      chart->loadWellData(data.chart, abs, reference);
      if (data.chart.modelWellId && std::isfinite(data.chart.x) && std::isfinite(data.chart.y)) {
        const auto known = [](const QString &value) { return value.isEmpty() ? tr("未确认") : value; };
        hint->setText(tr("井口 X：%1，Y：%2；井坐标系：%3（%4）")
            .arg(QString::number(data.chart.x, 'g', 12), QString::number(data.chart.y, 'g', 12),
                 known(data.chart.coordinateSystem), known(data.chart.coordinateUnit)));
        hint->show();
      }
    }
    else {
      const int selected = stack->currentIndex();
      stack->removeWidget(chart); chart->deleteLater();
      auto *failure = failureState(assetId, tr("井道图无法读取：%1").arg(data.chartError), host);
      stack->insertWidget(0, failure); stack->setCurrentIndex(selected);
    }
  });
  const auto *catalog = m_doc ? m_doc->catalog() : nullptr;
  const auto version = catalog ? catalog->currentVersion(assetId) : CatalogVersion{};
  const auto chosen = m_chosenVersionOfAsset.value(assetId);
  const qlonglong modelSource = (chosen.isEmpty() || chosen == version.id) && version.stage == "RAW"
      ? version.extra.value("areaModelSourceId").toLongLong() : 0;
  const qlonglong modelWell = catalog
      ? catalog->entityById(m_wellEntityOfAsset.value(assetId)).extra.value("areaModelWellId").toLongLong() : 0;
  session->open(abs, expectedSha, catalogProjectDir(catalog), modelSource, modelWell);
  return host;
}

void DataPreviewTabs::openModelWell(qlonglong modelWellId, const QString &wellName)
{
  if (!m_doc || modelWellId <= 0)
    return;
  const QString key = QStringLiteral("model-well:%1").arg(modelWellId);
  if (QWidget *existing = m_pageOfAsset.value(key))
  {
    m_tabs->setCurrentIndex(m_tabs->indexOf(existing));
    return;
  }
  const QString title = wellName.isEmpty() ? tr("井 %1").arg(modelWellId) : wellName;
  QWidget *page = new QWidget(this);
  auto *pageLay = new QVBoxLayout(page);
  pageLay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm,
                              PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm);
  const int idx = m_tabs->addTab(page, title);
  m_pageOfAsset.insert(key, page);
  m_tabs->setVisible(true);
  m_emptyLabel->setVisible(false);
  m_tabs->setCurrentIndex(idx);

  auto *chart = new WellComposite::WellCompositePanel(page);
  const QString project = catalogProjectDir(m_doc->catalog());
  chart->setProjectName(project);
  auto *hint = new QLabel(tr("正在后台从数据基座读取 %1 的井道图…").arg(title), page);
  hint->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(hint, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  auto *tableHost = new QWidget(page);
  auto *tableLayout = new QVBoxLayout(tableHost);
  tableLayout->setContentsMargins(0, 0, 0, 0);
  auto *tableHint = new QLabel(tr("正在后台读取数据列表…"), tableHost);
  tableLayout->addWidget(tableHint);
  auto *stack = new QStackedWidget(page);
  stack->addWidget(chart);
  stack->addWidget(tableHost);
  auto *bar = new QWidget(page);
  auto *buttons = new QHBoxLayout(bar);
  buttons->setContentsMargins(0, 0, 0, 0);
  styleViewSwitchBar(bar);
  auto *chartButton = new QToolButton(bar);
  chartButton->setText(tr("井道图"));
  chartButton->setCheckable(true);
  chartButton->setChecked(true);
  auto *tableButton = new QToolButton(bar);
  tableButton->setText(tr("数据列表"));
  tableButton->setCheckable(true);
  const auto select = [stack, chartButton, tableButton](int index) {
    stack->setCurrentIndex(index);
    chartButton->setChecked(index == 0);
    tableButton->setChecked(index == 1);
  };
  connect(chartButton, &QToolButton::clicked, page, [select] { select(0); });
  connect(tableButton, &QToolButton::clicked, page, [select] { select(1); });
  buttons->addWidget(caption8(tr("呈现模式:"), bar));
  buttons->addWidget(chartButton);
  buttons->addWidget(tableButton);
  buttons->addStretch(1);
  pageLay->addWidget(new PaleoToolRow(bar, page));
  pageLay->addWidget(hint);
  pageLay->addWidget(stack, 1);
  auto *session = new XmlPreviewSession(page);
  session->setTaskService(m_doc->taskService());
  connect(session, &XmlPreviewSession::ready, page,
          [page, stack, chart, hint, tableHint, tableHost, tableLayout, key, this](const XmlPreviewData &data) {
            hint->hide();
            delete tableHint;
            buildWorkbookTable(data.table, tableHost, tableLayout);
            if (data.chartOk)
            {
              chart->loadWellData(data.chart, QString(), false);
              if (std::isfinite(data.chart.x) && std::isfinite(data.chart.y)) {
                const auto known = [](const QString &value) { return value.isEmpty() ? tr("未确认") : value; };
                hint->setText(tr("井口 X：%1，Y：%2；井坐标系：%3（%4）")
                                  .arg(QString::number(data.chart.x, 'g', 12),
                                       QString::number(data.chart.y, 'g', 12),
                                       known(data.chart.coordinateSystem), known(data.chart.coordinateUnit)));
                hint->show();
              }
            }
            else
            {
              const int selected = stack->currentIndex();
              stack->removeWidget(chart);
              chart->deleteLater();
              auto *failure = failureState(key, tr("井道图无法读取：%1").arg(data.chartError), page);
              stack->insertWidget(0, failure);
              stack->setCurrentIndex(selected);
            }
          });
  session->openModelWell(project, modelWellId, title);
}
