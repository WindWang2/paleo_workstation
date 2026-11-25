// 层：QGIS 封装
#pragma once
#include <QList>
#include <QString>
#include <functional>
#include <optional>

class QgsVectorLayer;

namespace QgisTopologyGesture {
struct Step { bool available; QString text; };
bool crossLayerEligible(const QgsVectorLayer *layer);
bool apply(const QList<QgsVectorLayer *> &layers, const QString &title,
           const std::function<bool(QgsVectorLayer *)> &write, QString *error);
// A missing step means the adjacent native command is a single-layer operation.
std::optional<Step> describe(QgsVectorLayer *layer, bool undo);
bool step(QgsVectorLayer *layer, bool undo, QString *error);
}
