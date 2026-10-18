// 层：QGIS 封装
#pragma once
#include "../domain/exportrequest.h"
#include <QVariantMap>
#include <functional>
class QgsMapLayer;

namespace PaleoDataExportWriter {
// Called with private file-backed layers in the worker, never a live GUI layer.
bool write(const QString &source, const QString &destination,
           paleo::exporting::DataFormat format, QStringList *files,
           QVariantMap *description, QString *error,
           const std::function<bool()> &cancel = {});
bool writeLayer(QgsMapLayer *layer, const QString &directory, QStringList *files,
                QVariantMap *description, QString *error);
}
