// 层：QGIS 封装
#pragma once
#include "../metadata/layermanifest.h"

class QgsMapLayer;
class QString;

// 编图参考图层装载（克隆现有层或按声明新建 + 恢复 CRS + 套样式）。
// 与 MappingReferenceCanvas（QWidget）分开声明：只需装载图层的视图层 TU
// 不必经画布头传递引入 QtWidgets（transitive 闸 ui_qtwidgets_notes）。
namespace MappingReferenceLoader {
QgsMapLayer *load(const LayerDeclaration &declaration, QgsMapLayer *existing, const QString &stylesRoot);
} // namespace MappingReferenceLoader
