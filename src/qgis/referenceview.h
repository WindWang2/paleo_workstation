// 层：QGIS 封装
#pragma once

#include <QVariantMap>

class QgsMapCanvas;
class QgsMapLayer;
class QgsProject;
class QWidget;

// 参考证据画布的视图侧契约（issue #825）。
//
// 视图层只需要「开证据 / 清证据 / 联动 / 激活」这几类操作，不应为了构造控件
// 而 include qgis/mappingreferencecanvas.h——该头经 qgis/previewmapcanvas.h
// 传递引入 <QWidget>，会给视图层新增一条 QtWidgets 传递记录边
//（tools/check_layering.py --transitive 的 ui_qtwidgets_notes）。
// 构造点留在 qgis 层，面板只持有本接口；QWidget 仅前向声明。
class ReferenceView
{
public:
  virtual ~ReferenceView() = default;
  // 控件本体：加入布局/显隐用；所有权随父对象，调用方不得 delete。
  virtual QWidget *widget() = 0;
  virtual QgsMapCanvas *canvas() const = 0;
  virtual void setReference(QgsMapLayer *layer, const QVariantMap &metadata) = 0;
  virtual void clearReference() = 0;
  virtual bool hasReference() const = 0;
  virtual void setLinked(bool linked) = 0;
  virtual void setViewActive(bool active) = 0;
};

// 唯一构造点（qgis 层实现）。返回值随 parent 一起销毁。
ReferenceView *createReferenceView(QgsMapCanvas *mainCanvas, QgsProject *project,
                                   QWidget *parent);
