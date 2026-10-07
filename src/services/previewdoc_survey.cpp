// 层：数据
#include "previewdoc.h"
#include "catalog/datacatalog.h"
#include "io/dataimportservice.h"
#include "io/segyreader.h"
#include "paleotaskservice.h"
#include <cmath>

void PreviewDocService::requestSurveyBounds(const QString &assetId,
    std::function<void(const QVector<QPair<double, double>> &, const QString &)> done)
{
  const int generation = ++m_surveyGeneration;
  if (m_surveyTask)
    m_surveyTask->requestCancel();
  const QString path = assetSource(assetId);
  const QString cacheDir = m_svc ? m_svc->indexCacheDir() : QString();
  const auto cached = m_segyReaders.value(assetId);
  auto reader = std::make_shared<std::shared_ptr<SegyReader>>(cached);
  auto corners = std::make_shared<QVector<QPair<double, double>>>();
  const auto work = [path, cacheDir, reader, corners](PaleoTask *task) -> QString {
    if (!*reader) {
      SegyOptions options;
      options.cancel = [task] { return task && task->cancelRequested(); };
      options.progress = [task](qint64 done, qint64 total) {
        if (task) task->reportBytes(done, total);
      };
      *reader = std::make_shared<SegyReader>();
      QString error;
      if (!(*reader)->openCached(path, cacheDir, &error, &options))
        return error;
    }
    const auto geometry = (*reader)->geometry();
    for (int corner = 0; corner < 4; ++corner)
      if (std::isfinite(geometry.cornerX[corner]) && std::isfinite(geometry.cornerY[corner]))
        corners->append({geometry.cornerX[corner], geometry.cornerY[corner]});
    return corners->size() == 4 ? QString() : tr("完整测区角点不可用");
  };
  const auto apply = [this, assetId, generation, reader, corners, done](bool ok, const QString &error) {
    if (generation != m_surveyGeneration)
      return;
    m_surveyTask.clear();
    if (ok)
      m_segyReaders.insert(assetId, *reader);
    if (done)
      done(ok ? *corners : QVector<QPair<double, double>>(), error);
  };
  if (m_taskSvc) {
    auto *task = m_taskSvc->start(tr("恢复完整测区范围"), work, QString(), true);
    m_surveyTask = task;
    connect(task, &PaleoTask::finished, this, [apply, task] {
      if (task->state() != PaleoTask::State::Cancelled)
        apply(task->state() == PaleoTask::State::Succeeded, task->errorText());
    });
  } else {
    const QString error = work(nullptr);
    apply(error.isEmpty(), error);
  }
}
