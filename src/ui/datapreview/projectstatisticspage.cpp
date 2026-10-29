// 层：视图
#include "projectstatisticspage.h"
#include "datapreviewtabs_internal.h"
#include "../../catalog/datacatalog.h"
#include "../../workflow/projectareasummary.h"
#include "../paleotheme.h"
#include "../decorations/paleodecorations.h"
#include "../../qgis/dashboardsurveymap.h"
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>

using namespace paleo::datapreview_detail;

namespace
{
QLabel *textLabel(const QString &text, QWidget *parent, bool muted = false)
{
  auto *label = new QLabel(text, parent);
  label->setTextFormat(Qt::PlainText);
  label->setWordWrap(true);
  label->setMinimumWidth(0);
  if (muted)
    PaleoTheme::applyThemedStyleSheet(label, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  return label;
}
QFrame *card(QWidget *parent)
{
  auto *frame = new QFrame(parent);
  frame->setObjectName("dashboardCard");
  frame->setProperty("dashboardCard", true);
  frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  PaleoTheme::applyThemedStyleSheet(
      frame,
      []
      {
        const auto &t = PaleoTheme::tokens();
        return PaleoTheme::metricStyleSheet(
                   QStringLiteral("QFrame[dashboardCard=\"true\"] { background: %1; border: 1px solid %2; "
                                  "border-radius: {rounded.md}px; }"))
            .arg(t.surface.name(), t.border.name());
      });
  return frame;
}
QVBoxLayout *cardLayout(QWidget *frame)
{
  const auto &t = PaleoTheme::tokens();
  auto *layout = new QVBoxLayout(frame);
  layout->setContentsMargins(t.spacingMd, t.spacingMd, t.spacingMd, t.spacingMd);
  layout->setSpacing(t.spacingSm);
  return layout;
}
QWidget *metricCard(const QString &name, const QString &value, const QString &detail, const QString &id,
                    QWidget *parent)
{
  auto *frame = card(parent);
  auto *layout = cardLayout(frame);
  layout->addWidget(textLabel(name, frame, true));
  auto *number = textLabel(value, frame);
  number->setObjectName(id);
  number->setFont(PaleoTheme::monoFont(PaleoTheme::tokens().displayPt));
  layout->addWidget(number);
  layout->addWidget(textLabel(detail, frame, true));
  return frame;
}
QProgressBar *coverageBar(int value, int maximum, QWidget *parent)
{
  auto *bar = new QProgressBar(parent);
  bar->setRange(0, qMax(1, maximum));
  bar->setValue(value);
  bar->setTextVisible(false);
  bar->setFixedHeight(PaleoTheme::tokens().spacingSm);
  PaleoTheme::applyThemedStyleSheet(
      bar,
      []
      {
        const auto &t = PaleoTheme::tokens();
        return PaleoTheme::metricStyleSheet(
                   QStringLiteral(
                       "QProgressBar { border: none; background: %1; border-radius: {rounded.sm}px; }"
                       "QProgressBar::chunk { background: %2; border-radius: {rounded.sm}px; }"))
            .arg(t.surfaceAlt.name(), t.success.name());
      });
  return bar;
}
const QStringList stageKeys{QStringLiteral("well"), QStringLiteral("seismic"), QStringLiteral("factor"),
                            QStringLiteral("map"), QStringLiteral("output")};
QStringList stageNames()
{
  return {ProjectStatisticsPage::tr("测井预测"), ProjectStatisticsPage::tr("地震预测"),
          ProjectStatisticsPage::tr("单因素图"), ProjectStatisticsPage::tr("编图成果"),
          ProjectStatisticsPage::tr("图件输出")};
}
QWidget *horizonCard(const QVariantMap &row, QWidget *parent)
{
  auto *frame = card(parent);
  frame->setObjectName("dashboardHorizonCard");
  frame->setProperty("dashboardHorizon", row.value("horizon"));
  auto *layout = cardLayout(frame);
  auto *header = new QHBoxLayout;
  auto *name = textLabel(row.value("horizon").toString(), frame);
  name->setFont(PaleoTheme::monoFont(PaleoTheme::tokens().titlePt));
  header->addWidget(name);
  header->addStretch();
  header->addWidget(textLabel(
      ProjectStatisticsPage::tr("现有古地理图 %1 项").arg(row.value("existing_maps").toInt()), frame, true));
  layout->addLayout(header);
  const auto names = stageNames();
  auto *stages = new QGridLayout;
  stages->setSpacing(PaleoTheme::tokens().spacingSm);
  for (int i = 0; i < stageKeys.size(); ++i)
  {
    const bool present = row.value(stageKeys[i]).toBool();
    auto *label = PaleoTheme::capsuleLabel(
        names[i] + (present ? ProjectStatisticsPage::tr(" · 已有") : ProjectStatisticsPage::tr(" · 未登记")),
        present ? PaleoTheme::CapsuleKind::Success : PaleoTheme::CapsuleKind::Neutral, frame);
    label->setWordWrap(true);
    label->setMinimumWidth(0);
    stages->addWidget(label, i / 2, i % 2);
  }
  layout->addLayout(stages);
  return frame;
}
void clearGrid(QGridLayout *grid, QVector<QWidget *> &cards)
{
  while (auto *item = grid->takeAt(0))
    delete item;
  qDeleteAll(cards);
  cards.clear();
}
} // namespace

ProjectStatisticsPage::ProjectStatisticsPage(DataCatalog *catalog, QWidget *parent)
    : QWidget(parent), m_catalog(catalog)
{
  setObjectName("projectStatisticsPage");
  const auto &t = PaleoTheme::tokens();
  auto *outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  m_scroll = new QScrollArea(this);
  m_scroll->setObjectName("dashboardScroll");
  m_scroll->setWidgetResizable(true);
  m_scroll->setFrameShape(QFrame::NoFrame);
  m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_scroll->viewport()->installEventFilter(this);
  outer->addWidget(m_scroll);
  auto *body = new QWidget(m_scroll);
  body->setObjectName("dashboardBody");
  PaleoTheme::applyThemedStyleSheet(body,
                                    []
                                    {
                                      return QStringLiteral("QWidget#dashboardBody { background: %1; }")
                                          .arg(PaleoTheme::tokens().surfaceAlt.name());
                                    });
  m_content = new QVBoxLayout(body);
  m_content->setContentsMargins(t.spacingMd, t.spacingMd, t.spacingMd, t.spacingMd);
  m_content->setSpacing(t.spacingMd);
  auto *header = new QHBoxLayout;
  auto *titles = new QVBoxLayout;
  titles->setSpacing(t.spacingXs);
  auto *title = textLabel(tr("工区看板"), body);
  auto titleFont = PaleoTheme::bodyFont(t.displayPt);
  titleFont.setWeight(QFont::DemiBold);
  title->setFont(titleFont);
  titles->addWidget(title);
  titles->addWidget(textLabel(tr("数据总览与项目进度 · 随已登记资料更新"), body, true));
  header->addLayout(titles, 1);
  auto *map = new QPushButton(tr("打开全景地图"), body);
  map->setToolTip(tr("打开带完整工具的工区全景地图页"));
  map->setObjectName("dashboardOpenMap");
  map->setEnabled(catalog && catalog->isOpen());
  connect(map, &QPushButton::clicked, this, &ProjectStatisticsPage::surveyMapRequested);
  header->addWidget(map);
  m_content->addLayout(header);
  // 工区地图卡片惰性创建（showEvent → buildSurveyMapCard）：主窗口启动即
  // 常驻看板（#565），无头测试构造窗口时不落 QGIS 画布/OpenGL，避免
  // offscreen 下的堆损坏；看板真正显示时才建图并装数据。
  m_metrics = new QGridLayout;
  m_metrics->setSpacing(t.spacingSm);
  m_content->addLayout(m_metrics);
  auto *coverage = card(body);
  coverage->setObjectName("dashboardCoverage");
  auto *coverageLayout = cardLayout(coverage);
  auto *heading = textLabel(tr("成果覆盖"), coverage);
  PaleoTheme::applyThemedStyleSheet(heading, [] { return PaleoTheme::sectionTitleStyleSheet(); });
  coverageLayout->addWidget(heading);
  coverageLayout->addWidget(textLabel(tr("各环节已登记成果的层位数 / 全部层位数"), coverage, true));
  auto *coverageRows = new QGridLayout;
  coverageRows->setSpacing(t.spacingSm);
  const auto names = stageNames();
  for (int i = 0; i < stageKeys.size(); ++i)
  {
    coverageRows->addWidget(textLabel(names[i], coverage), i, 0);
    auto *bar = coverageBar(0, 1, coverage);
    bar->setObjectName("coverage_" + stageKeys[i]);
    bar->setAccessibleName(names[i]);
    coverageRows->addWidget(bar, i, 1);
    auto *count = textLabel(QString(), coverage);
    count->setObjectName("count_" + stageKeys[i]);
    count->setFont(PaleoTheme::monoFont());
    coverageRows->addWidget(count, i, 2);
  }
  coverageRows->setColumnStretch(1, 1);
  coverageLayout->addLayout(coverageRows);
  m_content->addWidget(coverage);
  auto *horizonTitle = textLabel(tr("分层位进度"), body);
  PaleoTheme::applyThemedStyleSheet(horizonTitle, [] { return PaleoTheme::sectionTitleStyleSheet(); });
  m_content->addWidget(horizonTitle);
  m_horizons = new QGridLayout;
  m_horizons->setSpacing(t.spacingSm);
  m_content->addLayout(m_horizons);
  m_content->addWidget(textLabel(tr("“已有”表示目录中已登记成果，含历史版本，不代表质量验收或解释完成度。测区"
                                    "面积取有效角点并集；井位包络仅估算分布范围。"),
                                 body, true));
  m_content->addStretch();
  m_scroll->setWidget(body);
  auto *timer = new QTimer(this);
  timer->setSingleShot(true);
  timer->setInterval(100);
  connect(timer, &QTimer::timeout, this, &ProjectStatisticsPage::refresh);
  if (catalog)
    connect(catalog, &DataCatalog::changed, timer, qOverload<>(&QTimer::start));
  refresh();
}

void ProjectStatisticsPage::refresh()
{
  const auto data = ProjectAreaSummary::snapshot(m_catalog);
  const bool ready = !data.isEmpty();
  const auto locale = QLocale();
  findChild<QPushButton *>("dashboardOpenMap")->setEnabled(ready);
  const auto number = [&](const char *key)
  { return ready ? locale.toString(data.value(key).toInt()) : tr("—"); };
  const auto area = data.value("survey_area_km2", -1).toDouble();
  clearGrid(m_metrics, m_metricCards);
  clearGrid(m_horizons, m_horizonCards);
  auto *body = m_scroll->widget();
  m_metricCards << metricCard(tr("井资料"), tr("%1 口").arg(number("wells")),
                              tr("有坐标 %1 口").arg(number("positioned_wells")), "dashboardWells", body);
  m_metricCards << metricCard(tr("测区覆盖面积"),
                              ready && area >= 0 ? tr("%1 km²").arg(locale.toString(area, 'f', 3))
                                                 : tr("未登记边界"),
                              tr("地震测区 %1 个").arg(number("seismic_surveys")), "dashboardArea", body);
  m_metricCards << metricCard(tr("资料资产"), tr("%1 项").arg(number("assets")),
                              tr("累计版本 %1 个").arg(number("versions")), "dashboardAssets", body);
  m_metricCards << metricCard(tr("现有古地理图"), tr("%1 项").arg(number("paleomaps")),
                              tr("输出版本 %1 个").arg(number("outputs")), "dashboardMaps", body);
  const auto horizons = data.value("horizons").toList();
  const auto counts = data.value("stage_counts").toMap();
  for (const auto &key : stageKeys)
  {
    const int count = counts.value(key).toInt();
    auto *bar = findChild<QProgressBar *>("coverage_" + key);
    bar->setMaximum(qMax(1, int(horizons.size())));
    bar->setValue(count);
    bar->setAccessibleDescription(tr("%1 / %2 个层位已有成果").arg(count).arg(horizons.size()));
    findChild<QLabel *>("count_" + key)->setText(tr("%1 / %2").arg(count).arg(horizons.size()));
  }
  for (const auto &v : horizons)
    m_horizonCards << horizonCard(v.toMap(), body);
  const auto envelopeKm2 = data.value("well_envelope_km2").toDouble();
  auto *envelope = metricCard(
      tr("井位包络面积（估算）"),
      ready && envelopeKm2 >= 0 ? tr("%1 km²").arg(locale.toString(envelopeKm2, 'f', 3)) : tr("—"),
      ready ? tr("用于衡量井位分布范围，非工区边界面积") : tr("打开工区后显示统计信息"), "dashboardEnvelope",
      body);
  if (auto *old = body->findChild<QWidget *>("dashboardEnvelopeCard", Qt::FindDirectChildrenOnly))
  {
    m_content->removeWidget(old);
    delete old;
  }
  envelope->setObjectName("dashboardEnvelopeCard");
  m_content->insertWidget(m_content->count() - 2, envelope);
  m_columns = 0;
  arrangeCards();
  refreshSurveyMap();
}

void ProjectStatisticsPage::refreshSurveyMap()
{
  if (!m_mapCanvas || !isVisible())
    return;
  // 导入期间 DataCatalog::changed 密集触发防抖刷新；页面隐藏时整个跳过，
  // showEvent 再补刷。停渲染→改层纪律在 DashboardSurveyMap::update。
  QVector<CatalogEntity> wells;
  if (m_catalog && m_catalog->isOpen())
    wells = m_catalog->entities(QStringLiteral("well"));
  DashboardSurveyMap::update(m_mapCanvas, wells);
}

bool ProjectStatisticsPage::eventFilter(QObject *watched, QEvent *event)
{
  if (watched == m_scroll->viewport() && event->type() == QEvent::Resize)
    arrangeCards();
  return QWidget::eventFilter(watched, event);
}

void ProjectStatisticsPage::showEvent(QShowEvent *event)
{
  QWidget::showEvent(event);
  if (!m_mapCanvas)
    buildSurveyMapCard();
  else
    refreshSurveyMap();
}

void ProjectStatisticsPage::buildSurveyMapCard()
{
  if (m_mapCanvas || !m_content || !m_scroll->widget())
    return;
  auto *body = m_scroll->widget();
  // 工区地图直接进看板：井位，零设置（滚轮缩放、拖拽漫游）；
  // 底图/比例尺/网格等完整工具在「工区全景地图」页，头部按钮直达。
  auto *mapCard = card(body);
  mapCard->setObjectName("dashboardSurveyMapCard");
  auto *mapLay = cardLayout(mapCard);
  auto *mapTitle = textLabel(tr("工区地图"), mapCard);
  PaleoTheme::applyThemedStyleSheet(mapTitle, [] { return PaleoTheme::sectionTitleStyleSheet(); });
  mapLay->addWidget(mapTitle);
  QFont labelFont = PaleoTheme::bodyFont(PaleoTheme::tokens().bodyPt);
  labelFont.setWeight(QFont::Medium);
  m_mapCanvas = DashboardSurveyMap::create(
      mapCard, {PaleoTheme::kColorPrimary, labelFont, double(PaleoTheme::tokens().bodyPt), tr("井位")});
  m_mapCanvas->setObjectName(QStringLiteral("dashboardSurveyMap"));
  QWidget *canvasWidget = DashboardSurveyMap::canvasWidget(m_mapCanvas);
  canvasWidget->setObjectName(QStringLiteral("dashboardSurveyMapCanvas"));
  m_mapCanvas->setMinimumHeight(340);
  mapLay->addWidget(m_mapCanvas);
  // 地图要素（非设置）：比例尺 + 指北针 + 图例行。
  m_decorations = PaleoDecorationManager::forCanvasWidget(canvasWidget, canvasWidget);
  m_decorations->setObjectName(QStringLiteral("dashboardSurveyDecor"));
  m_decorations->setScaleBarEnabled(true);
  m_decorations->setNorthArrowEnabled(true);
  auto *legend = new QLabel(tr("<span style=\"color:%1\">●</span>&nbsp;井位")
                                 .arg(qssHex(PaleoTheme::kColorPrimary)), mapCard);
  legend->setTextFormat(Qt::RichText);
  PaleoTheme::applyThemedStyleSheet(legend, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  mapLay->addWidget(legend);
  // 插在头部之后、指标卡之前，保持看板版面语义（地图在上）。
  m_content->insertWidget(1, mapCard);
  refreshSurveyMap();
}
void ProjectStatisticsPage::arrangeCards()
{
  // Breakpoints only control layout; individual cards retain native font/spacing tokens.
  const int width = m_scroll->viewport()->width();
  const int columns = width >= 1040 ? 4 : width >= 560 ? 2 : 1;
  if (columns == m_columns || !m_metrics || !m_horizons)
    return;
  m_columns = columns;
  while (auto *item = m_metrics->takeAt(0))
    delete item;
  while (auto *item = m_horizons->takeAt(0))
    delete item;
  for (int i = 0; i < 4; ++i)
  {
    m_metrics->setColumnStretch(i, i < columns ? 1 : 0);
    m_horizons->setColumnStretch(i, i < qMin(columns, 2) ? 1 : 0);
  }
  for (int i = 0; i < m_metricCards.size(); ++i)
    m_metrics->addWidget(m_metricCards[i], i / columns, i % columns);
  const int horizonColumns = qMin(columns, 2);
  for (int i = 0; i < m_horizonCards.size(); ++i)
    m_horizons->addWidget(m_horizonCards[i], i / horizonColumns, i % horizonColumns);
}
