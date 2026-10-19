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
struct Request {
  QStringList files;
  // Reprocess registered maps from managed RAW bundles, retaining identity and
  // each map's horizon. Mutually exclusive with files/folder.
  QStringList assetIds;
  QString folder, horizon;
  bool inferHorizons = true, normalizeFacies = true;
  QString microfaciesField;
};
struct Result { QStringList assets, errors; int unchanged = 0; bool cancelled = false; };
PaleoTask *importMaps(DataCatalog *catalog, QgisLayerService *layers, PaleoTaskService *tasks,
                     const Request &request, QObject *receiver, std::function<void(const Result &)> done,
                     QString *error = nullptr);
QStringList declareMaps(DataCatalog *catalog, QgisLayerService *layers);
}
