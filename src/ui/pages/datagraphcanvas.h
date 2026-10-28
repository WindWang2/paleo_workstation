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
  void includeFitScale(qreal scale) {
    if (std::isfinite(scale) && scale > 0) m_minimumZoom = qMin(.15, scale);
  }
  // issue-812：缩放锚点可配——滚轮用 AnchorUnderMouse（默认，贴着指针缩放），
  // 按钮等无指针位置的操作传 AnchorViewCenter（绕视口中心），避免锚到无关坐标。
  void zoomBy(qreal factor, QGraphicsView::ViewportAnchor anchor = QGraphicsView::AnchorUnderMouse) {
    if (!std::isfinite(factor) || factor <= 0) return;
    const qreal current = transform().m11();
    // A full-graph fit may be below the usual interactive floor. Preserve that
    // floor so zoom-in/out steps stay monotonic and can return to the fit.
    const qreal next = qBound(qMin(m_minimumZoom, current), current * factor, qMax(4.0, current));
    if (qFuzzyCompare(current, next)) return;
    setTransformationAnchor(anchor); scale(next / current, next / current);
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
private:
  qreal m_minimumZoom = .15;
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
