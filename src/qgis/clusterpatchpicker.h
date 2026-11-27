// 层：QGIS 封装
#pragma once
#include <QColor>
#include <QObject>
#include <QMouseEvent>
#include <memory>
#include <QPointer>
#include <QVariantList>
class QgsMapCanvas;
class QgsRubberBand;
class QgsVectorLayer;

// Inspect only native pan-tool clicks; editing, wheel zoom and dragging retain their normal routing.
class ClusterPatchPicker : public QObject {
  Q_OBJECT
public:
  explicit ClusterPatchPicker(QgsMapCanvas *canvas);
  void previewDraft(const QString &layerId, const QVariantList &edits);
  void highlight(const QList<qint64> &ids, const QColor &color);
signals:
  void picked(const QString &layerId, qint64 featureId);
  void cleared();
protected:
  bool eventFilter(QObject *object, QEvent *event) override;
private:
  QgsVectorLayer *visibleTarget() const;
  QgsMapCanvas *m_canvas;
  QPointer<QgsRubberBand> m_outline;
  QList<qint64> m_highlightIds;
  QColor m_highlightColor;
  QList<QPointer<QgsRubberBand>> m_previewBands;
  QString m_draftLayer;
  QVariantList m_draftEdits;
  void forwardPress();
  std::unique_ptr<QMouseEvent> m_press;
  bool m_forwarding = false;
};
