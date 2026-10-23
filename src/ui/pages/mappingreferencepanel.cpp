// 层：视图
#include "mappingreferencepanel.h"

#include "../../qgis/mappingreferencecanvas.h"
#include "../decorations/paleodecorations.h"
#include "../paleotheme.h"

#include <QComboBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QTimer>
#include <QToolButton>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
MappingReferencePanel::MappingReferencePanel(QgsMapCanvas *mainCanvas,
                                             QgsProject *project, QWidget *parent)
    : QWidget(parent)
{
  setObjectName(QStringLiteral("mappingReferencePanel"));
  auto *layout = new QVBoxLayout(this);
  const auto &tokens = PaleoTheme::tokens();
  layout->setContentsMargins(tokens.spacingSm, tokens.spacingSm, tokens.spacingSm, tokens.spacingSm);
  layout->setSpacing(tokens.spacingXs);
  auto *header = new QHBoxLayout;
  header->setSpacing(tokens.spacingXs);
  m_sources = new QComboBox(this);
  m_sources->setObjectName(QStringLiteral("referenceSource"));
  m_sources->setAccessibleName(tr("参考证据"));
  m_sources->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  m_sources->setMinimumContentsLength(12);
  m_linked = new QToolButton(this);
  m_linked->setObjectName(QStringLiteral("referenceLinked"));
  m_linked->setText(tr("联动"));
  m_linked->setCheckable(true);
  m_linked->setChecked(true);
  m_linked->setAccessibleName(tr("与主画布联动"));
  m_linked->setToolTip(tr("打开后证据视图跟随主画布的比例尺和中心，不反向改主图；关闭后可独立浏览"));
  m_remove = new QToolButton(this);
  m_remove->setObjectName(QStringLiteral("referenceRemoveView"));
  m_remove->setText(QStringLiteral("−"));
  m_remove->setAccessibleName(tr("关闭此证据"));
  m_remove->setToolTip(tr("关闭此证据"));
  header->addWidget(m_sources, 1);
  header->addWidget(m_linked);
  header->addWidget(m_remove);
  layout->addLayout(header);
  m_empty = new QLabel(tr("从上方下拉框选择参考证据。\n"
                         "可选择地震相、测井相、单因素图或参考相图。"), this);
  m_empty->setObjectName(QStringLiteral("referenceEmptyState"));
  m_empty->setWordWrap(true);
  m_empty->setAlignment(Qt::AlignCenter);
  layout->addWidget(m_empty, 1);
  m_view = new MappingReferenceCanvas(mainCanvas, project, this);
  m_view->setMinimumSize(240, 160);
  layout->addWidget(m_view, 1);
  m_view->hide();
  m_decor = new PaleoDecorationManager(canvas());
  m_decor->setScaleBarEnabled(true);
  connect(m_sources, &QComboBox::currentIndexChanged, this, [this] {
    clearReference();
    updateCaption();
    if (!sourceId().isEmpty()) emit referenceRequested(sourceId());
  });
  connect(m_linked, &QToolButton::toggled, this, &MappingReferencePanel::updateLink);
  connect(m_remove, &QToolButton::clicked, this, &MappingReferencePanel::removeViewRequested);
  setMinimumSize(260, 260);
  refreshSources();
}

MappingReferencePanel::~MappingReferencePanel()
{
  disconnect(m_view, nullptr, this, nullptr);
  m_view->clearReference();
}
QgsMapCanvas *MappingReferencePanel::canvas() const { return m_view->canvas(); }
QString MappingReferencePanel::sourceId() const { return m_sources->currentData().toString(); }
bool MappingReferencePanel::hasReference() const { return m_view->hasReference(); }

QString MappingReferencePanel::category(const QVariantMap &row)
{
  const auto kind = row.value("kind").toString();
  const auto id = row.value("id").toString();
  const auto metadata = row.value("parameters").toMap();
  if (kind.startsWith(QLatin1String("single_factor")) || kind == QLatin1String("contour_lines") ||
      !metadata.value("factor_id").toString().isEmpty() || id.startsWith(QLatin1String("wellfactor."))) return QStringLiteral("factor");
  if (kind.contains(QLatin1String("well")) || id.startsWith(QLatin1String("welllog.")) ||
      metadata.value("result_kind") == "formation_prediction")
    return QStringLiteral("well");
  if (kind.contains(QLatin1String("seismic"))) return QStringLiteral("seismic");
  return QStringLiteral("reference");
}

QVariantMap MappingReferencePanel::selectedSource() const
{
  for (const auto &row : m_rows)
    if (row.toMap().value("id").toString() == sourceId()) return row.toMap();
  return {};
}

void MappingReferencePanel::setSources(const QVariantList &sources, const QString &activeHorizon)
{
  m_rows = sources;
  m_activeHorizon = activeHorizon;
  refreshSources();
  updateCaption();
}

void MappingReferencePanel::refreshSources()
{
  const auto selected = sourceId();
  const QSignalBlocker blocker(m_sources);
  m_sources->clear();
  m_sources->addItem(tr("请选择参考证据"), QString());
  for (const auto &entry : m_rows) {
    const auto row = entry.toMap();
    if (row.value("type") != "vector" && row.value("type") != "raster") continue;
    const auto family = category(row);
    const auto familyName = family == QLatin1String("seismic") ? tr("地震相")
        : family == QLatin1String("well") ? tr("测井相")
        : family == QLatin1String("factor") ? tr("单因素图") : tr("参考图");
    const auto title = tr("%1 · %2").arg(familyName, row.value("name").toString());
    m_sources->addItem(title, row.value("id"));
    m_sources->setItemData(m_sources->count() - 1, row.value("path"), Qt::ToolTipRole);
  }
  m_sources->setCurrentIndex(qMax(0, m_sources->findData(selected)));
  m_sources->setEnabled(m_sources->count() > 1);
  m_sources->setToolTip(m_sources->isEnabled() ? tr("选择要对照的图件；同一面板可随时切换来源")
                                            : tr("暂无此类图件，请先导入参考相图或生成预测、单因素成果"));
  if (sourceId() != selected) clearReference();
  updateCaption();
}

void MappingReferencePanel::selectSource(const QString &id)
{
  const int index = m_sources->findData(id);
  if (index >= 0) {
    if (index == m_sources->currentIndex() && !hasReference() && !id.isEmpty())
      emit referenceRequested(id);
    else m_sources->setCurrentIndex(index);
  }
}

void MappingReferencePanel::setReference(QgsMapLayer *layer, const QVariantMap &metadata)
{
  clearReference();
  if (!layer) return;
  m_metadata = metadata;
  m_empty->hide();
  m_view->show();
  m_view->setReference(layer, metadata);
  updateCaption();
  setProperty("paleo.referenceVersion", selectedSource().value("version_id"));
  emit referenceChanged(selectedSource().value("name").toString(), selectedSource().value("version_id").toString());
}

void MappingReferencePanel::clearReference(const QString &message)
{
  m_view->clearReference();
  m_metadata.clear();
  m_view->hide();
  m_empty->setText(message.isEmpty() ? tr("从上方下拉框选择参考证据。\n"
                                         "可选择地震相、测井相、单因素图或参考相图。") : message);
  m_empty->show();
  setProperty("paleo.referenceVersion", QString());
  emit referenceChanged(QString(), QString());
}

void MappingReferencePanel::updateCaption()
{
  const auto row = selectedSource();
  const auto horizon = row.value("horizon").toString();
  const QString note = row.isEmpty() ? tr("选择本视图的参考证据，与主图联动对照。")
      : !horizon.isEmpty() && !m_activeHorizon.isEmpty() && horizon != m_activeHorizon
          ? tr("参考层位：%1 · 当前绘制层位：%2（跨层位对照）").arg(horizon, m_activeHorizon)
          : tr("参考层位：%1 · 保留所选版本，与主图独立管理").arg(horizon.isEmpty() ? tr("共享资料") : horizon);
  m_sources->setToolTip(note);
}

void MappingReferencePanel::setViewActive(bool active)
{
  m_view->setViewActive(active);
}

void MappingReferencePanel::updateLink()
{
  m_view->setLinked(m_linked->isChecked());
}

// D-12（full-audit-20261020）：#547 裁决允许关闭最后一个视图——按钮恒可点，
// 仅当要关的是最后一个视图时换提示（原 setCanRemove 的 bool 是死参，恒
// setEnabled(true)，误导维护者以为存在「至少一视图」保护）。
void MappingReferencePanel::setRemoveHint(bool lastView)
{
  m_remove->setEnabled(true);
  m_remove->setToolTip(lastView ? tr("将关闭最后一个证据视图（可随时再添加）")
                                 : tr("关闭此证据"));
}

MappingEvidencePanel::MappingEvidencePanel(QgsMapCanvas *mainCanvas, QgsProject *project,
                                           QWidget *parent)
    : QWidget(parent), m_mainCanvas(mainCanvas), m_project(project)
{
  setObjectName(QStringLiteral("mappingEvidencePanel"));
  setProperty("paleo.scrollHost", true); // 内部滚动仅作用于视图区，工具行保持可见。
  const auto &tokens = PaleoTheme::tokens();
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(tokens.spacingSm, tokens.spacingSm, tokens.spacingSm, tokens.spacingSm);
  layout->setSpacing(tokens.spacingXs);
  auto *tools = new QHBoxLayout;
  m_count = new QLabel(this);
  m_count->setObjectName(QStringLiteral("referenceViewCount"));
  auto *add = new QPushButton(tr("添加视图"), this);
  add->setObjectName(QStringLiteral("referenceAddView"));
  add->setToolTip(tr("添加参考视图，按两列排列，可滚动查看更多证据"));
  tools->addWidget(m_count);
  tools->addStretch();
  tools->addWidget(add);
  layout->addLayout(tools);
  m_scroll = new QScrollArea(this);
  m_scroll->setObjectName(QStringLiteral("referenceViewsScroll"));
  m_scroll->setFrameShape(QFrame::NoFrame);
  m_scroll->setWidgetResizable(true);
  m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  auto *body = new QWidget(m_scroll);
  body->setObjectName(QStringLiteral("referenceViewsRow"));
  body->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);
  m_row = new QGridLayout(body);
  m_row->setContentsMargins(0, 0, 0, 0);
  m_row->setSpacing(tokens.spacingSm);
  m_scroll->setWidget(body);
  layout->addWidget(m_scroll, 1);
  // D-12（full-audit-20261020）：空态引导——全部视图关闭后仅「0 个视图」
  // 计数与空白滚动区，新用户无从得知可恢复；§42.4 空态暖引导同仓既有口径。
  m_emptyHint = new QLabel(tr("无证据视图——点「添加视图」恢复对照"), this);
  m_emptyHint->setObjectName(QStringLiteral("referenceEmptyHint"));
  m_emptyHint->setWordWrap(true);
  m_emptyHint->setAlignment(Qt::AlignCenter);
  m_emptyHint->setAttribute(Qt::WA_TransparentForMouseEvents);
  PaleoTheme::applyThemedStyleSheet(m_emptyHint, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  m_emptyHint->hide();
  layout->addWidget(m_emptyHint);
  connect(add, &QPushButton::clicked, this, [this] { addView(); });
  addView();
  addView();
}

MappingReferencePanel *MappingEvidencePanel::addView()
{
  auto *view = new MappingReferencePanel(m_mainCanvas, m_project, m_scroll->widget());
  view->setSources(m_sources, m_horizon);
  view->setViewActive(m_active);
  m_views << view;
  connect(view, &MappingReferencePanel::referenceRequested, this, [this, view](const QString &id) {
    emit referenceRequested(view, id);
  });
  connect(view, &MappingReferencePanel::removeViewRequested, this, [this, view] { removeView(view); });
  layoutViews();
  QTimer::singleShot(0, view, [this, view] {
    if (m_views.contains(view)) m_scroll->ensureWidgetVisible(view);
  });
  return view;
}

void MappingEvidencePanel::removeView(MappingReferencePanel *view)
{
  if (!m_views.removeOne(view)) return;
  view->clearReference();
  m_row->removeWidget(view);
  view->hide();
  view->deleteLater();
  layoutViews();
}

void MappingEvidencePanel::layoutViews()
{
  while (QLayoutItem *item = m_row->takeAt(0))
    delete item;
  // DESIGN：按两列共享空间；第三个视图从下一行开始。
  for (qsizetype i = 0; i < m_views.size(); ++i) {
    auto *view = m_views[i];
    view->setMinimumSize(240, 200);
    view->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    view->setRemoveHint(m_views.size() == 1);
    view->setAccessibleName(tr("证据视图 %1").arg(i + 1));
    m_row->addWidget(view, int(i) / 2, int(i) % 2);
    view->show();
  }
  auto *body = m_scroll->widget();
  body->setMinimumWidth(0);
  m_row->setColumnStretch(0, 1);
  m_row->setColumnStretch(1, m_views.size() > 1 ? 1 : 0);
  m_row->activate();
  m_count->setText(tr("%1 个视图").arg(m_views.size()));
  m_scroll->setVisible(!m_views.isEmpty()); // D-12：空态收起滚动区，展示引导
  m_emptyHint->setVisible(m_views.isEmpty());
}

void MappingEvidencePanel::setSources(const QVariantList &sources, const QString &horizon)
{
  m_sources = sources;
  m_horizon = horizon;
  for (auto *view : m_views) view->setSources(sources, horizon);
}

void MappingEvidencePanel::clearReferences(const QString &message)
{
  for (auto *view : m_views) view->clearReference(message);
}

void MappingEvidencePanel::setViewActive(bool active)
{
  m_active = active;
  for (auto *view : m_views) view->setViewActive(active);
}

MappingReferencePanel *MappingEvidencePanel::showSource(const QString &id)
{
  for (auto *view : m_views)
    if (view->sourceId() == id) {
      view->selectSource(id);
      m_scroll->ensureWidgetVisible(view);
      return view;
    }
  auto *view = [&]() -> MappingReferencePanel * {
    for (auto *candidate : m_views) if (candidate->sourceId().isEmpty()) return candidate;
    return addView();
  }();
  view->selectSource(id);
  m_scroll->ensureWidgetVisible(view);
  return view;
}
