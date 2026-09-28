// 层：数据
#include "services/seismictaskservice.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QTimer>

#include "Engine/PagedPipeline.h"
#include "Engine/PagedWorkspace.h"
#include "Engine/QuickOpen.h"
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

namespace {

// 条目键：路径 + 后端枚举（同一 .sgy 的 Auto 直读与显式 .sf3p 并存）。
QString entryKey(const QString &path, sdk::Backend backend)
{
  return path + QLatin1Char('|') + QString::number(static_cast<int>(backend));
}

// Slice2D → SgySliceImage 的字段平移（values/rgba 保有 NaN 语义）。
std::shared_ptr<SgySliceImage> toSliceImage(const engine::Slice2D &slice)
{
  auto image = std::make_shared<SgySliceImage>();
  image->width = slice.width;
  image->height = slice.height;
  image->valueMin = slice.valueMin;
  image->valueMax = slice.valueMax;
  image->values = slice.values;
  image->rgba = slice.rgba;
  return image;
}

SeismicBackendStatus statusFromDataset(const sdk::Dataset &dataset)
{
  SeismicBackendStatus status;
  status.ok = dataset.IsOpen();
  status.backendName = QString::fromLatin1(dataset.BackendName());
  status.fellBackToDirect = dataset.FellBackToDirect();
  status.quality = QString::fromStdString(dataset.QualityName());
  status.geometry = QString::fromStdString(dataset.GeometryDescription());
  status.lodLevels = dataset.LodLevelCount();
  status.activeLod = dataset.ActiveLodLevel();
  return status;
}

} // namespace

SeismicTaskService::~SeismicTaskService() = default;

std::shared_ptr<SeismicDatasetEntry> SeismicTaskService::datasetEntryFor(const QString &path,
                                                                         sdk::Backend backend)
{
  const QString key = entryKey(path, backend);
  {
    QMutexLocker lock(&datasetMutex_);
    auto it = datasetEntries_.find(key);
    if (it != datasetEntries_.end())
    {
      it.value()->lastUse = ++datasetClock_;
      return it.value();
    }
  }

  // 打开在 datasetMutex_ 内串行：Open 是有限 IO，不与条目使用锁嵌套。
  sdk::OpenOptions options;
  options.backend = backend;
  if (backend == sdk::Backend::Paged)
    options.progressiveLod = true; // 兄弟层级发现 + 从最粗层起步
  engine::Status openStatus;
  auto dataset = sdk::Dataset::Open(std::filesystem::path(path.toStdString()), options, openStatus);
  if (!dataset || !openStatus.ok())
    return nullptr;

  auto entry = std::make_shared<SeismicDatasetEntry>();
  entry->dataset = std::move(dataset);

  QMutexLocker lock(&datasetMutex_);
  entry->lastUse = ++datasetClock_;
  datasetEntries_.insert(key, entry);

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

void SeismicTaskService::invalidateDataset(const QString &path)
{
  QMutexLocker lock(&datasetMutex_);
  datasetEntries_.remove(entryKey(path, sdk::Backend::Auto));
  datasetEntries_.remove(entryKey(path, sdk::Backend::Paged));
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
    std::function<void(bool, std::shared_ptr<const SgySliceImage>, const QString &)> onFinished,
    const QString &pagedPath)
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

  // 1. 检查内存 LRU 缓存（显式 paged 通道不查直读缓存——两条通道语义独立）
  if (pagedPath.isEmpty())
  {
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
  }

  if (!taskService_)
  {
    if (onFinished)
      onFinished(false, nullptr, QStringLiteral("PaleoTaskService not set"));
    return nullptr;
  }

  const QString title = tr("提取地震切片 (%1: %2)").arg(QString::fromLatin1(typeStr)).arg(sliceIndex);
  auto outImage = std::make_shared<SgySliceImage>();

  // 引擎优先：sdk::Dataset。Auto 通道在工作区存在时用随机访问后端；
  // pagedPath 非空走显式 .sf3p（LOD 映射）。条目在 worker 内解析——
  // Dataset::Open 是有限 IO，不得占用 UI 线程。
  const QString sgyPath = QString::fromStdString(sgyio::ToUtf8Path(volume->Index()->path));
  const QPointer<SeismicTaskService> self(this);

  auto work = [self, volume, sgyPath, pagedPath, type, sliceIndex, outImage](PaleoTask *task) -> QString {
    if (self)
    {
      const auto entry = self->datasetEntryFor(pagedPath.isEmpty() ? sgyPath : pagedPath,
                                               pagedPath.isEmpty() ? sdk::Backend::Auto : sdk::Backend::Paged);
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
            status = entry->dataset->ReadTimeSlice(sliceIndex, slice, &cancel);
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
        if (!pagedPath.isEmpty())
          return QObject::tr("分页工作区读取失败：%1").arg(QString::fromStdString(status.message));
        // Auto 通道失败回落 volume 直读（同一份上游实现，保底）
      }
    }

    if (!pagedPath.isEmpty())
      return QObject::tr("分页工作区不可用：%1").arg(pagedPath);

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
  connect(task, &PaleoTask::finished, this, [this, task, key, pagedPath, outImage, onFinished]() {
    if (task->state() == PaleoTask::State::Succeeded)
    {
      if (pagedPath.isEmpty())
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
  const QPointer<SeismicTaskService> self(this);

  auto work = [self, volume, sgyPath, pathPoints, options, outImage, outStats](PaleoTask *task) -> QString {
    if (self)
    {
      const auto entry = self->datasetEntryFor(sgyPath, sdk::Backend::Auto);
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

PaleoTask *SeismicTaskService::startQuickOpen(
    const QString &sgyPath,
    int maxColumns,
    std::function<void(bool, const SeismicQuickPreview &)> onFinished)
{
  if (!taskService_)
  {
    if (onFinished)
    {
      SeismicQuickPreview failed;
      failed.error = QStringLiteral("PaleoTaskService not set");
      onFinished(false, failed);
    }
    return nullptr;
  }

  const QString title = tr("秒开预览 SEG-Y: %1").arg(QFileInfo(sgyPath).fileName());
  auto result = std::make_shared<SeismicQuickPreview>();

  auto work = [sgyPath, maxColumns, result](PaleoTask *task) -> QString {
    engine::CancelToken cancel;
    cancel.SetPredicate([task]() { return task->cancelRequested(); });
    const engine::QuickOpenResult quick = engine::QuickOpenSegyPreview(
        std::filesystem::path(sgyPath.toStdString()), maxColumns, &cancel);

    if (quick.status.code == engine::StatusCode::Cancelled || task->cancelRequested())
      return QString();
    if (!quick.status.ok())
      return QString::fromStdString(quick.status.message);

    result->ok = true;
    result->ruleVerified = quick.ruleVerified;
    result->summary = quick.ruleVerified
        ? QString::fromStdString(quick.verificationSummary)
        : QObject::tr("规则探测未通过（%1），走全量索引流")
              .arg(QString::fromStdString(quick.fallbackReason));
    result->traceCount = static_cast<qint64>(quick.metadata.traceCount);
    result->sampleCount = quick.metadata.sampleCount;
    result->sampleIntervalUs = quick.metadata.sampleIntervalUs;
    result->inlineMin = quick.metadata.inlineMin;
    result->inlineMax = quick.metadata.inlineMax;
    result->xlineMin = quick.metadata.xlineMin;
    result->xlineMax = quick.metadata.xlineMax;
    result->previewInline = quick.selectedInline;
    result->previewXline = quick.selectedXline;
    result->columnsRead = quick.columnsRead;
    result->columnsTotal = quick.columnsTotal;
    result->totalMs = quick.times.totalMs;
    if (quick.ruleVerified)
      result->preview = std::make_shared<SgySliceImage>(quick.preview);
    task->reportBytes(std::max(1, quick.columnsRead), std::max(1, quick.columnsTotal));
    return QString();
  };

  PaleoTask *task = taskService_->start(title, work);
  connect(task, &PaleoTask::finished, this, [task, result, onFinished]() {
    if (!onFinished)
      return;
    if (task->state() == PaleoTask::State::Succeeded)
      onFinished(true, *result);
    else
    {
      result->error = (task->state() == PaleoTask::State::Cancelled)
          ? tr("秒开预览已取消")
          : task->errorText();
      onFinished(false, *result);
    }
  });
  return task;
}

PaleoTask *SeismicTaskService::startVolumeLoad(
    const QString &sgyPath,
    std::function<void(bool, std::shared_ptr<SgyVolume>, const QString &)> onFinished)
{
  if (!taskService_)
  {
    if (onFinished)
      onFinished(false, nullptr, QStringLiteral("PaleoTaskService not set"));
    return nullptr;
  }

  const QString title = tr("加载地震体: %1").arg(QFileInfo(sgyPath).fileName());
  auto outVolume = std::make_shared<std::shared_ptr<SgyVolume>>();

  auto work = [sgyPath, outVolume](PaleoTask *task) -> QString {
    auto volume = std::make_shared<SgyVolume>();
    std::string err;
    auto progress = [task](int processed, int total) -> bool {
      if (task->cancelRequested())
        return false;
      task->reportBytes(processed, total);
      return true;
    };
    if (!volume->Load(std::filesystem::path(sgyPath.toStdString()), err, progress))
    {
      if (task->cancelRequested())
        return QString();
      return QString::fromStdString(err);
    }
    *outVolume = std::move(volume);
    return QString();
  };

  PaleoTask *task = taskService_->start(title, work);
  connect(task, &PaleoTask::finished, this, [task, outVolume, onFinished]() {
    if (!onFinished)
      return;
    if (task->state() == PaleoTask::State::Succeeded)
      onFinished(true, *outVolume, QString());
    else if (task->state() == PaleoTask::State::Cancelled)
      onFinished(false, nullptr, tr("地震体加载已取消"));
    else
      onFinished(false, nullptr, task->errorText());
  });
  return task;
}

PaleoTask *SeismicTaskService::startPagedTranscode(
    const QString &sgyPath,
    const QString &sf3pPath,
    bool buildLod,
    std::function<void(bool, const QString &, const QString &)> onFinished)
{
  if (!taskService_)
  {
    if (onFinished)
      onFinished(false, sf3pPath, QStringLiteral("PaleoTaskService not set"));
    return nullptr;
  }

  const QString title = tr("转码分页工作区: %1").arg(QFileInfo(sgyPath).fileName());
  auto outPath = std::make_shared<QString>(sf3pPath);

  auto work = [sgyPath, sf3pPath, buildLod](PaleoTask *task) -> QString {
    engine::CancelToken cancel;
    cancel.SetPredicate([task]() { return task->cancelRequested(); });

    engine::PagedPipelineOptions options;
    options.buildLod1 = buildLod;
    options.buildLod2 = buildLod;
#ifdef SEISMIC_HAVE_ZSTD
    options.codec = engine::kPagedCodecZstd;
#endif
    // 金字塔三阶段分别计总量；每阶段内 done/total 单调（阶段切换时进度条
    // 归零属预期，任务页以明细行显示当前阶段）。
    auto progress = [task](const engine::PagedPipelineProgress &p) -> bool {
      if (task->cancelRequested())
        return false;
      task->reportBytes(static_cast<qint64>(p.chunksDone + p.chunksSkipped),
                        static_cast<qint64>(std::max<std::uint64_t>(1, p.chunksTotal)));
      task->reportDetail(QCoreApplication::translate(
          "seismic::SeismicTaskService", "阶段 %1 · 块 %2/%3 · 已读道 %4")
              .arg(QString::fromStdString(p.phase))
              .arg(p.chunksDone + p.chunksSkipped)
              .arg(p.chunksTotal)
              .arg(p.tracesRead));
      return true;
    };

    const engine::PagedPyramidResult result = engine::BuildPagedPyramid(
        std::filesystem::path(sgyPath.toStdString()),
        std::filesystem::path(sf3pPath.toStdString()),
        options, &cancel, progress);

    const auto phaseError = [&result](const engine::PagedBuildResult &phase,
                                      const char *name) -> QString {
      if (phase.status.ok() || phase.reused)
        return QString();
      return QObject::tr("分页转码 %1 阶段失败：%2")
          .arg(QString::fromLatin1(name), QString::fromStdString(phase.status.message));
    };
    if (QString err = phaseError(result.l0, "L0"); !err.isEmpty())
    {
      if (result.l0.status.code == engine::StatusCode::Cancelled || task->cancelRequested())
        return QString();
      return err;
    }
    if (QString err = phaseError(result.l1, "L1"); !err.isEmpty())
    {
      if (result.l1.status.code == engine::StatusCode::Cancelled || task->cancelRequested())
        return QString();
      return err;
    }
    if (QString err = phaseError(result.l2, "L2"); !err.isEmpty())
    {
      if (result.l2.status.code == engine::StatusCode::Cancelled || task->cancelRequested())
        return QString();
      return err;
    }
    return QString();
  };

  PaleoTask *task = taskService_->start(title, work);
  connect(task, &PaleoTask::finished, this, [this, task, outPath, onFinished]() {
    if (!onFinished)
      return;
    if (task->state() == PaleoTask::State::Succeeded)
    {
      invalidateDataset(*outPath); // 热切换：下次读取按磁盘现状重开
      onFinished(true, *outPath, QString());
    }
    else if (task->state() == PaleoTask::State::Cancelled)
      onFinished(false, *outPath, tr("分页转码已取消（可续跑）"));
    else
      onFinished(false, *outPath, task->errorText());
  });
  return task;
}

PaleoTask *SeismicTaskService::startTimeSliceTiled(
    const QString &sf3pPath,
    int sampleIndex,
    int tileSize,
    int focusInline,
    int focusXline,
    std::function<void(bool, std::shared_ptr<const SgySliceImage>, const QString &)> onFinished)
{
  if (!taskService_)
  {
    if (onFinished)
      onFinished(false, nullptr, QStringLiteral("PaleoTaskService not set"));
    return nullptr;
  }

  const QString title = tr("瓦片渐进时间片 (采样 %1)").arg(sampleIndex);
  auto outImage = std::make_shared<SgySliceImage>();
  const QPointer<SeismicTaskService> self(this);

  auto work = [self, sf3pPath, sampleIndex, tileSize, focusInline, focusXline, outImage](
                  PaleoTask *task) -> QString {
    if (!self)
      return QString();
    const auto entry = self->datasetEntryFor(sf3pPath, sdk::Backend::Paged);
    if (!entry || !entry->dataset)
      return QObject::tr("分页工作区不可用：%1").arg(sf3pPath);

    engine::CancelToken cancel;
    cancel.SetPredicate([task]() { return task->cancelRequested(); });

    // 瓦片在 worker 线程回调里产生：排队投递回服务所在线程发射 timeSliceTileReady；
    // 服务已析构则静默丢弃（UI 侧另有世代号过滤陈旧瓦片）。
    auto publish = [self, task, sampleIndex](int x, int y, engine::Slice2D &&tile,
                                             int completed, int total) -> bool {
      if (task->cancelRequested())
        return false;
      task->reportBytes(completed, std::max(1, total));
      SeismicTimeTile payload;
      payload.sampleIndex = sampleIndex;
      payload.x = x;
      payload.y = y;
      payload.completed = completed;
      payload.total = total;
      payload.image = toSliceImage(tile);
      QMetaObject::invokeMethod(self.data(), [self, payload]() {
        if (self)
          emit self->timeSliceTileReady(payload);
      }, Qt::QueuedConnection);
      return true;
    };

    engine::Slice2D full;
    engine::Status status;
    {
      QMutexLocker lock(&entry->mutex);
      status = entry->dataset->ReadTimeSliceTiled(sampleIndex, tileSize, focusInline,
                                                  focusXline, publish, full, &cancel);
    }
    if (status.ok())
    {
      outImage->width = full.width;
      outImage->height = full.height;
      outImage->valueMin = full.valueMin;
      outImage->valueMax = full.valueMax;
      outImage->values = std::move(full.values);
      outImage->rgba = std::move(full.rgba);
      return QString();
    }
    if (status.code == engine::StatusCode::Cancelled || task->cancelRequested())
      return QString();
    return QObject::tr("瓦片时间片读取失败：%1").arg(QString::fromStdString(status.message));
  };

  PaleoTask *task = taskService_->start(title, work);
  connect(task, &PaleoTask::finished, this, [task, outImage, onFinished]() {
    if (!onFinished)
      return;
    if (task->state() == PaleoTask::State::Succeeded)
      onFinished(true, outImage, QString());
    else if (task->state() == PaleoTask::State::Cancelled)
      onFinished(false, nullptr, tr("瓦片时间片已取消"));
    else
      onFinished(false, nullptr, task->errorText());
  });
  return task;
}

PaleoTask *SeismicTaskService::startVoxelWindow(
    const QString &datasetPath,
    const engine::VoxelWindowRequest &request,
    std::function<void(bool, const engine::VoxelWindow &, const QString &)> onFinished)
{
  if (!taskService_)
  {
    if (onFinished)
      onFinished(false, engine::VoxelWindow{}, QStringLiteral("PaleoTaskService not set"));
    return nullptr;
  }

  const QString title = tr("读取三维体素窗口 (%1×%2×%3)")
                            .arg(request.inlineCount).arg(request.xlineCount).arg(request.sampleCount);
  auto outWindow = std::make_shared<engine::VoxelWindow>();
  const QPointer<SeismicTaskService> self(this);
  const bool paged = datasetPath.endsWith(QStringLiteral(".sf3p"), Qt::CaseInsensitive);

  auto work = [self, datasetPath, paged, request, outWindow](PaleoTask *task) -> QString {
    if (!self)
      return QString();
    const auto entry = self->datasetEntryFor(
        datasetPath, paged ? sdk::Backend::Paged : sdk::Backend::Auto);
    if (!entry || !entry->dataset)
      return QObject::tr("数据集不可用：%1").arg(datasetPath);

    engine::CancelToken cancel;
    cancel.SetPredicate([task]() { return task->cancelRequested(); });
    engine::Status status;
    {
      QMutexLocker lock(&entry->mutex);
      status = entry->dataset->ReadVoxelWindow(request, *outWindow, &cancel);
    }
    if (status.ok())
    {
      task->reportBytes(static_cast<qint64>(outWindow->values.size()),
                        static_cast<qint64>(outWindow->values.size()));
      return QString();
    }
    if (status.code == engine::StatusCode::Cancelled || task->cancelRequested())
      return QString();
    return QObject::tr("体素窗口读取失败：%1").arg(QString::fromStdString(status.message));
  };

  PaleoTask *task = taskService_->start(title, work);
  connect(task, &PaleoTask::finished, this, [task, outWindow, onFinished]() {
    if (!onFinished)
      return;
    if (task->state() == PaleoTask::State::Succeeded)
      onFinished(true, *outWindow, QString());
    else if (task->state() == PaleoTask::State::Cancelled)
      onFinished(false, engine::VoxelWindow{}, tr("体素窗口读取已取消"));
    else
      onFinished(false, engine::VoxelWindow{}, task->errorText());
  });
  return task;
}

PaleoTask *SeismicTaskService::startPagedOpen(
    const QString &sf3pPath,
    std::function<void(bool, const SeismicBackendStatus &, const QString &)> onFinished)
{
  if (!taskService_)
  {
    if (onFinished)
      onFinished(false, SeismicBackendStatus{}, QStringLiteral("PaleoTaskService not set"));
    return nullptr;
  }

  const QString title = tr("打开分页工作区: %1").arg(QFileInfo(sf3pPath).fileName());
  auto status = std::make_shared<SeismicBackendStatus>();
  const QPointer<SeismicTaskService> self(this);

  auto work = [self, sf3pPath, status](PaleoTask *task) -> QString {
    if (!self)
      return QString();
    const auto entry = self->datasetEntryFor(sf3pPath, sdk::Backend::Paged);
    if (!entry || !entry->dataset)
      return QObject::tr("分页工作区不可用：%1").arg(sf3pPath);
    QMutexLocker lock(&entry->mutex);
    *status = statusFromDataset(*entry->dataset);
    return QString();
  };

  PaleoTask *task = taskService_->start(title, work);
  connect(task, &PaleoTask::finished, this, [task, status, onFinished]() {
    if (!onFinished)
      return;
    if (task->state() == PaleoTask::State::Succeeded)
      onFinished(true, *status, QString());
    else if (task->state() == PaleoTask::State::Cancelled)
      onFinished(false, *status, tr("打开分页工作区已取消"));
    else
      onFinished(false, *status, task->errorText());
  });
  return task;
}

PaleoTask *SeismicTaskService::startLodSwitch(
    const QString &sf3pPath,
    int lodLevel,
    std::function<void(bool, const QString &, const QString &)> onFinished)
{
  if (!taskService_)
  {
    if (onFinished)
      onFinished(false, QString(), QStringLiteral("PaleoTaskService not set"));
    return nullptr;
  }

  const QString title = tr("切换 LOD 层级 %1").arg(lodLevel);
  auto quality = std::make_shared<QString>();
  const QPointer<SeismicTaskService> self(this);

  auto work = [self, sf3pPath, lodLevel, quality](PaleoTask *task) -> QString {
    if (!self)
      return QString();
    const auto entry = self->datasetEntryFor(sf3pPath, sdk::Backend::Paged);
    if (!entry || !entry->dataset)
      return QObject::tr("分页工作区不可用：%1").arg(sf3pPath);
    QMutexLocker lock(&entry->mutex);
    engine::Status status = entry->dataset->SetActiveLod(lodLevel);
    if (!status.ok())
      return QObject::tr("LOD 切换失败：%1").arg(QString::fromStdString(status.message));
    *quality = QString::fromStdString(entry->dataset->QualityName());
    return QString();
  };

  PaleoTask *task = taskService_->start(title, work);
  connect(task, &PaleoTask::finished, this, [task, quality, onFinished]() {
    if (!onFinished)
      return;
    if (task->state() == PaleoTask::State::Succeeded)
      onFinished(true, *quality, QString());
    else if (task->state() == PaleoTask::State::Cancelled)
      onFinished(false, QString(), tr("LOD 切换已取消"));
    else
      onFinished(false, QString(), task->errorText());
  });
  return task;
}

PaleoTask *SeismicTaskService::startBackendProbe(
    const QString &sgyPath,
    std::function<void(bool, const SeismicBackendStatus &, const QString &)> onFinished)
{
  if (!taskService_)
  {
    if (onFinished)
      onFinished(false, SeismicBackendStatus{}, QStringLiteral("PaleoTaskService not set"));
    return nullptr;
  }

  const QString title = tr("探测地震后端: %1").arg(QFileInfo(sgyPath).fileName());
  auto status = std::make_shared<SeismicBackendStatus>();

  // 独立探测（不经条目缓存）：反映此刻磁盘现状——转码完成后的热切换提示。
  auto work = [sgyPath, status](PaleoTask *task) -> QString {
    Q_UNUSED(task);
    sdk::OpenOptions options; // Auto：有 .sf3c 用工作区，否则直读
    engine::Status openStatus;
    auto dataset = sdk::Dataset::Open(std::filesystem::path(sgyPath.toStdString()),
                                      options, openStatus);
    if (!dataset || !openStatus.ok())
      return QObject::tr("打开数据集失败：%1").arg(QString::fromStdString(openStatus.message));
    *status = statusFromDataset(*dataset);
    return QString();
  };

  PaleoTask *task = taskService_->start(title, work);
  connect(task, &PaleoTask::finished, this, [task, status, onFinished]() {
    if (!onFinished)
      return;
    if (task->state() == PaleoTask::State::Succeeded)
      onFinished(true, *status, QString());
    else if (task->state() == PaleoTask::State::Cancelled)
      onFinished(false, *status, tr("后端探测已取消"));
    else
      onFinished(false, *status, task->errorText());
  });
  return task;
}

} // namespace seismic
