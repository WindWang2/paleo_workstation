// 层：视图
#pragma once
#include <QGraphicsView>
#include <QMouseEvent>
#include <QWheelEvent>
#include <functional>
#include <cmath>

class DataGraphCanvas final : public QGraphicsView {
public:
  using QGraphicsView::QGraphicsView;
  std::function<void()> clearFocusRequested;
protected:
  void wheelEvent(QWheelEvent *event) override {
    if (!event->angleDelta().y()) { QGraphicsView::wheelEvent(event); return; }
    const qreal current = transform().m11();
    const qreal next = qBound(.15, current * std::pow(1.2, event->angleDelta().y() / 120.0), 4.0);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    scale(next / current, next / current);
    event->accept();
  }
  void mousePressEvent(QMouseEvent *event) override {
    const bool empty = !itemAt(event->position().toPoint());
    QGraphicsView::mousePressEvent(event);
    if (empty && event->button() == Qt::LeftButton && clearFocusRequested) clearFocusRequested();
  }
};
