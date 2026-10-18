// 层：QGIS 封装
#pragma once
#include <QVariantList>
class QgsMapLayer;
namespace FaciesMapLegend {
// QGIS 图层树、版面图例和导出共用当前渲染器的符号及三级分类。
void install(QgsMapLayer *layer);
// 仅供显示的瞬时符号预览，不写入分类快照或图层持久属性。
QVariantList symbolEntries(QgsMapLayer *layer, const QVariantList &entries);
// Filter the current renderer legend to classes actually present in the layer.
QVariantList presentEntries(QgsMapLayer *layer, const QVariantList &entries);
}
