// 层：视图
#pragma once
#include <QGraphicsView>
#include <QGraphicsItem>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QResizeEvent>
#include <functional>
#include <cmath>

class DataGraphCanvas final : public QGraphicsView {
public:
  using QGraphicsView::QGraphicsView;
  std::function<void()> clearFocusRequested, viewTransformChanged;
  void zoomBy(qreal factor) {
    const qreal current = transform().m11(); const qreal next = qBound(.15, current * factor, 4.0);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse); scale(next / current, next / current);
    if (viewTransformChanged) viewTransformChanged();
  }
protected:
  void wheelEvent(QWheelEvent *event) override {
    if (!event->angleDelta().y()) { QGraphicsView::wheelEvent(event); return; }
    zoomBy(std::pow(1.2, event->angleDelta().y() / 120.0));
    event->accept();
  }
  void mousePressEvent(QMouseEvent *event) override {
    auto *hit = itemAt(event->position().toPoint());
    const bool empty = !hit || !hit->flags().testFlag(QGraphicsItem::ItemIsSelectable);
    QGraphicsView::mousePressEvent(event);
    if (empty && event->button() == Qt::LeftButton && clearFocusRequested) clearFocusRequested();
  }
};

// Cards occupy their own hit regions; the uncovered canvas keeps receiving pan/zoom.
class DataGraphSurface final : public QWidget {
public:
  using QWidget::QWidget;
  QWidget *view = nullptr, *details = nullptr, *relations = nullptr, *summary = nullptr, *zoom = nullptr, *relationsToggle = nullptr;
  bool detailsWanted = false, narrowRelations = false;
  void arrange() {
    if (!view) return;
    view->setGeometry(rect());
    const int inset = 16, cardWidth = qMin(280, qMax(220, width() / 3));
    if (details) {
      details->setVisible(detailsWanted && !(width() < 900 && narrowRelations));
      details->setGeometry(inset, inset, cardWidth, qMax(160, qMin(height() - 96, details->property("compactCard").toBool() ? 360 : 500)));
    }
    if (relationsToggle) relationsToggle->setVisible(width() < 900);
    if (relations) {
      relations->setVisible(width() >= 900 || narrowRelations);
      relations->setGeometry(width() - cardWidth - inset, inset, cardWidth, qMax(120, qMin(height() - 96, relations->sizeHint().height())));
    }
    if (summary) { summary->adjustSize(); summary->move(inset, height() - summary->height() - inset); }
    if (zoom) { zoom->adjustSize(); zoom->move(width() - zoom->width() - inset, height() - zoom->height() - inset); }
  }
protected:
  void resizeEvent(QResizeEvent *e) override { QWidget::resizeEvent(e); arrange(); }
};
