// 层：QGIS 封装
#pragma once
#include <QImage>
#include <QVariantMap>
class QgsMapLayer;

// Immutable preview of a registered study-area polygon, including its CRS.
namespace FaciesMapBoundary {
QVariantMap inspect(QgsMapLayer *source, QString *error);
QImage thumbnail(const QByteArray &wkb, QSize size, const QColor &color);
bool seed(QgsMapLayer *draft, const QVariantMap &boundary, int code, QString *error);
}
