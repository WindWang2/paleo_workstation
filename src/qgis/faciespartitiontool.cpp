// 层：QGIS 封装
#include "faciespartitiontool.h"
#include "faciespartitioneditor.h"
#include <QKeyEvent>
#include <qgscurve.h>
#include <qgscurvepolygon.h>
#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>
#include <qgsvectorlayer.h>
#include <memory>
FaciesPartitionTool::FaciesPartitionTool(QgsMapCanvas *canvas,
    QgsAdvancedDigitizingDockWidget *cad, QgsVectorLayer *layer, Operation operation,
    std::function<int()> targetCode)
    : QgsMapToolCapture(canvas, cad, operation == Split ? CaptureLine : CapturePolygon),
      m_layer(layer), m_operation(operation), m_targetCode(std::move(targetCode)) {
  setToolName(operation == Split ? tr("分割相面") : tr("覆盖绘制相面"));
  setCurrentCaptureTechnique(Qgis::CaptureTechnique::StraightSegments);
}
void FaciesPartitionTool::activate() { QgsMapToolCapture::activate(); startCapturing(); }
void FaciesPartitionTool::deactivate() { stopCapturing(); QgsMapToolCapture::deactivate(); }
void FaciesPartitionTool::keyPressEvent(QKeyEvent *event) {
  if (event->key() == Qt::Key_Escape) emit editAborted();
  QgsMapToolCapture::keyPressEvent(event);
}
void FaciesPartitionTool::cadCanvasReleaseEvent(QgsMapMouseEvent *event) {
  const bool cancel = event->button() == Qt::RightButton && size() < (m_operation == Split ? 2 : 3);
  QgsMapToolCapture::cadCanvasReleaseEvent(event);
  if (cancel) emit editAborted();
}
void FaciesPartitionTool::lineCaptured(const QgsCurve *line) {
  const std::unique_ptr<const QgsCurve> owned(line);
  if (line) finish(QgsGeometry(line->segmentize()));
  stopCapturing();
}
void FaciesPartitionTool::polygonCaptured(const QgsCurvePolygon *polygon) {
  if (polygon) finish(QgsGeometry(polygon->segmentize()));
  stopCapturing();
}
void FaciesPartitionTool::finish(const QgsGeometry &geometry) {
  QString error;
  if (!m_layer || canvas()->currentLayer() != m_layer) error = tr("编辑目标已切换，请重新选择相面工具");
  else {
    const bool ok = m_operation == Split ? FaciesPartitionEditor::split(m_layer, geometry, &error)
        : FaciesPartitionEditor::paint(m_layer, geometry, m_targetCode(), &error);
    if (ok) { canvas()->refresh(); emit featureEdited(m_layer->id()); return; }
  }
  emit messageEmitted(error, Qgis::MessageLevel::Warning);
}
