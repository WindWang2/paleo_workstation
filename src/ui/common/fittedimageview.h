// 层：视图
#pragma once
#include "../../services/imagelod.h"
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QPainter>
#include <QResizeEvent>
#include <QWheelEvent>
#include <cmath>
#include <functional>

namespace paleo::ui {
// Keep a single decoded image. The native view maps its pixel coordinates to the
// logical viewport, including when its screen, size or device pixel ratio changes.
class FittedImageView final : public QGraphicsView {
public:
  enum class Mode { FitOnly, ZoomAndPan };
  FittedImageView(Mode mode, QWidget *parent) : QGraphicsView(parent), m_mode(mode) {
    setAlignment(Qt::AlignCenter);
    if (m_mode == Mode::FitOnly) {
      setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
      setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }
    setScene(new QGraphicsScene(this));
    setRenderHint(QPainter::SmoothPixmapTransform);
    if (m_mode == Mode::ZoomAndPan) setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
  }
  std::function<void(double)> zoomChanged;
  void setPhoto(QPixmap pixmap) {
    const QSizeF oldSize = sceneRect().size();
    const QPointF oldCenter = mapToScene(viewport()->rect().center());
    pixmap.setDevicePixelRatio(1.0);
    scene()->clear();
    auto *photo = scene()->addPixmap(pixmap);
    photo->setTransformationMode(Qt::SmoothTransformation);
    setSceneRect(QRectF(QPointF(), pixmap.size()));
    if (m_mode == Mode::ZoomAndPan)
      setBackgroundBrush(pixmap.hasAlpha() ? paleo::imagelod::alphaCheckerboard() : QBrush());
    if (m_fit || oldSize.isEmpty()) fitPhoto();
    else {
      const double ratio = oldSize.width() / pixmap.width();
      scale(ratio, ratio);
      centerOn(oldCenter.x() / oldSize.width() * pixmap.width(),
               oldCenter.y() / oldSize.height() * pixmap.height());
      notifyZoom();
    }
  }
  void fitPhoto() {
    m_fit = true;
    if (sceneRect().isEmpty()) return;
    fitInView(sceneRect(), Qt::KeepAspectRatio);
    notifyZoom();
  }
  void originalSize() {
    m_fit = false;
    resetTransform();
    notifyZoom();
  }
  void zoomBy(double factor) {
    if (sceneRect().isEmpty()) return;
    m_fit = false;
    const double current = transform().m11();
    const double next = qBound(0.01, current * factor, 16.0);
    scale(next / current, next / current);
    notifyZoom();
  }
protected:
  void wheelEvent(QWheelEvent *event) override {
    if (m_mode == Mode::FitOnly) { event->ignore(); return; }
    if (!event->angleDelta().y()) { QGraphicsView::wheelEvent(event); return; }
    zoomBy(std::pow(1.2, event->angleDelta().y() / 120.0));
    event->accept();
  }
  void resizeEvent(QResizeEvent *event) override {
    QGraphicsView::resizeEvent(event);
    if (m_fit) fitPhoto();
  }
private:
  void notifyZoom() { if (zoomChanged) zoomChanged(transform().m11()); }
  Mode m_mode;
  bool m_fit = true;
};
} // namespace paleo::ui
