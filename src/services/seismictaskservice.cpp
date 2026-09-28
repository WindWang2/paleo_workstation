// 层：数据
#include "services/seismictaskservice.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QTimer>

#include "Engine/Sdk.h"
#include "Engine/TranscodeJob.h"
#include "Engine/Types.h"
#include "Engine/WorkspaceFormat.h"

#include "domain/seismic/sgyindexbuilder.h"
#include "domain/seismic/sgyindexcache.h"
#include "domain/seismic/sgyio.h"
#include "services/paleotaskservice.h"

namespace seismic {

struct SeismicDatasetEntry {
  std::shared_ptr<sdk::Dataset> dataset;
  QMutex mutex;          // engine 契约：一次一个线程使用 Dataset
  quint64 lastUse = 0;
};

SeismicTaskService::~SeismicTaskService() = default;

std::shared_ptr<SeismicDatasetEntry> SeismicTaskService::datasetEntryFor(const QString &sgyPath)
{
  auto it = datasetEntries_.find(sgyPath);
  if (it != datasetEntries_.end())
  {
    it.value()->lastUse = ++datasetClock_;
    return it.value();
  }

  sdk::OpenOptions options; // Backend::Auto：有工作区用工作区，否则 Direct
  engine::Status status;
  auto dataset = sdk::Dataset::Open(std::filesystem::path(sgyPath.toStdString()), options, status);
  if (!dataset || !status.ok())
    return nullptr;

  auto entry = std::make_shared<SeismicDatasetEntry>();
  entry->dataset = std::move(dataset);
  entry->lastUse = ++datasetClock_;
  datasetEntries_.insert(sgyPath, entry);

  // 有界缓存：超出 8 个时淘汰最久未用
  while (datasetEntries_.size() > 8)
  {
    QString oldestKey;
    quint64 oldestUse = ~0ull;
    for (auto eit = datasetEntries_.begin(); eit != datasetEntries_.end(); ++eit)
    {
      if (eit.value()->lastUse < oldestUse)
      {
        oldestUse = eit.value()->lastUse;
        oldestKey = eit.key();
      }
    }
    if (oldestKey.isEmpty())
      break;
    datasetEntries_.remove(oldestKey);
  }
  return entry;
}

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

  auto work = [stdPath, forceReindex, resultIndex](PaleoTask *task) -> QString {
    if (!forceReindex)
    {
      std::string reason;
      task->reportDetail(QCoreApplication::translate("seismic::SeismicTaskService", "检查本地磁盘索引缓存..."));
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

    task->reportDetail(QCoreApplication::translate("seismic::SeismicTaskService", "正在进行全卷道头扫描与几何提取..."));
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

  // 引擎优先：sdk::Dataset（Backend::Auto 在工作区存在时用随机访问后端）。
  const QString sgyPath = QString::fromStdString(sgyio::ToUtf8Path(volume->Index()->path));
  const auto entry = datasetEntryFor(sgyPath);

  auto work = [volume, entry, type, sliceIndex, outImage](PaleoTask *task) -> QString {
    if (entry && entry->dataset)
    {
      engine::CancelToken cancel;
      cancel.SetPredicate([task]() { return task->cancelRequested(); });
      engine::Slice2D slice;
      engine::Status status;
      {
        QMutexLocker lock(&entry->mutex);
        switch (type)
        {
        case SgySliceType::Inline:
          status = entry->dataset->ReadInline(sliceIndex, slice, &cancel);
          break;
        case SgySliceType::Xline:
          status = entry->dataset->ReadCrossline(sliceIndex, slice, &cancel);
          break;
        case SgySliceType::Time:
        default:
          status = entry->dataset->ReadCachedTimeSlice(sliceIndex, slice, &cancel);
          break;
        }
      }
      if (status.ok())
      {
        outImage->width = slice.width;
        outImage->height = slice.height;
        outImage->valueMin = slice.valueMin;
        outImage->valueMax = slice.valueMax;
        outImage->values = std::move(slice.values);
        outImage->rgba = std::move(slice.rgba);
        task->reportBytes(outImage->width * outImage->height,
                          outImage->width * outImage->height);
        return QString();
      }
      if (status.code == engine::StatusCode::Cancelled || task->cancelRequested())
        return QString();
      // 引擎路径失败回落 volume 直读（同一份上游实现，保底）
    }

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

  const QString sgyPath = QString::fromStdString(sgyio::ToUtf8Path(volume->Index()->path));
  const auto entry = datasetEntryFor(sgyPath);

  auto work = [volume, entry, pathPoints, options, outImage, outStats](PaleoTask *task) -> QString {
    if (entry && entry->dataset)
    {
      engine::CancelToken cancel;
      cancel.SetPredicate([task]() { return task->cancelRequested(); });
      engine::SectionRequest request;
      request.pathPoints.reserve(pathPoints.size());
      for (const glm::ivec2 &p : pathPoints)
        request.pathPoints.push_back({p.x, p.y});
      request.interpolate = options.interpolate;
      request.keepOutsideColumns = options.keepOutsideColumns;
      request.maxColumns = options.maxColumns;
      request.useReadPlan = true; // 去重+升序+范围合并的扇区读

      engine::Slice2D slice;
      engine::Status status;
      {
        QMutexLocker lock(&entry->mutex);
        status = entry->dataset->ReadSection(request, slice, &cancel);
      }
      if (status.ok())
      {
        outImage->width = slice.width;
        outImage->height = slice.height;
        outImage->valueMin = slice.valueMin;
        outImage->valueMax = slice.valueMax;
        outImage->values = std::move(slice.values);
        outImage->rgba = std::move(slice.rgba);
        outStats->columns = slice.width;
        outStats->tracesRead = slice.columnsRead;
        outStats->columnDistances = std::move(slice.distances);
        int missing = 0;
        std::vector<int> traces = std::move(slice.traceIndices);
        for (int t : traces)
          missing += (t < 0) ? 1 : 0;
        outStats->missingColumns = missing;
        outStats->uniqueTraces = static_cast<int>(traces.size()) - missing;
        task->reportBytes(slice.width * slice.height, slice.width * slice.height);
        return QString();
      }
      if (status.code == engine::StatusCode::Cancelled || task->cancelRequested())
        return QString();
      // 引擎路径失败回落逐道构建
    }

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

PaleoTask *SeismicTaskService::startWorkspaceTranscode(
    const QString &sgyPath,
    const QString &workspaceBase,
    std::function<void(bool, const QString &, const QString &)> onFinished)
{
  if (!taskService_)
  {
    if (onFinished)
      onFinished(false, QString(), QStringLiteral("PaleoTaskService not set"));
    return nullptr;
  }

  // Auto 约定：workspaceBase = SEG-Y 路径本身（产出 <sgy>.sf3c.meta + .sf3.sNNN）
  const QString base = workspaceBase.isEmpty() ? sgyPath : workspaceBase;
  const QString title = tr("转码地震工作区: %1").arg(QFileInfo(sgyPath).fileName());
  auto outBase = std::make_shared<QString>(base);

  auto work = [sgyPath, base](PaleoTask *task) -> QString {
    engine::CancelToken cancel;
    cancel.SetPredicate([task]() { return task->cancelRequested(); });
    auto progress = [task](const engine::TranscodeProgress &p) -> bool {
      if (task->cancelRequested())
        return false;
      task->reportBytes(p.chunksDone + p.chunksSkipped, p.chunksTotal);
      task->reportDetail(QCoreApplication::translate(
          "seismic::SeismicTaskService", "阶段 %1 · 块 %2/%3 · 已写 %4 MB")
              .arg(QString::fromStdString(p.phase))
              .arg(p.chunksDone)
              .arg(p.chunksTotal)
              .arg(p.bytesWritten / (1024 * 1024)));
      return true;
    };

    engine::TranscodeOptions options;
#ifdef SEISMIC_HAVE_ZSTD
    options.codec = engine::kCodecZstd;
#endif
    const engine::TranscodeResult result = engine::TranscodeSegyToWorkspace(
        std::filesystem::path(sgyPath.toStdString()),
        std::filesystem::path(base.toStdString()),
        options, &cancel, progress);

    if (!result.status.ok())
    {
      if (result.status.code == engine::StatusCode::Cancelled || task->cancelRequested())
        return QString();
      return QString::fromStdString(result.status.message);
    }
    return QString();
  };

  PaleoTask *task = taskService_->start(title, work);
  connect(task, &PaleoTask::finished, this, [task, outBase, onFinished]() {
    if (!onFinished)
      return;
    if (task->state() == PaleoTask::State::Succeeded)
      onFinished(true, *outBase, QString());
    else if (task->state() == PaleoTask::State::Cancelled)
      onFinished(false, *outBase, tr("转码已取消（工作区可续跑）"));
    else
      onFinished(false, *outBase, task->errorText());
  });
  return task;
}

} // namespace seismic
