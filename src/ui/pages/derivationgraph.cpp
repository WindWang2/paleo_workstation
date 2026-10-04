// 层：视图
#include "derivationgraph.h"
#include "../paleotheme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFontMetrics>
#include <QGraphicsObject>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>

using namespace paleo::derivation;
namespace
{
// 几何是图符号尺寸（非 UI spacing）；列距/行距留出两行截断标签。
constexpr qreal NodeWidth = 160, NodeHeight = 112, Diameter = 72;
class VersionItem : public QGraphicsObject
{
public:
  VersionItem(const Node &node, std::function<void()> activate)
    : m_node(node), m_activate(std::move(activate))
  {
    setFlag(ItemIsFocusable);
    setCursor(Qt::PointingHandCursor);
    setData(0, node.versionId);
    setData(1, int(node.kind));
    setData(2, node.stale);
    QString tooltip = QObject::tr("版本：%1\n资产：%2\n阶段：%3\n版本标识：%4")
                          .arg(node.versionName, node.assetName, node.stage, node.versionId);
    if (node.stale)
      tooltip += QLatin1Char('\n') + QObject::tr("过时：%1").arg(
          node.staleReason.isEmpty() ? QObject::tr("未提供过时原因") : node.staleReason);
    setToolTip(QStringLiteral("<qt>%1</qt>").arg(tooltip.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br/>"))));
  }
  QRectF boundingRect() const override { return QRectF(0, 0, NodeWidth, NodeHeight); }
  void paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) override
  {
    const auto &t = PaleoTheme::tokens();
    p->setRenderHint(QPainter::Antialiasing);
    p->setPen(QPen(m_node.stale ? t.warning : data(3).toBool() ? t.focusRing : t.border,
                   m_node.stale || data(3).toBool() ? 2 : 1));
    p->setBrush(m_node.stale ? t.warningBg : t.surface);
    const QRectF shape((NodeWidth - Diameter) / 2, 2, Diameter, Diameter);
    if (m_node.kind == Kind::Raw) p->drawRect(shape);
    else if (m_node.kind == Kind::Derived) p->drawEllipse(shape);
    else p->drawPolygon(QPolygonF{shape.topLeft() + QPointF(Diameter / 2, 0),
                                 shape.topRight() + QPointF(0, Diameter / 2),
                                 shape.bottomLeft() + QPointF(Diameter / 2, 0),
                                 shape.topLeft() + QPointF(0, Diameter / 2)});
    p->setFont(PaleoTheme::bodyFont(t.labelPt));
    p->setPen(m_node.stale ? t.warningText : t.text);
    const QString kind = m_node.kind == Kind::Raw ? QObject::tr("原始")
                       : m_node.kind == Kind::Derived ? QObject::tr("衍生") : QObject::tr("外链");
    p->drawText(shape, Qt::AlignCenter, m_node.stale ? kind + QLatin1Char('\n') + QObject::tr("过时") : kind);
    if (hasFocus())
    {
      p->setPen(QPen(t.focusRing, 2));
      p->setBrush(Qt::NoBrush);
      p->drawRect(shape.adjusted(-3, -3, 3, 3));
    }
    p->setPen(t.text);
    const QFontMetrics fm(p->font());
    p->drawText(QRectF(0, 76, NodeWidth, 16), Qt::AlignCenter,
                fm.elidedText(m_node.versionName, Qt::ElideRight, int(NodeWidth)));
    p->setPen(t.textMuted);
    p->drawText(QRectF(0, 94, NodeWidth, 16), Qt::AlignCenter,
                fm.elidedText(m_node.assetName, Qt::ElideRight, int(NodeWidth)));
  }
protected:
  void mousePressEvent(QGraphicsSceneMouseEvent *event) override
  {
    if (event->button() == Qt::LeftButton) { setFocus(); m_activate(); event->accept(); }
    else QGraphicsObject::mousePressEvent(event);
  }
  void keyPressEvent(QKeyEvent *event) override
  {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Space)
    { m_activate(); event->accept(); }
    else QGraphicsObject::keyPressEvent(event);
  }
private:
  Node m_node;
  std::function<void()> m_activate;
};
}

DerivationGraph::DerivationGraph(QWidget *parent) : QGraphicsView(parent)
{
  setObjectName(QStringLiteral("derivationGraph"));
  setAccessibleName(tr("版本衍生血缘图"));
  setAccessibleDescription(tr("箭头从源版本指向下游；点击节点或按回车预览版本"));
  setScene(new QGraphicsScene(this));
  setRenderHint(QPainter::Antialiasing);
  setMinimumHeight(260);
  setMinimumWidth(0);
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  setBackgroundBrush(PaleoTheme::tokens().surface);
}

void DerivationGraph::loadGraph(const Graph &graph)
{
  ++m_generation;
  m_nodes.clear(); m_edges.clear(); scene()->clear();
  QVector<Node> nodes = graph.nodes;
  std::sort(nodes.begin(), nodes.end(), [](const Node &a, const Node &b) {
    return a.column == b.column ? a.versionId < b.versionId : a.column < b.column;
  });
  QHash<int, int> rows;
  for (const Node &n : nodes)
  {
    if (n.versionId.isEmpty() || m_nodes.contains(n.versionId) || m_nodes.size() >= 160) continue;
    auto *item = new VersionItem(n, [this, id = n.versionId, generation = m_generation] {
      // 槽可能重建 scene；等当前 graphics item 的事件栈退出后再发定位。
      QTimer::singleShot(0, this, [this, id, generation] {
        if (generation == m_generation && m_nodes.contains(id)) emit nodeClicked(id);
      });
    });
    item->setPos(n.column * (NodeWidth + PaleoTheme::tokens().spacing2xl), rows[n.column]++ * (NodeHeight + PaleoTheme::tokens().spacingLg));
    item->setZValue(1);
    scene()->addItem(item); m_nodes.insert(n.versionId, item);
  }
  QSet<QPair<QString, QString>> drawn;
  for (const Edge &e : graph.edges)
  {
    if (!m_nodes.contains(e.parentId) || !m_nodes.contains(e.childId) || drawn.contains(qMakePair(e.parentId, e.childId))) continue;
    drawn.insert(qMakePair(e.parentId, e.childId));
    // parent → child：箭头终点始终位于 child 的符号边界。环/自环用弯线，不做拓扑排序递归。
    const QPointF parent = m_nodes[e.parentId]->pos() + QPointF(NodeWidth / 2, 38);
    const QPointF child = m_nodes[e.childId]->pos() + QPointF(NodeWidth / 2, 38);
    QPainterPath path;
    QPointF end, tangent;
    if (e.parentId == e.childId)
    {
      path.moveTo(parent + QPointF(0, -Diameter / 2));
      end = parent + QPointF(Diameter / 2, 0);
      const QPointF control = parent + QPointF(Diameter, -Diameter);
      path.cubicTo(parent + QPointF(0, -Diameter), control, end);
      tangent = end - control;
    }
    else if (child.x() <= parent.x())
    {
      // 回边走符号上方，同列边走右侧；不能穿过中间节点伪装成链。
      const bool feedback = child.x() < parent.x();
      const QPointF port = feedback ? QPointF(0, -Diameter / 2) : QPointF(Diameter / 2, 0);
      const QPointF offset = feedback ? QPointF(0, -Diameter) : QPointF(Diameter, 0);
      const QPointF start = parent + port;
      end = child + port;
      path.moveTo(start); path.cubicTo(start + offset, end + offset, end);
      tangent = -offset;
    }
    else
    {
      QPointF delta = child - parent;
      const qreal length = std::hypot(delta.x(), delta.y());
      const QPointF unit = delta / length;
      // 正方形/菱形/圆的射线交点分别计算，箭头不落到文字或节点内部。
      const auto radius = [&nodes](const QString &id, const QPointF &u) {
        const auto it = std::find_if(nodes.cbegin(), nodes.cend(), [&id](const Node &n) { return n.versionId == id; });
        if (it->kind == Kind::Raw) return Diameter / (2 * qMax(std::abs(u.x()), std::abs(u.y())));
        if (it->kind == Kind::External) return Diameter / (2 * (std::abs(u.x()) + std::abs(u.y())));
        return Diameter / 2;
      };
      const QPointF start = parent + unit * radius(e.parentId, unit);
      end = child - unit * radius(e.childId, unit);
      path.moveTo(start);
      const QPointF c1 = start + QPointF(delta.x() / 2, 0);
      const QPointF c2 = end - QPointF(delta.x() / 2, 0);
      path.cubicTo(c1, c2, end); tangent = end - c2;
      if (std::hypot(tangent.x(), tangent.y()) < 0.01) tangent = unit;
    }
    const qreal angle = std::atan2(tangent.y(), tangent.x());
    const qreal arrow = PaleoTheme::tokens().spacingSm;
    path.moveTo(end - QPointF(std::cos(angle - 0.5), std::sin(angle - 0.5)) * arrow);
    path.lineTo(end);
    path.lineTo(end - QPointF(std::cos(angle + 0.5), std::sin(angle + 0.5)) * arrow);
    auto *item = scene()->addPath(path, QPen(PaleoTheme::tokens().textMuted, 1));
    item->setData(0, e.parentId); item->setData(1, e.childId); item->setZValue(0);
    m_edges << PaintedEdge{e.parentId, e.childId, item};
  }
  const qreal margin = PaleoTheme::tokens().spacingMd;
  scene()->setSceneRect(scene()->itemsBoundingRect().adjusted(-margin, -margin, margin, margin));
  // 重建保留用户缩放；只在明确点击「适配视图」时 fitInView（DESIGN.md）。
  viewport()->update();
}

void DerivationGraph::highlight(const QString &selected, const QSet<QString> &closure)
{
  for (auto it = m_nodes.cbegin(); it != m_nodes.cend(); ++it)
  {
    it.value()->setData(3, it.key() == selected);
    it.value()->setOpacity(closure.isEmpty() || closure.contains(it.key()) ? 1.0 : 0.25);
    it.value()->update();
  }
  for (const PaintedEdge &e : m_edges)
    e.item->setOpacity(closure.isEmpty() || (closure.contains(e.parent) && closure.contains(e.child)) ? 1.0 : 0.25);
}
void DerivationGraph::fitGraph()
{
  if (!m_nodes.isEmpty()) fitInView(scene()->sceneRect(), Qt::KeepAspectRatio);
}
void DerivationGraph::changeEvent(QEvent *event)
{
  QGraphicsView::changeEvent(event);
  if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
  {
    setBackgroundBrush(PaleoTheme::tokens().surface);
    for (const PaintedEdge &e : m_edges) e.item->setPen(QPen(PaleoTheme::tokens().textMuted, 1));
    scene()->update();
  }
}

DerivationPanel::DerivationPanel(QWidget *parent) : QWidget(parent)
{
  setObjectName(QStringLiteral("derivationPanel"));
  setMinimumWidth(0);
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(PaleoTheme::tokens().spacingSm);
  m_controls = new QWidget(this);
  m_controls->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
  auto *grid = new QGridLayout(m_controls);
  grid->setContentsMargins(0, 0, 0, 0);
  grid->setSpacing(PaleoTheme::tokens().spacingXs);
  const auto slider = [this, grid](const QString &name, const QString &title, int row) {
    auto *label = new QLabel(title, m_controls);
    auto *s = new QSlider(Qt::Horizontal, m_controls);
    s->setObjectName(name); s->setRange(0, 12); s->setValue(2); s->setAccessibleName(title);
    label->setBuddy(s);
    auto *value = new QLabel(QString::number(s->value()), m_controls);
    grid->addWidget(label, row, 0); grid->addWidget(s, row, 1); grid->addWidget(value, row, 2);
    connect(s, &QSlider::valueChanged, this, [this, value](int depth) { value->setText(QString::number(depth)); emit queryChanged(); });
    return s;
  };
  m_up = slider(QStringLiteral("derivationUpstreamDepth"), tr("上游层数"), 0);
  m_down = slider(QStringLiteral("derivationDownstreamDepth"), tr("下游层数"), 1);
  m_entity = new QComboBox(m_controls); m_entity->setObjectName(QStringLiteral("derivationEntityFilter"));
  m_entity->setAccessibleName(tr("按实体过滤"));
  m_entity->setMinimumWidth(0); m_entity->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  m_entity->addItem(tr("全部实体"), QString());
  m_kind = new QComboBox(m_controls); m_kind->setObjectName(QStringLiteral("derivationKindFilter"));
  m_kind->setAccessibleName(tr("按版本类别过滤"));
  m_kind->addItem(tr("全部类别"), -1); m_kind->addItem(tr("原始（方形）"), int(Kind::Raw));
  m_kind->addItem(tr("衍生（圆形）"), int(Kind::Derived)); m_kind->addItem(tr("外链（菱形）"), int(Kind::External));
  grid->addWidget(m_entity, 2, 0, 1, 3); grid->addWidget(m_kind, 3, 0, 1, 3);
  m_asset = new QLineEdit(m_controls); m_asset->setObjectName(QStringLiteral("derivationAssetFilter"));
  m_asset->setPlaceholderText(tr("过滤资产名称或标识")); m_asset->setAccessibleName(tr("按资产过滤"));
  grid->addWidget(m_asset, 4, 0, 1, 3);
  m_stale = new QCheckBox(tr("仅过期链路（含源版本）"), m_controls);
  m_stale->setObjectName(QStringLiteral("derivationStaleOnly")); grid->addWidget(m_stale, 5, 0, 1, 3);
  auto *fit = new QPushButton(tr("适配视图"), m_controls);
  grid->addWidget(fit, 6, 0, 1, 3);
  connect(m_entity, &QComboBox::currentIndexChanged, this, &DerivationPanel::queryChanged);
  connect(m_kind, &QComboBox::currentIndexChanged, this, &DerivationPanel::queryChanged);
  connect(m_asset, &QLineEdit::textChanged, this, &DerivationPanel::queryChanged);
  connect(m_stale, &QCheckBox::toggled, this, &DerivationPanel::queryChanged);
  layout->addWidget(m_controls);
  m_summary = new QLabel(this); m_summary->setObjectName(QStringLiteral("derivationSummary"));
  m_summary->setWordWrap(true); m_summary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Maximum);
  m_summary->setTextFormat(Qt::PlainText);
  PaleoTheme::applyThemedStyleSheet(m_summary, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  layout->addWidget(m_summary);
  m_graph = new DerivationGraph(this); layout->addWidget(m_graph, 1);
  connect(m_graph, &DerivationGraph::nodeClicked, this, &DerivationPanel::nodeClicked);
  connect(fit, &QPushButton::clicked, m_graph, &DerivationGraph::fitGraph);
  setGraph(Graph{});
}
Query DerivationPanel::query() const
{
  Query q;
  q.upstreamDepth = m_up->value(); q.downstreamDepth = m_down->value();
  q.entityFilter = m_entity->currentData().toString(); q.assetFilter = m_asset->text().trimmed();
  q.kindFilter = m_kind->currentData().toInt(); q.staleOnly = m_stale->isChecked();
  return q;
}
void DerivationPanel::setGraph(const Graph &graph)
{
  const QSignalBlocker block(m_entity);
  const QString selected = m_entity->currentData().toString();
  m_entity->clear(); m_entity->addItem(tr("全部实体"), QString());
  for (const Choice &e : graph.entities) m_entity->addItem(e.name, e.id);
  // 跨上下文仍保留显式过滤（无匹配时诚实空态），不静默扩大查询。
  if (!selected.isEmpty() && m_entity->findData(selected) < 0) m_entity->addItem(selected, selected);
  m_entity->setCurrentIndex(qMax(0, m_entity->findData(selected)));
  m_controls->setEnabled(graph.available); m_graph->setEnabled(graph.available);
  m_controls->setToolTip(graph.available ? QString() : tr("工程未打开，衍生血缘不可用"));
  m_graph->setToolTip(m_controls->toolTip());
  QStringList notes;
  notes << tr("源 → 下游 · 方形：原始 · 圆形：衍生 · 菱形：外链");
  if (!graph.message.isEmpty()) notes << graph.message;
  notes << tr("显示 %1 个版本 · 已折叠 %2 个上游 / %3 个下游节点")
               .arg(graph.nodes.size()).arg(graph.collapsedUpstream).arg(graph.collapsedDownstream);
  if (graph.collapsedSeeds) notes << tr("已折叠 %1 个所选实体版本").arg(graph.collapsedSeeds);
  if (graph.filtered) notes << tr("过滤移除 %1 个版本").arg(graph.filtered);
  if (graph.missingParents) notes << tr("缺失源版本 %1 个（未补造节点）").arg(graph.missingParents);
  m_summary->setText(notes.join(QLatin1Char('\n')));
  m_graph->loadGraph(graph);
  m_graph->setVisible(!graph.nodes.isEmpty()); // 空态用说明占位，不留空白画布。
}
