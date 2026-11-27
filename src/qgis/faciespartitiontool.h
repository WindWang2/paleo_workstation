// 层：QGIS 封装
#pragma once
#include <qgsmaptoolcapture.h>
#include <QPointer>
#include <functional>
#include <memory>
class QgsRubberBand;
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
  ~FaciesPartitionTool() override;
  void activate() override;
  void deactivate() override;
signals:
  void featureEdited(const QString &layerId);
  void editAborted();
protected:
  void keyPressEvent(QKeyEvent *event) override;
  void cadCanvasReleaseEvent(QgsMapMouseEvent *event) override;
private:
  QPointer<QgsVectorLayer> m_layer;
  Operation m_operation;
  std::function<int()> m_targetCode;
  std::unique_ptr<QgsRubberBand> m_kept, m_cut, m_issueMarker;
  QByteArray m_previewStroke, m_previewArea;
  QgsFeatureIds m_splitTargets;
  bool m_repairing = false, m_applying = false;
  bool previewCut(const QgsGeometry &stroke);
  QgsGeometry strokeGeometry() const;
  void clearPreview();
  void discardStroke();
  QString snapDescription() const;
  void updateHint(const QString &next = {}, Qgis::MessageLevel level = Qgis::MessageLevel::Info);
  void markIssue(const QgsGeometry &geometry);
  void finish(const QgsGeometry &geometry);
};
