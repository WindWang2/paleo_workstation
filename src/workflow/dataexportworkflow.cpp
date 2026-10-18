// 层：功能
#include "dataexportworkflow.h"
#include "../services/dataexport.h"
#include "../qgis/dataexportwriter.h"
#include "../services/paleotaskservice.h"
#include <QPointer>
#include <memory>

PaleoTask *startDataExport(DataCatalog *catalog, PaleoTaskService *tasks,
    const paleo::exporting::DataRequest &request, QObject *receiver,
    std::function<void(const paleo::exporting::Result &)> done, QString *error) {
  if (!catalog || !catalog->isOpen() || !tasks || request.versionIds.isEmpty()) {
    if (error) *error = QObject::tr("请先打开工程、选择数据，并连接任务中心");
    return nullptr;
  }
  const auto directory = catalog->projectDir();
  auto snapshot = std::shared_ptr<DataCatalog>(catalog->createStagingCopy(QString()));
  auto result = std::make_shared<paleo::exporting::Result>();
  auto *task = tasks->start((request.previewOnly ? QObject::tr("检查导出数据（%1 个版本）")
                                                : QObject::tr("导出数据（%1 个版本）")).arg(request.versionIds.size()),
      [snapshot, result, directory, request](PaleoTask *task) {
    const auto cancel = [task] { return task->cancelRequested(); };
    task->reportStage("scan", 0);
    const auto plan = paleo::exporting::plan(*snapshot, directory, request, cancel);
    if (cancel()) { result->cancelled = true; return QString(); }
    if (request.previewOnly) {
      result->succeeded = plan.items.size(); result->total = plan.items.size() + plan.failures.size();
      result->plannedBytes = plan.bytes; result->failures = plan.failures; result->warnings = plan.warnings;
      return QString();
    }
    task->reportStage("encode", 10);
    *result = paleo::exporting::execute(plan, cancel, [task](qint64 done, qint64 total, const QString &detail) {
      task->reportBytes(done, total); task->reportDetail(detail);
    }, PaleoDataExportWriter::write);
    return result->error.isEmpty() && !result->failures.isEmpty()
        ? QObject::tr("导出存在 %1 项问题，详见结果目录的 metadata.json").arg(result->failures.size()) : result->error;
  }, {}, request.previewOnly);
  QObject::connect(task, &PaleoTask::finished, receiver ? receiver : tasks,
      [task, result, done] {
    result->cancelled = result->cancelled || task->cancelRequested();
    if (done) done(*result);
  });
  return task;
}
