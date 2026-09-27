// 层：数据
#include "services/seismictaskservice.h"

#include <QFileInfo>
#include <QMetaObject>
#include <QTimer>

#include "domain/seismic/sgyindexbuilder.h"
#include "domain/seismic/sgyindexcache.h"
#include "services/paleotaskservice.h"

namespace seismic {

SeismicTaskService::SeismicTaskService(PaleoTaskService *taskService,
                                       std::size_t dataCacheBudgetMb,
                                       QObject *parent)
  : QObject(parent),
    taskService_(taskService),
    dataCache_(dataCacheBudgetMb * 1024ull * 1024ull)
{
}

PaleoTask *SeismicTaskService::startIndexing(
    const QString &sgyPath,
    bool forceReindex,
    std::function<void(bool, SgyIndexPtr, const QString &)> onFinished,
    const QString &layerId)
{
  if (!taskService_)
  {
    if (onFinished)
      onFinished(false, nullptr, QStringLiteral("PaleoTaskService not set"));
    return nullptr;
  }

  const std::filesystem::path stdPath(sgyPath.toStdString());
  const QString title = tr("索引 SEG-Y: %1").arg(QFileInfo(sgyPath).fileName());
  auto resultIndex = std::make_shared<SgyIndexPtr>();

  auto work = [this, stdPath, forceReindex, resultIndex](PaleoTask *task) -> QString {
    if (!forceReindex)
    {
      std::string reason;
      task->reportDetail(tr("检查本地磁盘索引缓存..."));
      if (auto cached = SgyIndexCache::Load(stdPath, reason))
      {
        if (cached->complete)
        {
          *resultIndex = cached;
          task->reportBytes(cached->traceCount, cached->traceCount);
          return QString();
        }
      }
    }

    if (task->cancelRequested())
      return QString();

    task->reportDetail(tr("正在进行全卷道头扫描与几何提取..."));
    SgyIndexBuildOptions opts;
    opts.progressInterval = 64; // 敏捷响应取消（<50ms）
    std::string err;
    SgyIndexPtr index;

    auto progress = [task](int processed, int total) -> bool {
      if (task->cancelRequested())
        return false;
      task->reportBytes(processed, total);
      return true;
    };

    if (!SgyIndexBuilder::Build(stdPath, index, err, progress, opts))
    {
      if (task->cancelRequested())
        return QString();
      return QString::fromStdString(err);
    }

    if (index && !task->cancelRequested())
    {
      std::string saveErr;
      SgyIndexCache::Save(index, saveErr);
      *resultIndex = index;
    }
    return QString();
  };

  PaleoTask *task = taskService_->start(title, work, layerId);
  connect(task, &PaleoTask::finished, this, [this, task, sgyPath, resultIndex, onFinished]() {
    const bool success = (task->state() == PaleoTask::State::Succeeded);
    if (onFinished)
    {
      if (task->state() == PaleoTask::State::Cancelled)
        onFinished(false, nullptr, tr("任务已取消"));
      else if (task->state() == PaleoTask::State::Failed)
        onFinished(false, nullptr, task->errorText());
      else
        onFinished(true, *resultIndex, QString());
    }
    emit indexingFinished(sgyPath, success);
  });

  return task;
}

PaleoTask *SeismicTaskService::startSliceExtraction(
    std::shared_ptr<SgyVolume> volume,
    SgySliceType type,
    int sliceIndex,
    std::function<void(bool, std::shared_ptr<const SgySliceImage>, const QString &)> onFinished)
{
  if (!volume || !volume->IsIndexComplete())
  {
    if (onFinished)
      onFinished(false, nullptr, tr("地震体未完成索引，无法提取切片"));
    return nullptr;
  }

  const char *typeStr = (type == SgySliceType::Inline) ? "inl" : ((type == SgySliceType::Xline) ? "xl" : "time");
  const std::string key = SgyDataCache::MakeKey(
      typeStr, volume->Index()->fileSize, static_cast<std::uint64_t>(sliceIndex));

  // 1. 检查内存 LRU 缓存
  if (auto cached = dataCache_.Find(key))
  {
    if (onFinished)
    {
      QTimer::singleShot(0, this, [onFinished, cached]() {
        onFinished(true, cached, QString());
      });
    }
    return nullptr;
  }

  if (!taskService_)
  {
    if (onFinished)
      onFinished(false, nullptr, QStringLiteral("PaleoTaskService not set"));
    return nullptr;
  }

  const QString title = tr("提取地震切片 (%1: %2)").arg(QString::fromLatin1(typeStr)).arg(sliceIndex);
  auto outImage = std::make_shared<SgySliceImage>();

  auto work = [volume, type, sliceIndex, outImage](PaleoTask *task) -> QString {
    std::string err;
    auto progress = [task](int processed, int total) -> bool {
      if (task->cancelRequested())
        return false;
      task->reportBytes(processed, total);
      return true;
    };

    if (!volume->ExtractSlice(type, sliceIndex, *outImage, err, progress))
    {
      if (task->cancelRequested())
        return QString();
      return QString::fromStdString(err);
    }
    return QString();
  };

  PaleoTask *task = taskService_->start(title, work);
  connect(task, &PaleoTask::finished, this, [this, task, key, outImage, onFinished]() {
    if (task->state() == PaleoTask::State::Succeeded)
    {
      dataCache_.Store(key, outImage);
      if (onFinished)
        onFinished(true, outImage, QString());
    }
    else
    {
      if (onFinished)
      {
        if (task->state() == PaleoTask::State::Cancelled)
          onFinished(false, nullptr, tr("切片提取已取消"));
        else
          onFinished(false, nullptr, task->errorText());
      }
    }
  });

  return task;
}

PaleoTask *SeismicTaskService::startSectionExtraction(
    std::shared_ptr<SgyVolume> volume,
    const std::vector<glm::ivec2> &pathPoints,
    const SgySectionOptions &options,
    std::function<void(bool, std::shared_ptr<const SgySliceImage>, const SgySectionStats &, const QString &)> onFinished)
{
  if (!volume || !volume->IsIndexComplete())
  {
    if (onFinished)
      onFinished(false, nullptr, SgySectionStats{}, tr("地震体未完成索引，无法提取剖面"));
    return nullptr;
  }

  if (!taskService_)
  {
    if (onFinished)
      onFinished(false, nullptr, SgySectionStats{}, QStringLiteral("PaleoTaskService not set"));
    return nullptr;
  }

  const QString title = tr("提取任意测线/过井剖面 (%1 节点)").arg(pathPoints.size());
  auto outImage = std::make_shared<SgySliceImage>();
  auto outStats = std::make_shared<SgySectionStats>();

  auto work = [volume, pathPoints, options, outImage, outStats](PaleoTask *task) -> QString {
    std::string err;
    auto progress = [task](int processed, int total) -> bool {
      if (task->cancelRequested())
        return false;
      task->reportBytes(processed, total);
      return true;
    };

    if (!BuildLineSection(*volume, pathPoints, options, *outImage, *outStats, err, progress))
    {
      if (task->cancelRequested())
        return QString();
      return QString::fromStdString(err);
    }
    return QString();
  };

  PaleoTask *task = taskService_->start(title, work);
  connect(task, &PaleoTask::finished, this, [task, outImage, outStats, onFinished]() {
    if (task->state() == PaleoTask::State::Succeeded)
    {
      if (onFinished)
        onFinished(true, outImage, *outStats, QString());
    }
    else
    {
      if (onFinished)
      {
        if (task->state() == PaleoTask::State::Cancelled)
          onFinished(false, nullptr, *outStats, tr("剖面提取已取消"));
        else
          onFinished(false, nullptr, *outStats, task->errorText());
      }
    }
  });

  return task;
}

} // namespace seismic
