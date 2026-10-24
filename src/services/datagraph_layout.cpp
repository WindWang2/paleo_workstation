// 层：数据
#include "datagraph.h"
#include <QRectF>
#include <cmath>
#include <algorithm>
namespace paleo::datagraph {
namespace {
// Real disconnected collections form separate islands. This is layout only:
// proximity never creates a catalog relationship.
void separateCollections(QVector<QPointF> &positions, const QVector<QPair<int, int>> &edges,
                         const Cancel &cancel) {
  QVector<QVector<int>> adjacent(positions.size()), components;
  for (const auto &[a, b] : edges) { adjacent[a] << b; adjacent[b] << a; }
  QSet<int> visited;
  QVector<QRectF> bounds;
  qreal area = 0;
  for (int root = 0; root < positions.size(); ++root) {
    if (visited.contains(root)) continue;
    QVector<int> component{root}; visited.insert(root);
    QRectF bound(positions[root], QSizeF(160, 80));
    for (int at = 0; at < component.size(); ++at) {
      if (cancel && cancel()) return;
      const int row = component[at]; bound |= QRectF(positions[row], QSizeF(160, 80));
      for (int next : adjacent[row]) if (!visited.contains(next)) { visited.insert(next); component << next; }
    }
    components << component; bounds << bound;
    area += (bound.width() + 72) * (bound.height() + 72);
  }
  if (components.size() < 2) return;
  const qreal shelfWidth = std::sqrt(area) * 1.3;
  qreal x = 0, y = 0, height = 0;
  for (int i = 0; i < components.size(); ++i) {
    const auto &bound = bounds[i];
    if (x > 0 && x + bound.width() > shelfWidth) { y += height + 72; x = 0; height = 0; }
    const QPointF offset = QPointF(x, y) - bound.topLeft();
    for (int row : components[i]) positions[row] += offset;
    x += bound.width() + 72; height = qMax(height, bound.height());
  }
}
}
void layoutKnowledgeGraph(Graph &graph, const Cancel &cancel) {
  const int count = graph.nodes.size();
  if (!count) return;
  const bool compact = graph.query.mode == Mode::Knowledge;
  QVector<QPointF> positions(count), movement(count);
  QHash<QString, int> rows;
  int anchor = 0;
  for (int i = 0; i < count; ++i) {
    rows.insert(graph.nodes[i].id, i);
    if (graph.nodes[i].id == graph.query.focus) anchor = i;
    const double angle = i * 2.399963229728653;
    const double radius = (compact ? 160.0 : 250.0) * std::sqrt(i + 1.0);
    positions[i] = QPointF(std::cos(angle) * radius, std::sin(angle) * radius);
  }
  positions[anchor] = {};
  QVector<QPair<int, int>> edges;
  for (const auto &edge : graph.edges)
    if (rows.contains(edge.source) && rows.contains(edge.target)) edges << qMakePair(rows[edge.source], rows[edge.target]);
  for (int pass = 0; pass < 150; ++pass) {
    if (cancel && cancel()) return;
    movement.fill({});
    for (int i = 0; i < count; ++i)
      for (int j = i + 1; j < count; ++j) {
        QPointF delta = positions[i] - positions[j];
        double distance = std::hypot(delta.x(), delta.y());
        if (distance < 1) { delta = QPointF(1, 1); distance = std::sqrt(2.0); }
        // Keep unrelated collections apart without letting the repulsion push
        // a small, useful graph beyond the viewport's readable zoom range.
        const auto repulsion = delta * (1000.0 / (distance * distance));
        movement[i] += repulsion; movement[j] -= repulsion;
      }
    for (const auto &[a, b] : edges) {
      const auto delta = positions[b] - positions[a];
      const double distance = std::hypot(delta.x(), delta.y());
      if (distance < 1) continue;
      const auto spring = delta * ((distance - (compact ? 240.0 : 360.0)) * .20 / distance);
      movement[a] += spring; movement[b] -= spring;
    }
    const double temperature = 25.0 * (1.0 - pass / 160.0);
    for (int i = 0; i < count; ++i) {
      if (i == anchor) continue;
      auto delta = movement[i] - positions[i] * .002;
      const double length = std::hypot(delta.x(), delta.y());
      if (length > temperature) delta *= temperature / length;
      positions[i] += delta;
    }
  }
  // 标签卡按实际矩形避让；不依靠缩小字体来塞进密集关系。
  for (int pass = 0; pass < 100; ++pass) {
    if (cancel && cancel()) return;
    bool overlap = false;
    for (int i = 0; i < count; ++i)
      for (int j = i + 1; j < count; ++j) {
        const auto delta = positions[j] - positions[i];
        const double x = (compact ? 184 : 290) - std::abs(delta.x()), y = (compact ? 104 : 135) - std::abs(delta.y());
        if (x <= 0 || y <= 0) continue;
        overlap = true;
        const auto correction = x < y ? QPointF((delta.x() < 0 ? -1 : 1) * (x + 1) / 2, 0)
            : QPointF(0, (delta.y() < 0 ? -1 : 1) * (y + 1) / 2);
        positions[i] -= correction; positions[j] += correction;
      }
    if (!overlap) break;
  }
  if (compact) separateCollections(positions, edges, cancel);
  for (int i = 0; i < count; ++i) graph.nodes[i].position = positions[i];
}
}
