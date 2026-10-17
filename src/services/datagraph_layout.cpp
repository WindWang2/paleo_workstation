// 层：数据
#include "datagraph.h"
#include <cmath>
#include <algorithm>
namespace paleo::datagraph {
void layoutKnowledgeGraph(Graph &graph, const Cancel &cancel) {
  const int count = graph.nodes.size();
  if (!count) return;
  QVector<QPointF> positions(count), movement(count);
  QHash<QString, int> rows;
  int anchor = 0;
  for (int i = 0; i < count; ++i) {
    rows.insert(graph.nodes[i].id, i);
    if (graph.nodes[i].id == graph.query.focus) anchor = i;
    const double angle = i * 2.399963229728653;
    const double radius = 250.0 * std::sqrt(i + 1.0);
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
      const auto spring = delta * ((distance - 360.0) * .20 / distance);
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
        const double x = 290 - std::abs(delta.x()), y = 135 - std::abs(delta.y());
        if (x <= 0 || y <= 0) continue;
        overlap = true;
        const auto correction = x < y ? QPointF((delta.x() < 0 ? -1 : 1) * (x + 1) / 2, 0)
            : QPointF(0, (delta.y() < 0 ? -1 : 1) * (y + 1) / 2);
        positions[i] -= correction; positions[j] += correction;
      }
    if (!overlap) break;
  }
  for (int i = 0; i < count; ++i) graph.nodes[i].position = positions[i];
}
}
