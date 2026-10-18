// 层：QGIS 封装
#pragma once
#include "../domain/exportrequest.h"
#include <QVariantMap>
#include <functional>
class QgsMapLayer;
class QgsProject;

namespace PaleoDataExportWriter {
// Called with private file-backed layers in the worker, never a live GUI layer.
bool write(const QString &source, const QString &destination,
           paleo::exporting::DataFormat format, QStringList *files,
           QVariantMap *description, QString *error,
           const std::function<bool()> &cancel = {});
bool writeLayer(QgsMapLayer *layer, const QString &directory, QStringList *files,
                QVariantMap *description, QString *error);
// 按图层 id 写成果数据（#531）：功能层在 worker 侧只持有工程句柄，不再直接
// 取 QgsMapLayer。description 内补 qgis_layer_id / title 两个图件元信息键
// （与地质图导出的 metadata 层数据一致）。
bool writeLayerById(QgsProject *project, const QString &layerId, const QString &directory,
                    QStringList *files, QVariantMap *description, QString *error);
}
