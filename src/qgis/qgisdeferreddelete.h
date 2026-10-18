// 层：QGIS 封装
// #540 子项 1：功能层同步 pump Qt 事件队列是壳/组装根职责，此前渗漏进
// src/workflow/mappingworkbench_edit.cpp 的 releaseLayerHandles()。收到本层：
// 功能层只发请求，事件派发归 QGIS 封装。
//
// #499 的来由：QgsProject::removeMapLayers 走 deleteLater——DeferredDelete 在
// 主线程落地之前，被删 QgsMapLayer 实例仍持 OGR/GDAL 数据源句柄（Windows 上
// 锁定对应 GeoPackage/工作副本文件，删除必败；Linux 允许 unlink 打开的 fd，
// 平台差异被掩盖）。删除（或派发后台删除）任何受管文件前，先显式销毁层实例
// 释放句柄——processEvents/qWait 默认不处理 DeferredDelete，必须
// sendPostedEvents（同 tst_versions #499 审计注释的纪律）。
#pragma once
#include <QCoreApplication>
#include <QEvent>

namespace paleo::qgis {

// 提前落地全队列 DeferredDelete。注意这是全队列 flush：调用点不得处于「对已
// deleteLater 对象仍持引用」的调用链中（Qt 契约下此类引用本就非法）。
inline void releaseDeferredLayerHandles()
{
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

} // namespace paleo::qgis
