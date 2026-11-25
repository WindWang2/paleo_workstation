// 层：QGIS 封装
#include "faciespartitiontool.h"
#include "faciespartitioneditor.h"
#include <QKeyEvent>
#include <qgscurve.h>
#include <qgscurvepolygon.h>
#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>
#include <qgspolygon.h>
#include <qgsrubberband.h>
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
FaciesPartitionTool::~FaciesPartitionTool() = default;
void FaciesPartitionTool::activate() { QgsMapToolCapture::activate(); startCapturing(); }
void FaciesPartitionTool::deactivate() { clearPreview(); stopCapturing(); QgsMapToolCapture::deactivate(); }
void FaciesPartitionTool::clearPreview() {
  m_kept.reset();
  m_cut.reset();
  m_previewStroke.clear();
  m_previewArea.clear();
}
bool FaciesPartitionTool::previewCut() {
  if (m_operation != Paint || size() < 3 || !m_layer || canvas()->currentLayer() != m_layer) return false;
  auto polygon = std::make_unique<QgsPolygon>();
  polygon->setExteriorRing(captureCurve()->segmentize());
  QgsGeometry stroke(polygon.release());
  if (!stroke.isGeosValid()) return false;
  const auto kept = FaciesPartitionEditor::clippedPaint(m_layer, stroke);
  const auto cut = stroke.difference(kept);
  if (kept.isEmpty() || cut.isEmpty() || cut.area() <= 0) return false;
  if (stroke.asWkb() == m_previewStroke && kept.asWkb() == m_previewArea) return false;
  clearPreview();
  m_previewStroke = stroke.asWkb();
  m_previewArea = kept.asWkb();
  m_kept = std::make_unique<QgsRubberBand>(canvas(), Qgis::GeometryType::Polygon);
  m_kept->setToGeometry(kept, m_layer);
  m_cut = std::make_unique<QgsRubberBand>(canvas(), Qgis::GeometryType::Polygon);
  m_cut->setBrushStyle(Qt::BDiagPattern);
  m_cut->setLineStyle(Qt::DashLine);
  m_cut->setToGeometry(cut, m_layer);
  emit messageEmitted(tr("实线内将保留，斜线部分将裁掉。再次右键确认，或退格修正笔画。"), Qgis::MessageLevel::Info);
  return true;
}
void FaciesPartitionTool::keyPressEvent(QKeyEvent *event) {
  clearPreview();
  if (event->key() == Qt::Key_Escape) emit editAborted();
  QgsMapToolCapture::keyPressEvent(event);
}
void FaciesPartitionTool::cadCanvasReleaseEvent(QgsMapMouseEvent *event) {
  if (event->button() == Qt::RightButton && previewCut()) return;
  clearPreview();
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
