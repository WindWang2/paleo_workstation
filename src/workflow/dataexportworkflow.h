// 层：功能
#pragma once
#include "../domain/exportrequest.h"
#include <functional>
class DataCatalog;
class QObject;
class PaleoTask;
class PaleoTaskService;
PaleoTask *startDataExport(DataCatalog *catalog, PaleoTaskService *tasks,
    const paleo::exporting::DataRequest &request, QObject *receiver,
    std::function<void(const paleo::exporting::Result &)> done, QString *error = nullptr);
