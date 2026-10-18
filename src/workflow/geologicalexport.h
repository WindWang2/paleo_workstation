// 层：功能
#pragma once
#include "../qgis/layoutexport.h"
#include "../domain/exportrequest.h"
#include <QVariantMap>
#include <functional>
class DataCatalog;
class PaleoTask;
class PaleoTaskService;
class QObject;
namespace PaleoGeologicalExport {
struct Request {
  QString destination; // a new batch directory, not an existing directory
  QVector<PaleoLayoutExport::Format> formats{PaleoLayoutExport::Format::Pdf};
  double dpi = 300;
  PaleoLayoutExport::PageRange pages;
  PaleoLayoutExport::ExportOptions options;
  QVariantMap context;
  QStringList sourceVersionIds;
  bool includeData = false, includeSources = false;
  bool registerInProject = true;
};
struct Result {
  QString directory, pdfAssetId, pdfPath, pdfSha256, error;
  QStringList files, assetIds, warnings;
  bool cancelled = false;
};
// layout 为 src/qgis 的图件版面句柄（#531：功能层签名不出现 Qgs* 类型）。
PaleoTask *start(PaleoLayoutExport::LayoutHandle *layout, DataCatalog *catalog, PaleoTaskService *tasks,
    const Request &request, QObject *receiver, std::function<void(const Result &)> done,
    QString *error = nullptr);
}
