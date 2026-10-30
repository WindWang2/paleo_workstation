// 层：QGIS 封装
#pragma once
#include "previewmapcanvas.h"
#include "referenceview.h"
#include "../metadata/layermanifest.h"
#include <QVariantMap>

class MapCanvasLink;

// 编图参考图只使用独立图层；范围/光标连接不会接管主图的编辑工具。
// 同时实现 qgis/referenceview.h 的视图侧接口：面板只依赖该接口，避免
// 视图层为构造控件直接 include 本头（传递引入 QtWidgets 的可见债）。
class MappingReferenceCanvas : public PreviewMapCanvas, public ReferenceView
{
  Q_OBJECT
public:
  MappingReferenceCanvas(QgsMapCanvas *mainCanvas, QgsProject *project,
                         QWidget *parent = nullptr);
  ~MappingReferenceCanvas() override;
  QWidget *widget() override { return this; }
  QgsMapCanvas *canvas() const override;
  void setReference(QgsMapLayer *layer, const QVariantMap &metadata) override;
  void clearReference() override;
  bool hasReference() const override;
  void setLinked(bool linked) override;
  void setViewActive(bool active) override;

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
