// 层：QGIS 封装
#pragma once
#include <QString>
#include <QVariantList>
#include <functional>
#include <qgsgeometry.h>

#include "faciespartitionstate.h"

class QgsMapCanvas;
class QgsMapTool;
class QgsAdvancedDigitizingDockWidget;
class QgsVectorLayer;

// One leaf partition owns all three display levels. Operations never commit
// providers: geometry, attributes and observer audits share one native command.
// Parameter names mirror the definitions (readability-inconsistent-declaration-parameter-name).
namespace FaciesPartitionEditor {
bool isPartition(const QgsVectorLayer *v);
QVariantList choices(const QgsVectorLayer *v);
int selectionSize(const QgsVectorLayer *v);
QString layerKey(const QgsVectorLayer *v);
QString footprint(QgsVectorLayer *v, QString *error = nullptr);
bool install(QgsVectorLayer *v, const QVariantList &source,
             const QString &frozenFootprint = {}, QString *error = nullptr);
bool validate(QgsVectorLayer *v, QString *error = nullptr);
bool split(QgsVectorLayer *v, const QgsGeometry &line, QString *error = nullptr);
// Paint is clipped to the existing footprint (including existing no-data holes).
bool paint(QgsVectorLayer *v, const QgsGeometry &polygon, int code,
           QString *error = nullptr);
// code=0 means preserve a common microfacies; mixed classes need a target.
bool merge(QgsVectorLayer *v, int code = 0, QString *error = nullptr);
// #533：相面捕获工具经本门面创建——视图层不再直接 include faciespartitiontool.h
//（该头经 QgsMapToolCapture 传递引入 QtWidgets 边，ui_qtwidgets_notes 因此超
// baseline）。工具栏只拿一个普通 QgsMapTool。
enum class PartitionOperation { Split, Paint };
// canvas 不持有工具所有权：调用方装到画布上并负责拆除。\a aborted 在用户取消
// 捕获（Esc / 右键）时触发——工具的 editAborted 信号在本层接到它上面（视图层
// 拿不到工具的具体类型，无法写成员指针 connect）。
QgsMapTool *createCaptureTool(QgsMapCanvas *canvas, QgsAdvancedDigitizingDockWidget *cad,
                              QgsVectorLayer *layer, PartitionOperation operation,
                              const std::function<int()> &targetCode,
                              const std::function<void()> &aborted);
}
