// 层：功能
#pragma once
#include <QString>
#include <functional>
class QObject;
class PaleoTask;
class PaleoTaskService;
class QgisProjectService;
class QgisLayerService;
namespace PaleoOfflineBasemap {
struct Request { QString path; bool hillshade = false; bool display = true; };
// Completion reports publication, not just file copying. Cancelled/stale jobs never change settings.
PaleoTask *importFile(QgisProjectService *project, QgisLayerService *layers, PaleoTaskService *tasks,
                      const Request &request, QObject *receiver,
                      const std::function<void(bool, const QString &)> &done, QString *error);
}
