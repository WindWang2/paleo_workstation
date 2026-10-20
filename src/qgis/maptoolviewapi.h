// 层：QGIS 封装
#pragma once

#include <qgsmaptool.h>
#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>

// qgis/maptoolviewapi — 视图层地图工具门面。
//
// 由来：#552 的 identifytool 曾直连 qgsmaptool.h / qgsmapcanvas.h /
// qgsmapmouseevent.h，越过本封装层，qgs-audit 116→118、qgis-spread
// 149→151 双双突破初冻基线。docs/PALEO_QGIS_PLAN.md §2：QGIS 迁移成本
// 只许落在 src/qgis——ui 侧工具类经本面派生与签名，不再直接命名 Qgs
// 类型（别名编译期归一，零运行期成本；moc 对别名基类正常生成
// SuperData::link<PaleoMapTool::staticMetaObject>，Qt5/Qt6 均已实测）。
//
// 新增 ui/maptools 工具一律用本面，不要再 include 外部 qgs* 头。
using PaleoMapTool = QgsMapTool;
using PaleoMapCanvas = QgsMapCanvas;
using PaleoMapMouseEvent = QgsMapMouseEvent;
