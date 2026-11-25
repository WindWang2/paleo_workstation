// 层：QGIS 封装
#include "faciespartitiontool.h"
#include "faciespartitioneditor.h"
#include <QKeyEvent>
#include <QScopedValueRollback>
#include <qgscompoundcurve.h>
#include <qgscurve.h>
#include <qgslinestring.h>
#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>
#include <qgspolygon.h>
#include <qgsproject.h>
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
  connect(canvas, &QgsMapCanvas::currentLayerChanged, this, [this](QgsMapLayer *current) {
    if (current != m_layer) discardStroke();
  });
  connect(layer, &QgsVectorLayer::editingStopped, this, &FaciesPartitionTool::discardStroke);
  connect(layer, &QgsVectorLayer::willBeDeleted, this, &FaciesPartitionTool::discardStroke);
  if (auto *project = canvas->project())
    connect(project, &QgsProject::aboutToBeCleared, this, &FaciesPartitionTool::discardStroke);
  connect(layer, &QgsVectorLayer::selectionChanged, this, [this] {
    if (m_operation == Split && !m_applying && size()) discardStroke();
  });
}
FaciesPartitionTool::~FaciesPartitionTool() = default;
void FaciesPartitionTool::activate() { QgsMapToolCapture::activate(); startCapturing(); updateHint(); }
void FaciesPartitionTool::deactivate() {
  clearPreview();
  stopCapturing();
  QgsMapToolCapture::deactivate();
}
void FaciesPartitionTool::discardStroke() {
  if (!isActive()) return;
  clearPreview();
  stopCapturing();
  emit editAborted();
}
void FaciesPartitionTool::clearPreview() {
  m_kept.reset();
  m_cut.reset();
  m_issueMarker.reset();
  m_repairing = false;
  m_previewStroke.clear();
  m_previewArea.clear();
}
QgsGeometry FaciesPartitionTool::strokeGeometry() const {
  if (m_operation == Split) return QgsGeometry(captureCurve()->curveToLine());
  auto polygon = std::make_unique<QgsPolygon>();
  polygon->setExteriorRing(captureCurve()->curveToLine());
  return QgsGeometry(polygon.release());
}
bool FaciesPartitionTool::previewCut(const QgsGeometry &stroke) {
  if (m_operation != Paint || !stroke.isGeosValid()) return false;
  const auto kept = FaciesPartitionEditor::clippedPaint(m_layer, stroke);
  if (kept.isEmpty() || kept.area() <= 0 || stroke.isGeosEqual(kept)) return false;
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
  updateHint(tr("实线内将保留，斜线部分将裁掉。再次右键确认，或退格修正笔画。"));
  return true;
}
void FaciesPartitionTool::keyPressEvent(QKeyEvent *event) {
  QgsMapToolCapture::keyPressEvent(event);
  if (event->key() == Qt::Key_Escape) discardStroke();
  else if (event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete) {
    clearPreview();
    updateHint();
  }
}
void FaciesPartitionTool::cadCanvasReleaseEvent(QgsMapMouseEvent *event) {
  if (!m_layer || !m_layer->isEditable() || canvas()->currentLayer() != m_layer) {
    discardStroke();
    return;
  }
  if (event->button() == Qt::RightButton) {
    if (size() < (m_operation == Split ? 2 : 3)) { discardStroke(); return; }
    const auto geometry = strokeGeometry();
    if (!previewCut(geometry)) finish(geometry);
    return;
  }
  if (event->button() == Qt::LeftButton) {
    if (m_repairing && event->modifiers().testFlag(Qt::ShiftModifier)) undo();
    clearPreview();
  }
  QgsMapToolCapture::cadCanvasReleaseEvent(event);
  if (event->button() == Qt::LeftButton) updateHint();
}
void FaciesPartitionTool::updateHint(const QString &next, Qgis::MessageLevel level) {
  const auto matches = snappingMatches();
  const auto *snapped = matches.isEmpty() ? nullptr : matches.last().layer();
  const auto snap = snapped ? tr("吸附到「%1」").arg(snapped->name()) : tr("未吸附");
  const auto action = size() < (m_operation == Split ? 2 : 3) ? tr("左键加点，右键取消") : tr("右键完成，退格撤回一点");
  const auto hint = tr("%1 个顶点 · %2 · %3").arg(size()).arg(snap, next.isEmpty() ? action : next);
  setProperty("paleo.captureHint", hint);
  emit messageEmitted(hint, level);
}
void FaciesPartitionTool::markIssue(const QgsGeometry &geometry) {
  QgsPointXY point(captureCurve()->endPoint());
  if (m_operation == Paint) {
    QVector<QgsGeometry::Error> issues;
    geometry.validateGeometry(issues, Qgis::GeometryValidationEngine::Geos);
    for (const auto &issue : issues) if (issue.hasWhere()) { point = issue.where(); break; }
  }
  m_issueMarker = std::make_unique<QgsRubberBand>(canvas(), Qgis::GeometryType::Point);
  m_issueMarker->setIcon(QgsRubberBand::ICON_X);
  m_issueMarker->setToGeometry(QgsGeometry::fromPointXY(point), m_layer);
}
void FaciesPartitionTool::finish(const QgsGeometry &geometry) {
  clearPreview();
  QString error;
  QScopedValueRollback applying(m_applying, true);
  const bool ok = m_operation == Split ? FaciesPartitionEditor::split(m_layer, geometry, &error)
      : FaciesPartitionEditor::paint(m_layer, geometry, m_targetCode(), &error);
  if (ok) {
    stopCapturing();
    canvas()->refresh();
    updateHint();
    emit featureEdited(m_layer->id());
  } else {
    m_repairing = true;
    markIssue(geometry);
    updateHint(tr("%1。Shift + 左键移动末点，退格撤回一点，或继续加点后右键重试。").arg(error), Qgis::MessageLevel::Warning);
  }
}
