// 层：功能
#pragma once
#include <QStringList>
#include <QVariantList>
#include <functional>
class DataCatalog;
class QgisLayerService;
class PaleoTaskService;
class PaleoTask;
class QObject;
namespace PaleoMapLibrary {
struct Request { QStringList files; QString folder, horizon; bool inferHorizons = true; bool normalizeFacies = true; QString microfaciesField; };
struct Result { QStringList assets, errors; int unchanged = 0; bool cancelled = false; };
PaleoTask *importMaps(DataCatalog *catalog, QgisLayerService *layers, PaleoTaskService *tasks,
                     const Request &request, QObject *receiver, std::function<void(const Result &)> done,
                     QString *error = nullptr);
QStringList declareMaps(DataCatalog *catalog, QgisLayerService *layers);
}
