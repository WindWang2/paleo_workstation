// 层：QGIS 封装
#pragma once

#include <QObject>
#include <QPointer>

class QgsMapLayer;
class QgsProject;

// 标注随图层 z 序：QGIS 原生的 PAL 标注引擎把全部标注统一画在渲染最后一遍，
// 效果等同永远置顶——下层井位/顶点标注会「穿透」压在它们之上的栅格与
// 多边形。vendored QGIS（superbuild 补丁
// patches/qgis-4.2.2-labels-with-layer.patch，编译期宏
// QGIS_PALEO_LABELS_WITH_LAYER）支持图层自定义属性
// rendering/labelsWithLayer=true：该层标注画进图层自己的渲染目标、
// 按图层树序被上层图层盖住，且只与本层标注避让。
// 本类把该属性钉到工程内每个图层上（存量图层、layersAdded、readProject
// 之后），并清掉旧实现写过的 rendering/renderAboveLabels 残留属性。
// 未打补丁的 QGIS 构建里该属性是无害空值——标注回到原生置顶，优雅降级。
class QgisLabelZOrder : public QObject
{
    Q_OBJECT
  public:
    explicit QgisLabelZOrder( QgsProject *project, QObject *parent = nullptr );

    // 立即给全部图层打标（构造时已调用一次；测试可直接调用）。
    void refresh();

  private:
    void applyToLayer( QgsMapLayer *layer );

    QPointer<QgsProject> m_project;
};
