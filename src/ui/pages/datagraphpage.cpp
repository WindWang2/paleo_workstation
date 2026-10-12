// 层：视图
#include "datagraphpage.h"
#include "../../workflow/datagraphworkflow.h"
#include "../../services/previewdoc.h"
#include "../../catalog/datacatalog.h"
#include "../paleotheme.h"
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QGraphicsObject>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsView>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QStyleOptionGraphicsItem>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <cmath>

using namespace paleo::datagraph;
namespace {
class NodeItem final : public QGraphicsObject {
public:
  Node node;
  std::function<void(const Node &, bool)> selected;
  std::function<void(const Node &)> activated;
  explicit NodeItem(const Node &n) : node(n) {
    setFlags(ItemIsSelectable | ItemIsFocusable); setCursor(Qt::PointingHandCursor);
    setZValue(1); updateNode(n);
  }
  QRectF boundingRect() const override { return QRectF(0, 0, 260, 88); }
  void updateNode(const Node &n) {
    node = n; setPos(n.position);
    setToolTip((n.title + QLatin1Char('\n') + n.detail + QLatin1Char('\n') + n.path).toHtmlEscaped()); update();
  }
  void paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) override {
    const auto &t = PaleoTheme::tokens();
    p->setPen(QPen(isSelected() || hasFocus() ? t.focusRing : node.stale || node.unresolved || node.cycle ? t.warning : t.border, isSelected() || hasFocus() ? 2 : 1));
    p->setBrush(node.kind == Kind::Group ? t.surfaceAlt : t.surface);
    p->drawRoundedRect(boundingRect().adjusted(1, 1, -1, -1), t.radiusSm, t.radiusSm);
    QFont font = p->font(); font.setPointSize(t.bodyPt); font.setBold(node.kind == Kind::Group); p->setFont(font); p->setPen(t.text);
    const qreal lod = QStyleOptionGraphicsItem::levelOfDetailFromTransform(p->worldTransform());
    if (lod < .35) return;
    p->drawText(QRectF(t.spacingSm, t.spacingSm, 244, 26), Qt::AlignVCenter,
                QFontMetrics(font).elidedText(node.title, Qt::ElideMiddle, 244));
    if (lod < .65) return;
    font.setBold(false); font.setPointSize(t.labelPt); p->setFont(font); p->setPen(t.textMuted);
    QString kind = node.kind == Kind::Entity ? QCoreApplication::translate("DataGraphPage", "实体") :
                   node.kind == Kind::Asset ? QCoreApplication::translate("DataGraphPage", "文件资料") :
                   node.kind == Kind::Version ? QCoreApplication::translate("DataGraphPage", "资料版本") :
                   node.kind == Kind::Activity ? QCoreApplication::translate("DataGraphPage", "生成记录") :
                   node.kind == Kind::Missing ? QCoreApplication::translate("DataGraphPage", "待核实") : QCoreApplication::translate("DataGraphPage", "分组");
    p->drawText(QRectF(t.spacingSm, 36, 244, 19), QFontMetrics(font).elidedText(kind + QStringLiteral(" · ") + node.detail, Qt::ElideRight, 244));
    QString state = node.kind == Kind::Asset ? stageText(node.stage) : node.path;
    if (node.stale) state += QCoreApplication::translate("DataGraphPage", " · 已过期");
    if (node.unresolved) state += QCoreApplication::translate("DataGraphPage", " · 待关联");
    p->drawText(QRectF(t.spacingSm, 58, 244, 19), QFontMetrics(font).elidedText(state, Qt::ElideMiddle, 244));
  }
  void mousePressEvent(QGraphicsSceneMouseEvent *e) override {
    QGraphicsObject::mousePressEvent(e); dispatch(false);
  }
  void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *e) override { e->accept(); dispatch(true); }
  void keyPressEvent(QKeyEvent *e) override {
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) { dispatch(true); e->accept(); }
    else if (e->key() == Qt::Key_Space) { dispatch(false); e->accept(); }
    else QGraphicsObject::keyPressEvent(e);
  }
private:
  void dispatch(bool open) {
    // Selection can refresh other panels and eventually rebuild the scene; leave item event stack first.
    const Node value = node; const auto pick = selected; const auto activate = activated;
    QTimer::singleShot(0, this, [value, pick, activate, open] { if (open) { if (activate) activate(value); } else if (pick) pick(value, true); });
  }
};
}
DataGraphPage::DataGraphPage(QWidget *parent) : QWidget(parent), m_workflow(new DataGraphWorkflow(this)) {
  setObjectName(QStringLiteral("dataGraphPage")); setAccessibleName(tr("资料关联与成果血缘图谱"));
  const auto &t = PaleoTheme::tokens(); auto *layout = new QVBoxLayout(this); layout->setContentsMargins(t.spacingSm, t.spacingSm, t.spacingSm, t.spacingSm);
  auto *controls = new QWidget(this); auto *controlLayout = new QVBoxLayout(controls); controlLayout->setContentsMargins(0, 0, 0, 0);
  auto *filters = new QHBoxLayout;
  m_mode = new QComboBox(this); m_mode->setObjectName("dataGraphMode");
  m_mode->addItem(tr("工区概览"), int(Mode::Overview)); m_mode->addItem(tr("资料关联"), int(Mode::Relations)); m_mode->addItem(tr("成果追溯"), int(Mode::Lineage));
  m_category = new QComboBox(this); m_category->setObjectName("dataGraphCategory"); m_category->addItem(tr("全部类别"), QString());
  for (const QString &c : {QStringLiteral("well"), QStringLiteral("seismic"), QStringLiteral("framework"), QStringLiteral("product"), QStringLiteral("misc")}) m_category->addItem(categoryText(c), c);
  m_stage = new QComboBox(this); m_stage->setObjectName("dataGraphStage"); m_stage->addItem(tr("全部阶段"), QString());
  for (const QString &s : {QStringLiteral("RAW"), QStringLiteral("INTERMEDIATE"), QStringLiteral("DERIVED"), QStringLiteral("OUTPUT"), QStringLiteral("NONE")}) m_stage->addItem(stageText(s), s);
  m_search = new QLineEdit(this); m_search->setObjectName("dataGraphSearch"); m_search->setPlaceholderText(tr("搜索文件、路径或资料名称")); m_search->setClearButtonEnabled(true);
  filters->addWidget(m_mode); filters->addWidget(m_category); filters->addWidget(m_stage); filters->addWidget(m_search, 1); controlLayout->addLayout(filters);
  auto *tools = new QHBoxLayout;
  m_depth = new QSpinBox(this); m_depth->setRange(1, 6); m_depth->setValue(2); m_depth->setPrefix(tr("邻域层数 ")); m_depth->setAccessibleName(tr("邻域层数"));
  m_history = new QCheckBox(tr("包含历史版本"), this);
  auto *refreshButton = new QPushButton(tr("刷新图谱"), this), *fit = new QPushButton(tr("适配视图"), this);
  auto *zoomIn = new QPushButton(tr("放大"), this), *zoomOut = new QPushButton(tr("缩小"), this);
  m_cancel = new QPushButton(tr("取消计算"), this); m_cancel->setEnabled(false);
  for (QWidget *widget : std::initializer_list<QWidget *>{m_depth, m_history, refreshButton, fit, zoomIn, zoomOut, m_cancel}) tools->addWidget(widget);
  tools->addStretch(); controlLayout->addLayout(tools);
  auto *controlScroll = new QScrollArea(this); controlScroll->setWidget(controls); controlScroll->setWidgetResizable(true);
  controlScroll->setFrameShape(QFrame::NoFrame); controlScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  controlScroll->setFixedHeight(controls->sizeHint().height() + style()->pixelMetric(QStyle::PM_ScrollBarExtent));
  layout->addWidget(controlScroll);
  m_summary = new QLabel(this); m_summary->setWordWrap(true); layout->addWidget(m_summary);
  auto *splitter = new QSplitter(this); splitter->setChildrenCollapsible(false);
  m_scene = new QGraphicsScene(this); m_view = new QGraphicsView(m_scene, splitter); m_view->setObjectName("dataGraphView");
  m_view->setRenderHint(QPainter::Antialiasing); m_view->setDragMode(QGraphicsView::ScrollHandDrag); m_view->setAccessibleName(tr("资料图谱画布"));
  auto *details = new QWidget(splitter); auto *dl = new QVBoxLayout(details); dl->setContentsMargins(t.spacingSm, 0, 0, 0);
  m_details = new QPlainTextEdit(details); m_details->setReadOnly(true); m_details->setPlaceholderText(tr("选择节点查看资料、版本、文件路径与关联。双击分组展开，双击文件预览。")); dl->addWidget(m_details, 1);
  m_open = new QPushButton(tr("预览文件 / 版本"), details); m_locate = new QPushButton(tr("在数据管理中定位"), details);
  m_folder = new QPushButton(tr("打开文件所在目录"), details); m_trace = new QPushButton(tr("追溯这个成果"), details);
  for (auto *button : {m_open, m_locate, m_folder, m_trace}) { dl->addWidget(button); button->setEnabled(false); }
  splitter->addWidget(m_view); splitter->addWidget(details); splitter->setStretchFactor(0, 3); splitter->setStretchFactor(1, 1); layout->addWidget(splitter, 1);
  m_status = new QLabel(this); m_status->setWordWrap(true); layout->addWidget(m_status);
  connect(m_workflow, &DataGraphWorkflow::ready, this, &DataGraphPage::render);
  connect(m_workflow, &DataGraphWorkflow::statusChanged, this, [this](const QString &s, bool busy) {
    m_status->setText(!busy && !m_graphMessage.isEmpty() ? s + QStringLiteral(" · ") + m_graphMessage : s); m_busy = busy; m_cancel->setEnabled(busy); m_view->setEnabled(!busy);
    if (busy) for (auto *b : {m_open, m_locate, m_folder, m_trace}) b->setEnabled(false);
    else selectNode(m_selected, false);
  });
  for (auto *combo : {m_mode, m_category, m_stage}) connect(combo, &QComboBox::currentIndexChanged, this, [this] { request(); });
  connect(m_search, &QLineEdit::textChanged, this, [this] { request(); }); connect(m_depth, &QSpinBox::valueChanged, this, [this] { request(); });
  connect(m_history, &QCheckBox::toggled, this, [this] { request(); }); connect(refreshButton, &QPushButton::clicked, this, &DataGraphPage::refresh);
  connect(fit, &QPushButton::clicked, this, [this] { m_view->fitInView(m_scene->itemsBoundingRect().adjusted(-16, -16, 16, 16), Qt::KeepAspectRatio); });
  connect(zoomIn, &QPushButton::clicked, this, [this] { if (m_view->transform().m11() < 4) m_view->scale(1.2, 1.2); });
  connect(zoomOut, &QPushButton::clicked, this, [this] { if (m_view->transform().m11() > .15) m_view->scale(1 / 1.2, 1 / 1.2); });
  connect(m_cancel, &QPushButton::clicked, m_workflow, &DataGraphWorkflow::cancel);
  connect(m_open, &QPushButton::clicked, this, [this] { activateNode(m_selected); });
  connect(m_locate, &QPushButton::clicked, this, [this] { selectNode(m_selected, true); });
  connect(m_trace, &QPushButton::clicked, this, [this] {
    if (m_selected.versionId.isEmpty() && m_selected.assetId.isEmpty()) return;
    m_focus = m_selected.versionId.isEmpty() ? assetKey(m_selected.assetId) : versionKey(m_selected.versionId); m_mode->setCurrentIndex(2); request();
  });
  connect(m_folder, &QPushButton::clicked, this, [this] {
    const QString path = selectedPath(); if (!path.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
  });
  PaleoTheme::applyThemedStyleSheet(this, [this] {
    const auto &tokens = PaleoTheme::tokens(); m_scene->setBackgroundBrush(tokens.surface); m_view->viewport()->update();
    for (auto *edge : m_edges) { auto *path = static_cast<QGraphicsPathItem *>(edge); auto pen = path->pen(); pen.setColor(path->data(0).toBool() ? tokens.warning : tokens.textMuted); path->setPen(pen); }
    return QString();
  });
}
void DataGraphPage::setDocService(PreviewDocService *doc) { m_doc = doc; m_workflow->setContext(doc ? doc->catalog() : nullptr, doc ? doc->taskService() : nullptr); refresh(); }
void DataGraphPage::setPresentation(const Presentation &presentation) { m_presentation = presentation; m_workflow->setPresentation(presentation); refresh(); }
void DataGraphPage::setSelection(const QString &entity, const QString &asset, const QString &version) {
  if (m_navigating) return;
  const QString key = !version.isEmpty() ? versionKey(version) : !asset.isEmpty() ? assetKey(asset) : !entity.isEmpty() ? entityKey(entity) : QString();
  if (m_focus == key) return;
  m_focus = key; if (m_mode->currentIndex() != 0) request();
}
void DataGraphPage::openRelations(const QString &asset, const QString &version) {
  m_focus = !version.isEmpty() ? versionKey(version) : !asset.isEmpty() ? assetKey(asset) : m_focus;
  QSignalBlocker a(m_category), b(m_stage), c(m_search), d(m_mode);
  m_category->setCurrentIndex(0); m_stage->setCurrentIndex(0); m_search->clear(); m_mode->setCurrentIndex(m_focus.isEmpty() ? 0 : 1); request();
}
void DataGraphPage::refresh() {
  const QString path = m_doc && m_doc->catalog() ? m_doc->catalog()->catalogPath() : QString();
  if (m_projectPath != path) { m_projectPath = path; m_focus.clear(); render({}); }
  m_workflow->setContext(m_doc ? m_doc->catalog() : nullptr, m_doc ? m_doc->taskService() : nullptr); request();
}
void DataGraphPage::request() {
  m_history->setEnabled(m_mode->currentIndex() == 1);
  m_history->setToolTip(tr("资料关联视图可展开历史版本；成果追溯按实际版本血缘显示，工区概览按当前版本统计。"));
  if (!isVisible()) return;
  Query q; q.mode = Mode(m_mode->currentData().toInt()); q.focus = m_focus; q.category = m_category->currentData().toString();
  q.stage = m_stage->currentData().toString(); q.search = m_search->text().trimmed(); q.depth = m_depth->value(); q.history = m_history->isChecked(); m_workflow->request(q);
}
void DataGraphPage::showEvent(QShowEvent *event) { QWidget::showEvent(event); refresh(); }
void DataGraphPage::hideEvent(QHideEvent *event) { m_workflow->cancel(); QWidget::hideEvent(event); }
void DataGraphPage::render(const Graph &graph) {
  m_graphMessage = graph.message;
  QSet<QString> keep; for (const Node &n : graph.nodes) keep.insert(n.id);
  for (auto it = m_items.begin(); it != m_items.end();) { if (!keep.contains(it.key())) { delete it.value(); it = m_items.erase(it); } else ++it; }
  for (const Node &n : graph.nodes) {
    auto *item = static_cast<NodeItem *>(m_items.value(n.id));
    if (!item) { item = new NodeItem(n); item->selected = [this](const Node &node, bool navigate) { selectNode(node, navigate); }; item->activated = [this](const Node &node) { activateNode(node); }; m_scene->addItem(item); m_items.insert(n.id, item); }
    else item->updateNode(n);
  }
  for (auto *e : m_edges) delete e; m_edges.clear();
  for (const Edge &e : graph.edges) {
    if (!m_items.contains(e.source) || !m_items.contains(e.target)) continue;
    const QPointF start = m_items[e.source]->pos() + QPointF(260, 44), end = m_items[e.target]->pos() + QPointF(0, 44);
    QPainterPath path; path.moveTo(start);
    if (end.x() <= start.x()) path.cubicTo(start + QPointF(30, -70), end + QPointF(-30, -70), end);
    else path.cubicTo(start + QPointF(30, 0), end - QPointF(30, 0), end);
    const double angle = end.x() > start.x() ? 0 : std::atan2(70.0, 30.0);
    path.moveTo(end - QPointF(std::cos(angle - .5), std::sin(angle - .5)) * 8); path.lineTo(end); path.lineTo(end - QPointF(std::cos(angle + .5), std::sin(angle + .5)) * 8);
    QPen pen(e.unresolved ? PaleoTheme::tokens().warning : PaleoTheme::tokens().textMuted, 1); if (e.relation == Relation::EditBaseline || e.relation == Relation::Rollback || e.unresolved) pen.setStyle(Qt::DashLine);
    auto *item = m_scene->addPath(path, pen); item->setToolTip(e.label.toHtmlEscaped()); item->setData(0, e.unresolved); item->setZValue(0); m_edges.append(item);
  }
  m_scene->setSceneRect(m_scene->itemsBoundingRect().adjusted(-16, -16, 16, 16));
  bool found = false; for (const Node &n : graph.nodes) if (n.id == m_selected.id) { m_selected = n; found = true; break; }
  if (!found) m_selected = {};
  selectNode(m_selected, false);
  m_summary->setText(tr("工区：%1 个实体 · %2 份资料 · %3 个版本；当前 %4 个节点 / %5 条关系；待关联 %6 项 · 缺失或回收来源 %7 项%8%9%10")
    .arg(graph.totalEntities).arg(graph.totalAssets).arg(graph.totalVersions).arg(graph.nodes.size()).arg(graph.edges.size()).arg(graph.unresolved).arg(graph.missingParents)
    .arg(graph.hidden ? tr(" · 还有 %1 项未展开，请缩小筛选范围").arg(graph.hidden) : QString())
    .arg(graph.hiddenEdges ? tr(" · 还有 %1 条关系未绘制").arg(graph.hiddenEdges) : QString())
    .arg(graph.cycles ? tr(" · %1 组血缘环需核查").arg(graph.cycles) : QString()));
  if (!graph.message.isEmpty()) m_status->setText(graph.message);
}
QString DataGraphPage::selectedPath() const {
  if (!m_doc || !m_doc->catalog() || m_selected.assetId.isEmpty() || m_presentation.removedIds.contains(m_selected.assetId)) return {};
  const auto v = m_selected.versionId.isEmpty() ? m_doc->catalog()->currentVersion(m_selected.assetId) : m_doc->catalog()->versionById(m_selected.versionId);
  return v.id.isEmpty() ? QString() : m_doc->absolutePathForVersion(v);
}
void DataGraphPage::selectNode(const Node &node, bool navigate) {
  m_selected = node;
  m_details->setPlainText(node.id.isEmpty() ? QString() : node.title + QStringLiteral("\n\n") + node.detail + QStringLiteral("\n\n") + tr("类别：%1\n阶段：%2\n文件：%3\n资产 ID：%4\n版本 ID：%5")
    .arg([&] { QStringList names; for (const auto &c : node.categories) names << categoryText(c); return names.join(QStringLiteral(" / ")); }(),
         node.kind == Kind::Entity ? tr("不适用") : node.kind == Kind::Group && node.stage.isEmpty() ? tr("全部阶段") : stageText(node.stage),
         selectedPath(), node.assetId, node.versionId)
    + tr("\n导入来源：%1\nSHA-256：%2").arg(node.sourceUri, node.sha256)
    + (node.evidenceText.isEmpty() ? QString() : tr("\n\n生成证据（输入、模型、参数、支撑与软件）：\n") + node.evidenceText));
  const bool real = !m_busy && node.kind != Kind::Missing && node.kind != Kind::Group &&
                    !m_presentation.removedIds.contains(node.assetId) && !m_presentation.removedIds.contains(node.entityId);
  m_open->setEnabled(real && !node.assetId.isEmpty()); m_locate->setEnabled(real && (!node.assetId.isEmpty() || !node.entityId.isEmpty()));
  m_folder->setEnabled(real && !selectedPath().isEmpty()); m_trace->setEnabled(real && !node.assetId.isEmpty());
  if (!navigate || !real) return;
  QScopedValueRollback<bool> guard(m_navigating, true);
  if (!node.versionId.isEmpty()) emit versionFocusRequested(node.assetId, node.versionId);
  else if (!node.assetId.isEmpty()) emit assetFocusRequested(node.assetId);
  else if (!node.entityId.isEmpty()) { emit entityFocusRequested(node.entityId); if (node.detail == QLatin1String("well")) emit wellSelected(node.entityId); }
}
void DataGraphPage::activateNode(const Node &node) {
  if (m_busy || node.id.isEmpty() || node.kind == Kind::Missing ||
      m_presentation.removedIds.contains(node.assetId) || m_presentation.removedIds.contains(node.entityId)) return;
  if (node.kind == Kind::Group) {
    QSignalBlocker a(m_category), b(m_stage), c(m_mode); m_mode->setCurrentIndex(0); m_category->setCurrentIndex(m_category->findData(node.categories.value(0)));
    m_stage->setCurrentIndex(node.stage.isEmpty() ? 0 : m_stage->findData(node.stage)); request(); return;
  }
  selectNode(node, true);
  if (!node.versionId.isEmpty()) emit versionActivated(node.versionId);
  else if (!node.assetId.isEmpty()) emit assetActivated(node.assetId);
}
