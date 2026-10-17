// 层：视图
#include "datagraphsceneitems.h"
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <cmath>
QGraphicsPathItem *addDataGraphEdge(QGraphicsScene *scene, QGraphicsItem *source,
    QGraphicsItem *target, const paleo::datagraph::Edge &edge, double bend) {
  const auto a = source->sceneBoundingRect(), b = target->sceneBoundingRect();
  const auto direction = b.center() - a.center();
  const double length = std::hypot(direction.x(), direction.y());
  const auto boundary = [&direction](QGraphicsItem *item, const QRectF &rect, bool reverse) {
    const double dx = std::abs(direction.x()), dy = std::abs(direction.y());
    const auto *node = dynamic_cast<DataGraphNodeItem *>(item);
    const double scale = node && node->node.kind == Kind::Entity
        ? 1.0 / std::sqrt(4 * dx * dx / (rect.width() * rect.width()) + 4 * dy * dy / (rect.height() * rect.height()) + 1e-12)
        : qMin(dx > 0 ? rect.width() / (2 * dx) : 1e9,
                              dy > 0 ? rect.height() / (2 * dy) : 1e9);
    return rect.center() + direction * (reverse ? -scale : scale);
  };
  const auto start = boundary(source, a, false), end = boundary(target, b, true);
  const QPointF normal = length > 1 ? QPointF(-direction.y(), direction.x()) / length : QPointF();
  const auto control = (start + end) / 2 + normal * bend;
  QPainterPath path(start);
  path.quadTo(control, end);
  if (length > 1) {
    const auto tangent = end - control;
    const auto unit = tangent / qMax(1.0, std::hypot(tangent.x(), tangent.y()));
    const QPointF normal(-unit.y(), unit.x());
    path.moveTo(end - unit * 8 + normal * 4); path.lineTo(end);
    path.lineTo(end - unit * 8 - normal * 4);
  }
  const auto &tokens = PaleoTheme::tokens();
  QPen pen(edge.unresolved ? tokens.warning : tokens.textMuted, 1);
  if (edge.unresolved || edge.relation == Relation::Rollback || edge.relation == Relation::EditBaseline) pen.setStyle(Qt::DashLine);
  auto *item = scene->addPath(path, pen);
  if (!edge.label.isEmpty()) {
    auto *label = new QGraphicsSimpleTextItem(edge.label, item);
    QFont font = label->font(); font.setPointSize(tokens.labelPt); label->setFont(font);
    label->setBrush(tokens.textMuted);
    label->setPos((start + end) / 2 + normal * (bend / 2) - label->boundingRect().center() - QPointF(0, tokens.spacingSm));
    label->setAcceptedMouseButtons(Qt::NoButton);
  }
  return item;
}

void updateDataGraphEdgeLabels(QGraphicsPathItem *edge) {
  for (auto *child : edge->childItems())
    if (auto *label = dynamic_cast<QGraphicsSimpleTextItem *>(child)) label->setBrush(PaleoTheme::tokens().textMuted);
}
