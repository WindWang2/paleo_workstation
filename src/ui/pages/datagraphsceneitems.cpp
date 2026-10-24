// 层：视图
#include "datagraphsceneitems.h"
#include "datagraphstyle.h"
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsView>
#include <cmath>
QRectF DataGraphNodeItem::connectionRect() const {
  if (!compact) return boundingRect();
  const qreal radius = node.kind == Kind::Entity ? qBound(25.0, 24.0 + std::sqrt(qMax(0, node.count)) * 2, 37.0) : 17.0;
  return QRectF(-radius, -radius, radius * 2, radius * 2);
}
QColor DataGraphNodeItem::networkColor() const {
  if (node.kind == Kind::Entity) return PaleoTheme::graphColor(node.entityType == "well" ? 0 : node.entityType == "seismic_survey" ? 1 : 5);
  const auto title = node.title.toLower();
  if (title.endsWith(".pdf") || title.endsWith(".docx")) return PaleoTheme::graphColor(5);
  if (title.endsWith(".xlsx") || title.endsWith(".csv") || title.contains("tops")) return PaleoTheme::graphColor(3);
  if (node.categories.contains("seismic") || title.endsWith(".sgy")) return PaleoTheme::graphColor(1);
  if (node.categories.contains("framework")) return PaleoTheme::graphColor(4);
  if (node.categories.contains("product")) return PaleoTheme::graphColor(3);
  return PaleoTheme::graphColor(node.categories.contains("well") || title.endsWith(".las") || title.endsWith(".dlis") ? 2 : 5);
}
void DataGraphNodeItem::paintNetworkNode(QPainter *p) const {
  const auto &t = PaleoTheme::tokens(); const auto circle = connectionRect();
  const bool active = isSelected() || hasFocus(); const auto color = networkColor();
  p->setRenderHint(QPainter::Antialiasing);
  if (active || isUnderMouse()) {
    QColor halo = color; halo.setAlpha(active ? 38 : 22);
    p->setPen(Qt::NoPen); p->setBrush(halo); p->drawEllipse(circle.adjusted(-9, -9, 9, 9));
  }
  p->setBrush(color); p->setPen(QPen(active ? t.text : t.surface, active ? 2.5 : 2)); p->drawEllipse(circle);
  QFont font = p->font(); font.setPointSize(t.bodyPt); font.setBold(true); p->setFont(font); p->setPen(PaleoTheme::graphInkColor());
  if (node.kind == Kind::Entity) p->drawText(circle, Qt::AlignCenter, QString::number(node.count));
  else {
    p->setBrush(Qt::NoBrush); p->setPen(QPen(PaleoTheme::graphInkColor(), 1.4));
    const QRectF glyph(-5, -7, 10, 14);
    p->drawRoundedRect(glyph, t.radiusSm / 2, t.radiusSm / 2);
    p->drawLine(QPointF(-2, -2), QPointF(3, -2)); p->drawLine(QPointF(-2, 2), QPointF(3, 2));
  }
  const qreal lod = QStyleOptionGraphicsItem::levelOfDetailFromTransform(p->worldTransform());
  if (lod < .38 && !active) return;
  font.setPointSize(t.bodyPt + 1); font.setBold(active); p->setFont(font); p->setPen(t.text);
  p->drawText(QRectF(-78, circle.bottom() + 7, 156, 24), Qt::AlignCenter,
              QFontMetrics(font).elidedText(node.title, Qt::ElideRight, 152));
  if (active || node.stale || node.unresolved || node.cycle) {
    font.setPointSize(t.labelPt); font.setBold(false); p->setFont(font); p->setPen(t.textMuted);
    const QString state = node.unresolved ? QCoreApplication::translate("DataGraphPage", "待关联")
        : node.stale ? QCoreApplication::translate("DataGraphPage", "已过期")
        : node.cycle ? QCoreApplication::translate("DataGraphPage", "血缘环需核查")
        : node.kind == Kind::Entity ? QCoreApplication::translate("DataGraphPage", "%1 份文件").arg(node.count) : stageText(node.stage);
    p->drawText(QRectF(-78, circle.bottom() + 29, 156, 20), Qt::AlignCenter, state);
  }
}

QGraphicsPathItem *addDataGraphEdge(QGraphicsScene *scene, QGraphicsItem *source,
    QGraphicsItem *target, const paleo::datagraph::Edge &edge, double bend) {
  const auto rectFor = [](QGraphicsItem *item) {
    if (const auto *node = dynamic_cast<DataGraphNodeItem *>(item)) {
      const auto rect = item->mapRectToScene(node->connectionRect());
      if (!node->compact || !item->scene() || item->scene()->views().isEmpty()) return rect;
      const qreal scale = item->scene()->views().first()->transform().m11();
      const auto size = rect.size() / qMax(.01, scale);
      return QRectF(item->scenePos() - QPointF(size.width() / 2, size.height() / 2), size);
    }
    return item->sceneBoundingRect();
  };
  const auto a = rectFor(source), b = rectFor(target);
  const auto *sourceNode = dynamic_cast<DataGraphNodeItem *>(source);
  const bool network = sourceNode && sourceNode->compact;
  const auto *targetNode = dynamic_cast<DataGraphNodeItem *>(target);
  const qreal viewScale = network && !scene->views().isEmpty() ? qMax(.01, scene->views().first()->transform().m11()) : 1.0;
  const auto direction = b.center() - a.center();
  const double length = std::hypot(direction.x(), direction.y());
  const auto boundary = [&direction](QGraphicsItem *item, const QRectF &rect, bool reverse) {
    const double dx = std::abs(direction.x()), dy = std::abs(direction.y());
    const auto *node = dynamic_cast<DataGraphNodeItem *>(item);
    const double scale = node && (node->compact || node->node.kind == Kind::Entity)
        ? 1.0 / std::sqrt(4 * dx * dx / (rect.width() * rect.width()) + 4 * dy * dy / (rect.height() * rect.height()) + 1e-12)
        : qMin(dx > 0 ? rect.width() / (2 * dx) : 1e9,
                              dy > 0 ? rect.height() / (2 * dy) : 1e9);
    return rect.center() + direction * (reverse ? -scale : scale);
  };
  const auto start = boundary(source, a, false), end = boundary(target, b, true);
  const QPointF normal = length > 1 ? QPointF(-direction.y(), direction.x()) / length : QPointF();
  const auto control = (start + end) / 2 + normal * (network && qFuzzyIsNull(bend) ? qMin(32.0, length * .12) : bend);
  QPainterPath path(start);
  path.quadTo(control, end);
  if (length > 1) {
    const auto tangent = end - control;
    const auto unit = tangent / qMax(1.0, std::hypot(tangent.x(), tangent.y()));
    const QPointF normal(-unit.y(), unit.x());
    path.moveTo(end - unit * (8 / viewScale) + normal * (4 / viewScale)); path.lineTo(end);
    path.lineTo(end - unit * (8 / viewScale) - normal * (4 / viewScale));
  }
  const auto &tokens = PaleoTheme::tokens();
  QColor edgeColor = edge.unresolved ? tokens.warning : network ? (targetNode ? targetNode->networkColor() : sourceNode->networkColor()) : tokens.textMuted;
  QPen pen(edgeColor, network ? 1.3 : 1);
  pen.setCosmetic(true);
  if (edge.unresolved || edge.relation == Relation::Rollback || edge.relation == Relation::EditBaseline) pen.setStyle(Qt::DashLine);
  auto *item = scene->addPath(path, pen);
  item->setData(4, network); item->setData(5, edgeColor); item->setAcceptedMouseButtons(Qt::NoButton);
  if (!edge.label.isEmpty()) {
    auto *label = new QGraphicsSimpleTextItem(edge.label, item);
    QFont font = label->font(); font.setPointSize(tokens.labelPt); label->setFont(font);
    label->setBrush(tokens.textMuted);
    label->setFlag(QGraphicsItem::ItemIgnoresTransformations, network);
    const auto midpoint = (start + end) / 4 + control / 2;
    qreal angle = std::atan2(direction.y(), direction.x()) * 180 / 3.141592653589793;
    if (angle > 90) angle -= 180; if (angle < -90) angle += 180;
    if (network) { label->setTransformOriginPoint(label->boundingRect().center()); label->setRotation(angle); }
    const qreal scale = network && !scene->views().isEmpty() ? scene->views().first()->transform().m11() : 1.0;
    label->setPos(midpoint - label->boundingRect().center() / qMax(.01, scale) - normal * (tokens.spacingSm / qMax(.01, scale)));
    label->setAcceptedMouseButtons(Qt::NoButton);
  }
  return item;
}

void updateDataGraphEdgeLabels(QGraphicsPathItem *edge) {
  for (auto *child : edge->childItems())
    if (auto *label = dynamic_cast<QGraphicsSimpleTextItem *>(child)) label->setBrush(PaleoTheme::tokens().textMuted);
}
