// 层：QGIS 封装
#pragma once
#include <qgsmaptoolcapture.h>
#include <QPointer>
#include <functional>
class QgsVectorLayer;
class QgsAdvancedDigitizingDockWidget;
// Native capture supplies snapping, CAD and layer-CRS geometry. No raster math.
class FaciesPartitionTool final : public QgsMapToolCapture {
  Q_OBJECT
public:
  enum Operation { Split, Paint };
  FaciesPartitionTool(QgsMapCanvas *canvas, QgsAdvancedDigitizingDockWidget *cad,
                      QgsVectorLayer *layer, Operation operation,
                      std::function<int()> targetCode);
  void activate() override;
  void deactivate() override;
signals:
  void featureEdited(const QString &layerId);
  void editAborted();
protected:
  void lineCaptured(const QgsCurve *line) override;
  void polygonCaptured(const QgsCurvePolygon *polygon) override;
  void keyPressEvent(QKeyEvent *event) override;
  void cadCanvasReleaseEvent(QgsMapMouseEvent *event) override;
private:
  QPointer<QgsVectorLayer> m_layer;
  Operation m_operation;
  std::function<int()> m_targetCode;
  void finish(const QgsGeometry &geometry);
};
