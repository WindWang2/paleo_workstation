// 层：视图
#include "mappingreferencepanel.h"
#include "../../domain/facieshierarchy.h"

#include "../../qgis/mappingreferencecanvas.h"
#include "../decorations/paleodecorations.h"
#include "../paleotheme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QTreeWidget>
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
  m_remove = new QToolButton(this);
  m_remove->setObjectName(QStringLiteral("referenceRemoveView"));
  m_remove->setText(tr("移除"));
  m_remove->setAccessibleName(tr("移除此证据视图"));
  header->addWidget(m_sources, 1);
  header->addWidget(m_remove);
  layout->addLayout(header);
  auto *actions = new QHBoxLayout;
  actions->setSpacing(tokens.spacingXs);
  m_linked = new QCheckBox(tr("范围与光标联动"), this);
  m_linked->setObjectName(QStringLiteral("referenceLinked"));
  m_linked->setChecked(true);
  m_linked->setToolTip(tr("双向联动主图的范围和光标；关闭后可独立浏览此图"));
  m_fullExtent = new QPushButton(tr("全图"), this);
  m_fullExtent->setObjectName(QStringLiteral("referenceFullExtent"));
  m_fullExtent->setEnabled(false);
  m_fullExtent->setToolTip(tr("请先选择可读取的参考图件"));
  auto *legendToggle = new QToolButton(this);
  legendToggle->setObjectName("referenceLegendToggle");
  legendToggle->setText(tr("图例"));
  legendToggle->setCheckable(true);
  legendToggle->setEnabled(false);
  legendToggle->setToolTip(tr("在视图下方展开图例，查看此版本的符号和三级归属"));
  actions->addWidget(m_linked);
  actions->addWidget(legendToggle);
  actions->addStretch();
  actions->addWidget(m_fullExtent);
  layout->addLayout(actions);
  m_caption = new QLabel(this);
  m_caption->setObjectName(QStringLiteral("referenceCaption"));
  m_caption->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(m_caption, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  layout->addWidget(m_caption);
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
  auto *legendTree = new QTreeWidget(this);
  legendTree->setObjectName("referenceLegendTree");
  legendTree->setAccessibleName(tr("当前参考图件图例"));
  legendTree->setHeaderLabel(tr("图例"));
  legendTree->setMaximumHeight(160);
  legendTree->setIconSize(QSize(tokens.spacingMd * 2, tokens.spacingMd));
  legendTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
  legendTree->hide();
  layout->addWidget(legendTree);
  connect(legendToggle, &QToolButton::toggled, legendTree, &QWidget::setVisible);
  m_decor = new PaleoDecorationManager(canvas());
  m_decor->setScaleBarEnabled(true);
  connect(m_sources, &QComboBox::currentIndexChanged, this, [this] {
    clearReference();
    updateCaption();
    if (!sourceId().isEmpty()) emit referenceRequested(sourceId());
  });
  connect(m_linked, &QCheckBox::toggled, this, &MappingReferencePanel::updateLink);
  connect(m_fullExtent, &QPushButton::clicked, this, [this] {
    // 全图是此参考图的浏览操作，先解除联动，保留主图正在绘制的范围。
    m_linked->setChecked(false);
    m_view->zoomToFullExtent();
  });
  connect(m_remove, &QToolButton::clicked, this, &MappingReferencePanel::removeViewRequested);
  connect(m_view, &MappingReferenceCanvas::legendChanged, this,
          [this, legendTree, legendToggle](const QVariantList &entries) {
    legendTree->clear();
    legendToggle->setEnabled(!entries.isEmpty());
    const auto level = entries.value(0).toMap().value("level").toString();
    const auto title = level.isEmpty() ? tr("参考图例") : FaciesHierarchy::title(level);
    legendTree->setHeaderLabel(title + (m_metadata.value("mock").toBool() ? tr(" · Mock") : QString()));
    QVector<QTreeWidgetItem *> parents;
    for (const auto &entry : FaciesHierarchy::legendTree(entries)) {
      const auto row = entry.toMap();
      const int depth = row.value("depth", 0).toInt();
      while (parents.size() > depth) parents.removeLast();
      auto *parent = parents.isEmpty() ? legendTree->invisibleRootItem() : parents.last();
      auto *node = new QTreeWidgetItem(parent, {row.value("name").toString()});
      node->setToolTip(0, row.value("path", row.value("name")).toString());
      if (!row.value("heading").toBool()) node->setIcon(0, row.value("legend_icon").value<QIcon>());
      parents << node;
    }
    legendTree->expandAll();
  });
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
  m_fullExtent->setEnabled(true);
  m_fullExtent->setToolTip(tr("查看此参考图全范围，并解除联动以保留主图范围"));
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
  m_fullExtent->setEnabled(false);
  m_fullExtent->setToolTip(tr("请先选择可读取的参考图件"));
  setProperty("paleo.referenceVersion", QString());
  emit referenceChanged(QString(), QString());
}

void MappingReferencePanel::updateCaption()
{
  const auto row = selectedSource();
  const auto horizon = row.value("horizon").toString();
  m_caption->setText(row.isEmpty() ? tr("选择本视图的参考证据，与主图联动对照。")
      : !horizon.isEmpty() && !m_activeHorizon.isEmpty() && horizon != m_activeHorizon
          ? tr("参考层位：%1 · 当前绘制层位：%2（跨层位对照）").arg(horizon, m_activeHorizon)
          : tr("参考层位：%1 · 保留所选版本，与主图独立管理").arg(horizon.isEmpty() ? tr("共享资料") : horizon));
}

void MappingReferencePanel::setViewActive(bool active)
{
  m_view->setViewActive(active);
}

void MappingReferencePanel::updateLink()
{
  m_view->setLinked(m_linked->isChecked());
}

void MappingReferencePanel::setCanRemove(bool canRemove)
{
  m_remove->setEnabled(canRemove);
  m_remove->setToolTip(canRemove ? tr("移除此视图，其他视图继续保留")
                                : tr("至少保留一个视图；可关闭整个参考证据面板"));
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
  add->setToolTip(tr("在本面板内增加一个视图，再用视图上方的下拉框选择参考证据"));
  tools->addWidget(m_count);
  tools->addStretch();
  tools->addWidget(add);
  layout->addLayout(tools);
  m_scroll = new QScrollArea(this);
  m_scroll->setObjectName(QStringLiteral("referenceViewsScroll"));
  m_scroll->setFrameShape(QFrame::NoFrame);
  m_scroll->setWidgetResizable(true);
  auto *body = new QWidget(m_scroll);
  m_grid = new QGridLayout(body);
  m_grid->setContentsMargins(0, 0, 0, 0);
  m_grid->setSpacing(tokens.spacingSm);
  m_grid->setColumnStretch(0, 1);
  m_grid->setColumnStretch(1, 1);
  m_scroll->setWidget(body);
  layout->addWidget(m_scroll, 1);
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
  if (m_views.size() <= 1 || !m_views.removeOne(view)) return;
  view->clearReference();
  m_grid->removeWidget(view);
  view->hide();
  view->deleteLater();
  layoutViews();
}

void MappingEvidencePanel::layoutViews()
{
  for (auto *view : m_views) m_grid->removeWidget(view);
  for (qsizetype i = 0; i < m_views.size(); ++i) {
    auto *view = m_views[i];
    view->setCanRemove(m_views.size() > 1);
    view->setAccessibleName(tr("证据视图 %1").arg(i + 1));
    m_grid->addWidget(view, i / 2, i % 2, 1, m_views.size() == 1 ? 2 : 1);
    view->show();
  }
  m_count->setText(tr("%1 个视图").arg(m_views.size()));
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
