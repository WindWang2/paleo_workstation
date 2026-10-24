// 层：视图
#include "datagraphpage.h"
#include "datagraphcanvas.h"
#include "datagraphstyle.h"
#include "../paleotheme.h"
#include "../../services/previewdoc.h"
#include "../../catalog/datacatalog.h"
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHeaderView>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QTreeWidget>
#include <QTextBrowser>
#include <QPushButton>
#include <QComboBox>
#include <QSignalBlocker>
#include <QMenu>
#include <QLabel>
#include <QFrame>
#include <QToolButton>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QTimer>
#include <QWidgetAction>
#include <QSpinBox>
using namespace paleo::datagraph;
QWidget *DataGraphPage::createGraphPanels() {
  const auto &t = PaleoTheme::tokens();
  auto *surface = new DataGraphSurface(this); m_graphSurface = surface;
  surface->setObjectName(QStringLiteral("dataGraphSurface"));
  m_scene = new QGraphicsScene(this);
  auto *canvas = new DataGraphCanvas(m_scene, surface);
  canvas->clearFocusRequested = [this] { selectNode({}, false); };
  canvas->viewTransformChanged = [this] { rebuildGraphEdges(); selectNode(m_selected, false); };
  m_view = canvas;
  m_view->setObjectName("dataGraphView");
  m_view->setRenderHint(QPainter::Antialiasing);
  m_view->setDragMode(QGraphicsView::ScrollHandDrag);
  m_view->setAccessibleName(tr("资料图谱画布"));
  // 空场景下 QGraphicsView 的 sizeHint 极小，分栏的拉伸权重 = 初始尺寸 ×
  // stretch——先给画布一个初始尺寸意见，图谱才能拿到主区；宽度之后由用户拖动决定。
  m_view->setFrameShape(QFrame::NoFrame);
  m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  surface->view = m_view;
  auto *details = new QFrame(surface); m_detailsPanel = details;
  details->setObjectName(QStringLiteral("dataGraphDetailsPanel"));
  auto *relations = new QFrame(surface); m_relationsPanel = relations;
  relations->setObjectName(QStringLiteral("dataGraphRelationsPanel"));
  auto *rl = new QVBoxLayout(relations);
  rl->setContentsMargins(t.spacingMd, t.spacingMd, t.spacingMd, t.spacingMd);
  rl->setSpacing(t.spacingSm);
  auto *dl = new QVBoxLayout(details);
  dl->setContentsMargins(t.spacingMd, t.spacingMd, t.spacingMd, t.spacingMd);
  dl->setSpacing(t.spacingSm);
  const auto heading = [&](QVBoxLayout *layout, QWidget *panel, const QString &text, bool close) {
    auto *row = new QHBoxLayout; auto *label = new QLabel(text, panel);
    auto font = label->font(); font.setBold(true); font.setPointSize(t.bodyPt + 2); label->setFont(font);
    row->addWidget(label, 1);
    if (close) { auto *button = new QToolButton(panel); button->setText(QString::fromUtf8("×")); button->setToolTip(tr("收起节点详情"));
      connect(button, &QToolButton::clicked, surface, [surface] { surface->detailsWanted = false; surface->arrange(); }); row->addWidget(button); }
    layout->addLayout(row); return label;
  };
  heading(dl, details, tr("节点详情"), true);
  m_relationHeading = heading(rl, relations, tr("图谱概览"), false);
  m_graphLegend = new QLabel(relations); m_graphLegend->setWordWrap(true); m_graphLegend->setTextFormat(Qt::RichText);
  rl->addWidget(m_graphLegend);
  m_relations = new QTreeWidget(relations);
  rl->addWidget(m_relations, 1);
  m_relations->setObjectName("dataGraphRelations");
  m_relations->setHeaderLabels({tr("方向"), tr("关系"), tr("关联资料")});
  m_relations->setRootIsDecorated(false);
  m_relations->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  m_relations->header()->resizeSection(1, 88);
  m_relations->header()->setSectionResizeMode(2, QHeaderView::Stretch);
  m_relations->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_relations->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_relations->setMaximumHeight(200); m_relations->setMinimumHeight(100);
  connect(m_relations, &QTreeWidget::itemActivated, this,
          [this](QTreeWidgetItem *item) {
            const auto id = item->data(0, Qt::UserRole).toString();
            QTimer::singleShot(0, this, [this, id] {
              for (const auto &node : m_graph.nodes)
                if (node.id == id) {
                  selectNode(node, true);
                  if (auto *target = m_items.value(id))
                    m_view->centerOn(target);
                  break;
                }
            });
          });
  m_details = new QTextBrowser(details);
  m_details->setOpenLinks(false); m_details->setOpenExternalLinks(false);
  m_details->setReadOnly(true);
  m_details->setPlaceholderText(tr(
      "选择节点查看资料、版本、文件路径与关联。双击分组展开，双击文件预览。"));
  m_details->setObjectName(QStringLiteral("dataGraphNodeDetails"));
  dl->addWidget(m_details, 1);
  m_details->setFrameShape(QFrame::NoFrame);
  m_open = new QPushButton(tr("预览文件 / 版本"), details);
  m_locate = new QPushButton(tr("在数据管理中定位"), details);
  m_folder = new QPushButton(tr("打开文件所在目录"), details);
  m_trace = new QPushButton(tr("追溯这个成果"), details);
  m_impact = new QPushButton(tr("查看来源的下游影响"), details);
  m_impact->setObjectName("dataGraphImpact");
  m_related = new QPushButton(tr("展开这个节点的关联"), details);
  m_map = new QPushButton(tr("在地图 / 图件工作台中显示"), details);
  m_reference = new QPushButton(tr("打开联动参考图"), details);
  m_wells = new QPushButton(tr("查看 / 修订测井预测相"), details);
  auto *tasks = new QPushButton(tr("打开任务中心"), details);
  m_manage = new QPushButton(tr("文件 / 版本操作"), details);
  auto *manageMenu = new QMenu(m_manage);
  for (const auto &[title, action] :
       {qMakePair(tr("版本管理 / 对比 / 回滚"), QStringLiteral("versions")),
        qMakePair(tr("调整资料归属与用途"), QStringLiteral("role")),
        qMakePair(tr("添加标签"), QStringLiteral("tag")),
        qMakePair(tr("移入回收清单"), QStringLiteral("remove"))})
    connect(manageMenu->addAction(title), &QAction::triggered, this,
            [this, action, manageMenu] {
              const auto asset = manageMenu->property("asset_id").toString();
              if (m_presentation.removedIds.contains(asset))
                return;
              emit managementRequested(
                  action, asset, manageMenu->property("version_id").toString());
            });
  connect(manageMenu, &QMenu::aboutToShow, this, [this, manageMenu] {
    manageMenu->setProperty("asset_id", m_selected.assetId);
    manageMenu->setProperty("version_id", m_selected.versionId);
  });
  m_manage->setMenu(manageMenu);
  auto *actionGrid = new QGridLayout;
  int row = 0;
  for (auto *button : {m_open, m_locate, m_folder}) { dl->addWidget(button); button->setEnabled(false); }
  for (auto *button : {m_related, m_trace, m_impact, m_map,
                       m_reference, m_wells, m_manage}) {
    actionGrid->addWidget(button, row, 0);
    button->setEnabled(false);
    ++row;
  }
  actionGrid->addWidget(tasks, row, 0);
  rl->addLayout(actionGrid);
  connect(tasks, &QPushButton::clicked, this,
          &DataGraphPage::taskCenterRequested);
  connect(m_impact, &QPushButton::clicked, this, [this] {
    const auto version = m_selected.evidence.value("source_version", m_selected.versionId).toString();
    if (version.isEmpty()) return;
    m_focus = versionKey(version);
    {
      // 模式切换由下方显式 request 统一发起：不屏蔽信号会同事件双发查询。
      const QSignalBlocker block(m_mode);
      m_mode->setCurrentIndex(m_mode->findData(int(Mode::Impact)));
    }
    request();
  });
  connect(m_related, &QPushButton::clicked, this, [this] {
    if (m_selected.id.isEmpty())
      return;
    m_focus = m_selected.id;
    {
      const QSignalBlocker block(m_mode);
      m_mode->setCurrentIndex(m_mode->findData(int(Mode::Relations)));
    }
    request();
  });
  for (const auto &[button, action] :
       {qMakePair(m_map, QStringLiteral("show")),
        qMakePair(m_reference, QStringLiteral("compare")),
        qMakePair(m_wells, QStringLiteral("welltracks"))})
    connect(button, &QPushButton::clicked, this, [this, action] {
      if (action == "show" && !m_selected.featureUid.isEmpty()) {
        emit evidenceTargetRequested(nodeTarget(m_selected)); return;
      }
      if (!m_doc || !m_doc->catalog())
        return;
      const auto version =
          m_selected.versionId.isEmpty()
              ? m_doc->catalog()->currentVersion(m_selected.assetId)
              : m_doc->catalog()->versionById(m_selected.versionId);
      if (!version.id.isEmpty())
        emit workspaceRequested(version.id, action);
    });
  surface->details = details; surface->relations = relations; details->hide();
  m_summary->setParent(surface); surface->summary = m_summary;
  auto *zoom = new QFrame(surface); zoom->setObjectName(QStringLiteral("dataGraphZoomControls"));
  auto *zoomLayout = new QHBoxLayout(zoom);
  zoomLayout->setContentsMargins(t.spacingXs, t.spacingXs, t.spacingXs, t.spacingXs);
  zoomLayout->setSpacing(t.spacingXs);
  for (const auto *name : {"dataGraphZoomOut", "dataGraphFit", "dataGraphZoomIn"})
    if (auto *button = findChild<QToolButton *>(QLatin1String(name))) { button->setParent(zoom); zoomLayout->addWidget(button); }
  surface->zoom = zoom;
  if (auto *toggle = findChild<QToolButton *>("dataGraphRelationsToggle")) {
    surface->relationsToggle = toggle;
    connect(toggle, &QToolButton::clicked, surface, [surface] { surface->narrowRelations = !surface->narrowRelations; surface->arrange(); });
  }
  for (auto *card : {details, relations}) {
    auto *shadow = new QGraphicsDropShadowEffect(card); shadow->setBlurRadius(24); shadow->setOffset(0, 4);
    QColor color = t.text; color.setAlpha(22); shadow->setColor(color); card->setGraphicsEffect(shadow);
  }
  PaleoTheme::applyThemedStyleSheet(surface, [this] {
    m_graphLegend->setText(tr("<p><span style='color:%1'>●</span> 井集合　<span style='color:%2'>●</span> 地震测区</p><p><span style='color:%3'>●</span> 井资料　<span style='color:%4'>●</span> 表格 / 成果</p><p><span style='color:%5'>●</span> 空间资料　<span style='color:%6'>●</span> 文档 / 其他</p>")
      .arg(PaleoTheme::graphColor(0).name(), PaleoTheme::graphColor(1).name(), PaleoTheme::graphColor(2).name(),
           PaleoTheme::graphColor(3).name(), PaleoTheme::graphColor(4).name(), PaleoTheme::graphColor(5).name()));
    return PaleoTheme::graphSurfaceStyleSheet();
  });
  surface->arrange();
  return surface;
}

void DataGraphPage::updateGraphPanels() {
  if (!m_graphSurface) return;
  const bool selected = !m_selected.id.isEmpty();
  static_cast<DataGraphSurface *>(m_graphSurface)->detailsWanted = selected;
  m_detailsPanel->setProperty("compactCard", m_selected.kind == Kind::Entity);
  m_relations->setVisible(selected);
  if (selected) m_relations->setFixedHeight(qBound(120, m_relations->header()->sizeHint().height() +
      m_relations->topLevelItemCount() * qMax(20, m_relations->sizeHintForRow(0)) + PaleoTheme::tokens().spacingMd, 220));
  m_graphLegend->setVisible(!selected);
  m_relationHeading->setText(selected ? tr("关联关系") : tr("图谱概览"));
  for (auto *button : {m_open, m_locate, m_folder, m_related, m_trace, m_impact, m_map, m_reference, m_wells, m_manage})
    button->setVisible(button->isEnabled());
  static_cast<DataGraphSurface *>(m_graphSurface)->arrange();
}

void DataGraphPage::configureGraphFilters(QToolButton *filters) {
  const auto &t = PaleoTheme::tokens();
  auto *filterMenu = new QMenu(filters); auto *filterPanel = new QWidget(filterMenu);
  auto *filterLayout = new QVBoxLayout(filterPanel);
  filterLayout->setContentsMargins(t.spacingMd, t.spacingMd, t.spacingMd, t.spacingMd); filterLayout->setSpacing(t.spacingSm);
  for (QWidget *widget : std::initializer_list<QWidget *>{m_category, m_stage, m_evidenceTypeFilter, m_reviewFilter,
          m_relationFilter, m_depth, m_history, m_evidencePage}) { widget->setParent(filterPanel); filterLayout->addWidget(widget); }
  m_depth->setPrefix(tr("邻域层数 ")); m_evidencePage->setPrefix(tr("证据页 "));
  m_history->setText(tr("包含历史版本")); m_history->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  auto *filterAction = new QWidgetAction(filterMenu); filterAction->setDefaultWidget(filterPanel); filterMenu->addAction(filterAction);
  filters->setMenu(filterMenu); filters->setPopupMode(QToolButton::InstantPopup);
}

QString DataGraphPage::nodeDetailsHtml(const Node &node) const {
  if (node.id.isEmpty()) return {};
  const auto &t = PaleoTheme::tokens();
  QString html = QStringLiteral("<h3>%1</h3><p style='color:%2'>%3</p><table width='100%' cellspacing='0' cellpadding='%4'>")
      .arg(node.title.toHtmlEscaped(), t.textMuted.name(), node.detail.toHtmlEscaped()).arg(t.spacingSm);
  const auto row = [&](const QString &label, const QString &value) {
    if (value.isEmpty()) return;
    html += QStringLiteral("<tr><td width='30%' valign='top' style='color:%1;border-bottom:1px solid %2'>%3</td><td style='border-bottom:1px solid %2'>%4</td></tr>")
        .arg(t.textMuted.name(), t.border.name(), label.toHtmlEscaped(), value.toHtmlEscaped());
  };
  QStringList categories; for (const auto &category : node.categories) categories << categoryText(category);
  row(tr("类别"), categories.join(QStringLiteral(" / ")));
  if (node.kind == Kind::Entity) row(tr("文件数"), QString::number(node.count));
  else row(tr("阶段"), stageText(node.stage));
  row(tr("实体 ID"), node.entityId); row(tr("资产 ID"), node.assetId); row(tr("版本 ID"), node.versionId);
  row(tr("文件"), selectedPath()); row(tr("导入来源"), node.sourceUri); row(tr("SHA-256"), node.sha256);
  html += QStringLiteral("</table>");
  if (!node.evidenceText.isEmpty()) html += QStringLiteral("<h4>%1</h4><pre>%2</pre>")
      .arg(tr("生成证据").toHtmlEscaped(), node.evidenceText.toHtmlEscaped());
  return html;
}
