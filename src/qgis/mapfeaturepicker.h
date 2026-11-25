// 层：QGIS 封装
#pragma once
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <functional>
#include <memory>

class QgsHighlight;
class QgsMapCanvas;
class QgsMapTool;
class QgsVectorLayer;
class QMenu;

// Point picking and delete share the same map-space search and preview.
class MapFeaturePicker : public QObject {
  Q_OBJECT
public:
  static QgsMapTool *createSelectionTool(QgsMapCanvas *canvas);
  enum class Result { Empty, Handled, Failed };
  explicit MapFeaturePicker(QgsMapCanvas *canvas, QgsMapTool *owner);
  ~MapFeaturePicker() override;
  void hover(QgsVectorLayer *layer, const QPoint &position);
  Result choose(QgsVectorLayer *layer, const QPoint &position, bool modifying,
                const std::function<void(qint64)> &apply);
  void clear();

private:
  void highlight(QgsVectorLayer *layer, qint64 id);
  QPointer<QgsMapCanvas> m_canvas;
  QPointer<QgsMapTool> m_owner;
  QPointer<QMenu> m_menu;
  std::unique_ptr<QgsHighlight> m_highlight;
};
