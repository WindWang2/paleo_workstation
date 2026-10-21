// 层：QGIS 封装
#pragma once
#include "previewmapcanvas.h"
#include "../metadata/layermanifest.h"
#include <QVariantMap>

class MapCanvasLink;

// 编图参考图只使用独立图层；范围/光标连接不会接管主图的编辑工具。
class MappingReferenceCanvas : public PreviewMapCanvas
{
  Q_OBJECT
public:
  MappingReferenceCanvas(QgsMapCanvas *mainCanvas, QgsProject *project,
                         QWidget *parent = nullptr);
  ~MappingReferenceCanvas() override;
  void setReference(QgsMapLayer *layer, const QVariantMap &metadata);
  void clearReference();
  bool hasReference() const;
  void setLinked(bool linked);
  void setViewActive(bool active);

signals:
  void legendChanged(const QVariantList &entries);

protected:
  void resizeEvent(QResizeEvent *event) override;

private:
  void updateLegend();
  void updateLink();
  QPointer<QgsMapCanvas> m_mainCanvas;
  QPointer<QgsMapLayer> m_layer;
  MapCanvasLink *m_link = nullptr;
  QVariantList m_schema;
  QString m_legendLevel;
  bool m_linked = true, m_active = false, m_facies = false, m_resizeSyncQueued = false;
};
