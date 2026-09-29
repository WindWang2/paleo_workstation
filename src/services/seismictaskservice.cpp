// 层：数据
#include "services/seismictaskservice.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <cmath>

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

// ---- D1.1 分阶段聚合进度 ----------------------------------------------------
// 把引擎的分阶段 done/total 聚合成单调全局百分比喂给 PaleoTask：
// reportBytes 的字节速率 ETA 与进度条都直接受益（不再阶段归零）。
class TranscodePhaseTracker
{
public:
  void addPhase(const QString &name, double weight)
  {
    m_weights[name] = weight;
    m_fracs[name] = 0.0;
  }

  void setFrac(const QString &name, double frac)
  {
    auto it = m_fracs.find(name);
    if (it != m_fracs.end())
      it.value() = std::clamp(frac, 0.0, 1.0);
  }

  // 某阶段整体完成（例如扫描结束进入写片）。
  void finishPhase(const QString &name) { setFrac(name, 1.0); }

  double overallFrac() const
  {
    double total = 0.0, weightSum = 0.0;
    for (auto it = m_weights.constBegin(); it != m_weights.constEnd(); ++it)
    {
      weightSum += it.value();
      total += it.value() * m_fracs.value(it.key(), 0.0);
    }
    return weightSum > 0.0 ? total / weightSum : 0.0;
  }

private:
  QMap<QString, double> m_weights;
  QMap<QString, double> m_fracs;
};

// 二进制头轻量估计体字节（3600B 头读一次；不建索引、不扫道头）。
// 估计失真只影响 LOD 层数规划（D1.9），不影响转码正确性。
qint64 estimateVolumeBytesFromSgy(const QString &sgyPath)
{
  QFile file(sgyPath);
  if (!file.open(QIODevice::ReadOnly))
    return 0;
  QByteArray header = file.read(3600);
  if (header.size() < 3600)
    return 0;
  const auto be16 = [&](int offset) -> int {
    return (quint8(header[offset]) << 8) | quint8(header[offset + 1]);
  };
  const int samples = be16(3220);
  const int format = be16(3224);
  int bytesPerSample = 4; // 1=IBM 5=IEEE
  if (format == 2 || format == 3)
    bytesPerSample = 2;   // int16 / int32(上头是 4 字节？SEG-Y 3=INT32 —— 按字宽表处理)
  else if (format == 8)
    bytesPerSample = 1;   // int8
  if (samples <= 0 || bytesPerSample <= 0)
    return 0;
  const qint64 bytesPerTrace = 240 + qint64(samples) * bytesPerSample;
  const qint64 traceCount = (file.size() - 3600) / bytesPerTrace;
  return traceCount * qint64(samples) * 4;
}

// D1.9 自适应金字塔层数：按体字节规划（L1=4x4x1, L2=8x8x1, L3=16x16x1）。
QStringList planPagedLodLevels(qint64 volumeBytes, bool userWantsLod)
{
  if (!userWantsLod)
    return {};
  if (volumeBytes <= 0)
    return {QStringLiteral("L1"), QStringLiteral("L2")}; // 未知体量走保守生产档
  constexpr qint64 kMiB = 1024ll * 1024ll;
  if (volumeBytes < 64 * kMiB)
    return {};                        // 小体量：LOD 收益不抵构建成本
  if (volumeBytes < 2ll * 1024 * kMiB)
    return {QStringLiteral("L1")};
  if (volumeBytes < 16ll * 1024 * kMiB)
    return {QStringLiteral("L1"), QStringLiteral("L2")};
  return {QStringLiteral("L1"), QStringLiteral("L2"), QStringLiteral("L3")};
}

// D1.10 结构化转码日志：PALEO-SEISMIC-TRANSCODE 前缀 + 单行紧凑 JSON。
void logTranscodeEvent(const QJsonObject &event)
{
  qInfo().noquote() << QStringLiteral("PALEO-SEISMIC-TRANSCODE ")
                    << QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact));
}

QStringList damagedListFromEngine(const std::vector<std::pair<int, int>> &samples)
{
  QStringList out;
  out.reserve(int(samples.size()));
  for (const auto &p : samples)
    out << QStringLiteral("%1/%2").arg(p.first).arg(p.second);
  return out;
}

void fillReportFromSf3c(SeismicTranscodeReport &report,
                        const engine::TranscodeResult &result)
{
  report.kind = QStringLiteral("sf3c");
  report.chunksWritten = qint64(result.chunksWritten);
  report.chunksSkipped = qint64(result.chunksSkipped);
  report.chunksTotal = qint64(result.info.ChunkCount());
  report.tracesRead = qint64(result.tracesRead);
  report.tracesTotal = qint64(result.info.inlines) * result.info.xlines;
  report.missingTraces = qint64(result.missingTraceCount);
  report.damagedTraces = qint64(result.damagedTraceCount);
  report.damagedSample = damagedListFromEngine(result.damagedTraceSample);
  if (!std::isnan(result.valueMin) && !std::isnan(result.valueMax))
  {
    report.valueMin = result.valueMin;
    report.valueMax = result.valueMax;
    report.validValues = true;
  }
  report.bytesWritten = qint64(result.bytesWritten);
  report.elapsedSeconds = result.elapsedSeconds;
  report.resumed = result.chunksSkipped > 0;
  report.ok = result.status.ok();
  report.cancelled = result.status.code == engine::StatusCode::Cancelled;
}

void fillReportFromPaged(SeismicTranscodeReport &report,
                         const engine::PagedPyramidResult &pyramid,
                         const QStringList &levels,
                         const engine::PagedBuildResult *l3)
{
  report.kind = QStringLiteral("sf3p");
  const engine::PagedBuildResult &l0 = pyramid.l0;
  report.chunksWritten = qint64(l0.chunksWritten);
  report.chunksSkipped = qint64(l0.chunksSkipped);
  report.chunksTotal = qint64(l0.info.ChunkCount());
  report.tracesRead = qint64(l0.tracesRead);
  report.tracesTotal = qint64(l0.info.inlineAxis.count) * l0.info.xlineAxis.count;
  report.missingTraces = qint64(l0.missingTraceCount);
  report.damagedTraces = qint64(l0.damagedTraceCount);
  report.damagedSample = damagedListFromEngine(l0.damagedTraceSample);
  if (!std::isnan(l0.valueMin) && !std::isnan(l0.valueMax))
  {
    report.valueMin = l0.valueMin;
    report.valueMax = l0.valueMax;
    report.validValues = true;
  }
  report.bytesWritten = qint64(l0.bytesWritten);
  report.elapsedSeconds = l0.elapsedSeconds;
  report.resumed = l0.chunksSkipped > 0 || l0.reused;
  report.lodLevels = levels;
  report.ok = pyramid.status.ok() && (!l3 || l3->status.ok() || l3->reused);
  report.cancelled = l0.status.code == engine::StatusCode::Cancelled ||
                     (l3 && l3->status.code == engine::StatusCode::Cancelled);
  if (l3)
  {
    report.bytesWritten += qint64(l3->bytesWritten);
    report.elapsedSeconds += l3->elapsedSeconds;
  }
}

// D1.4/D1.10：报告 → JSON 对象（终态日志与 toJsonLine 共用字段集）。
QJsonObject reportToJson(const SeismicTranscodeReport &r)
{
  QJsonObject o;
  o.insert("kind", r.kind);
  o.insert("output", r.output);
  o.insert("ok", r.ok);
  o.insert("cancelled", r.cancelled);
  o.insert("resumed", r.resumed);
  o.insert("chunksWritten", double(r.chunksWritten));
  o.insert("chunksSkipped", double(r.chunksSkipped));
  o.insert("chunksTotal", double(r.chunksTotal));
  o.insert("tracesRead", double(r.tracesRead));
  o.insert("tracesTotal", double(r.tracesTotal));
  o.insert("missingTraces", double(r.missingTraces));
  o.insert("damagedTraces", double(r.damagedTraces));
  o.insert("coverage", r.coverage());
  o.insert("droppedRatio", r.droppedRatio());
  if (r.validValues)
  {
    o.insert("valueMin", r.valueMin);
    o.insert("valueMax", r.valueMax);
  }
  o.insert("bytesWritten", double(r.bytesWritten));
  o.insert("elapsedSeconds", r.elapsedSeconds);
  if (!r.lodLevels.isEmpty())
    o.insert("lodLevels", QJsonArray::fromStringList(r.lodLevels));
  if (!r.damagedSample.isEmpty())
    o.insert("damagedSample", QJsonArray::fromStringList(r.damagedSample));
  return o;
}

} // namespace

// 条目注册表：以 shared_ptr 持有（服务与各 worker 各持引用）。worker 在
// 服务析构后仍可能排队在 registry->mutex 上——注册表必须活到最后一个
// 使用者退出，否则出现「锁在等待者手中被销毁」的析构竞态（挂死/UAF）。
struct SeismicDatasetRegistry
{
  QMutex mutex;
  QHash<QString, std::shared_ptr<SeismicDatasetEntry>> entries;
  quint64 clock = 0;
};

SeismicTaskService::~SeismicTaskService() = default;

// 按需打开并缓存条目。registry 以值参 shared_ptr 传入：worker 与服务共用，
// 生命周期自动延伸过任何在途 worker。
static std::shared_ptr<SeismicDatasetEntry> datasetEntryFor(
    const std::shared_ptr<SeismicDatasetRegistry> &registry,
    const QString &path, sdk::Backend backend)
{
  const QString key = entryKey(path, backend);
  {
    QMutexLocker lock(&registry->mutex);
    auto it = registry->entries.find(key);
    if (it != registry->entries.end())
    {
      it.value()->lastUse = ++registry->clock;
      return it.value();
    }
  }

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

  QMutexLocker lock(&registry->mutex);
  entry->lastUse = ++registry->clock;
  registry->entries.insert(key, entry);

  // 有界缓存：超出 8 个时淘汰最久未用
  while (registry->entries.size() > 8)
  {
    QString oldestKey;
    quint64 oldestUse = ~0ull;
    for (auto eit = registry->entries.begin(); eit != registry->entries.end(); ++eit)
    {
      if (eit.value()->lastUse < oldestUse)
      {
        oldestUse = eit.value()->lastUse;
        oldestKey = eit.key();
      }
    }
    if (oldestKey.isEmpty())
      break;
    registry->entries.remove(oldestKey);
  }
  return entry;
}

void SeismicTaskService::invalidateDataset(const QString &path)
{
  QMutexLocker lock(&registry_->mutex);
  registry_->entries.remove(entryKey(path, sdk::Backend::Auto));
  registry_->entries.remove(entryKey(path, sdk::Backend::Paged));
}

SeismicTaskService::SeismicTaskService(PaleoTaskService *taskService,
                                       std::size_t dataCacheBudgetMb,
                                       QObject *parent)
  : QObject(parent),
    taskService_(taskService),
    dataCache_(dataCacheBudgetMb * 1024ull * 1024ull),
    registry_(std::make_shared<SeismicDatasetRegistry>())
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
  const auto registry = registry_;

  auto work = [registry, volume, sgyPath, pagedPath, type, sliceIndex, outImage](PaleoTask *task) -> QString {
    const auto entry = datasetEntryFor(registry, pagedPath.isEmpty() ? sgyPath : pagedPath,
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
  const auto registry = registry_;

  auto work = [registry, volume, sgyPath, pathPoints, options, outImage, outStats](PaleoTask *task) -> QString {
    const auto entry = datasetEntryFor(registry, sgyPath, sdk::Backend::Auto);
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
  return startWorkspaceTranscodeDetailed(sgyPath, workspaceBase, onFinished, {});
}

PaleoTask *SeismicTaskService::startWorkspaceTranscodeDetailed(
    const QString &sgyPath,
    const QString &workspaceBase,
    std::function<void(bool, const QString &, const QString &)> onFinished,
    std::function<void(const SeismicTranscodeReport &)> onReport)
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
  auto outResult = std::make_shared<engine::TranscodeResult>();
  auto engineRan = std::make_shared<bool>(false);

  // D1.7 同输出互斥：在途转码未终态前拒绝第二次写同一目标
  if (activeTranscodeOutputs_.contains(base))
  {
    const QString error = tr("该工作区已有转码任务进行中（%1）").arg(base);
    logTranscodeEvent({{"event", "rejected"},
                       {"kind", "sf3c"},
                       {"output", base},
                       {"reason", "concurrent-transcode"}});
    if (onFinished)
      onFinished(false, base, error);
    return nullptr;
  }
  activeTranscodeOutputs_.insert(base);

  auto work = [sgyPath, base, outResult, engineRan](PaleoTask *task) -> QString {
    engine::CancelToken cancel;
    cancel.SetPredicate([task]() { return task->cancelRequested(); });

    // D1.1：扫描/写片/收尾加权聚合（扫描含索引/续跑探测，实测 ~3% 耗时）
    TranscodePhaseTracker tracker;
    tracker.addPhase(QStringLiteral("scanning"), 0.04);
    tracker.addPhase(QStringLiteral("transcoding"), 0.92);
    tracker.addPhase(QStringLiteral("finalizing"), 0.04);

    auto progress = [task, &tracker](const engine::TranscodeProgress &p) -> bool {
      if (task->cancelRequested())
        return false;
      const QString phase = QString::fromStdString(p.phase);
      if (phase == QLatin1String("scanning"))
      {
        tracker.setFrac(QStringLiteral("scanning"), 0.5);
      }
      else if (phase == QLatin1String("transcoding"))
      {
        tracker.finishPhase(QStringLiteral("scanning"));
        const double frac = p.chunksTotal > 0
            ? double(p.chunksDone + p.chunksSkipped) / double(p.chunksTotal)
            : 0.0;
        tracker.setFrac(QStringLiteral("transcoding"), frac);
      }
      else if (phase == QLatin1String("finalizing"))
      {
        tracker.finishPhase(QStringLiteral("transcoding"));
        tracker.setFrac(QStringLiteral("finalizing"), 0.5);
      }
      constexpr qint64 kUnits = 100000;
      task->reportBytes(qint64(tracker.overallFrac() * kUnits), kUnits);
      task->reportDetail(tr("阶段 %1 · 块 %2/%3 · 跳过 %4 · 总体 %5%")
                             .arg(phase)
                             .arg(p.chunksDone + p.chunksSkipped)
                             .arg(p.chunksTotal)
                             .arg(p.chunksSkipped)
                             .arg(int(tracker.overallFrac() * 100)));
      return true;
    };

    engine::TranscodeOptions options;
    options.writerThreads = 4; // D1.3：并行分片编码池（≤4；raw 编解码自动退单线程）
#ifdef SEISMIC_HAVE_ZSTD
    options.codec = engine::kCodecZstd;
#endif
    *outResult = engine::TranscodeSegyToWorkspace(
        std::filesystem::path(sgyPath.toStdString()),
        std::filesystem::path(base.toStdString()),
        options, &cancel, progress);
    *engineRan = true;

    if (!outResult->status.ok())
    {
      if (outResult->status.code == engine::StatusCode::Cancelled || task->cancelRequested())
        return QString();
      return QString::fromStdString(outResult->status.message);
    }
    return QString();
  };

  PaleoTask *task = taskService_->start(title, work);
  if (!task)
  {
    activeTranscodeOutputs_.remove(base);
    if (onFinished)
      onFinished(false, base, tr("任务提交失败"));
    return nullptr;
  }
  logTranscodeEvent({{"event", "start"},
                     {"kind", "sf3c"},
                     {"source", sgyPath},
                     {"output", base},
                     {"writerThreads", 4}});

  connect(task, &PaleoTask::finished, this, [this, task, outBase, outResult, engineRan, onFinished, onReport]() {
    activeTranscodeOutputs_.remove(*outBase);
    if (!onFinished && !onReport)
      return;

    const bool succeeded = task->state() == PaleoTask::State::Succeeded;
    const bool cancelled = task->state() == PaleoTask::State::Cancelled;

    SeismicTranscodeReport report;
    report.output = *outBase;
    report.ok = succeeded;
    report.cancelled = cancelled;
    if (*engineRan)
      fillReportFromSf3c(report, *outResult); // 成功/取消/引擎失败都带质量证据
    report.ok = succeeded;                    // 终态由任务状态定夺
    report.cancelled = cancelled;

    if (succeeded)
    {
      invalidateDataset(*outBase); // 热切换：下次读取按磁盘现状重开
      if (onFinished)
        onFinished(true, *outBase, QString());
    }
    else if (cancelled)
    {
      if (onFinished)
        onFinished(false, *outBase, tr("转码已取消（工作区可续跑）"));
    }
    else if (onFinished)
    {
      onFinished(false, *outBase, task->errorText());
    }

    // D1.10 结构化终态日志（成功/取消/失败都记）
    QJsonObject event = reportToJson(report);
    event.insert("event", succeeded ? "finished" : (cancelled ? "cancelled" : "failed"));
    if (!succeeded && !cancelled)
      event.insert("error", task->errorText());
    logTranscodeEvent(event);
    if (onReport)
      onReport(report);
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
  // 旧 API 语义保持不变：buildLod=true 恒建 L1+L2（自适应只属于 Detailed）
  return startPagedTranscodeImpl(sgyPath, sf3pPath, buildLod ? 1 : 0, onFinished, {});
}

PaleoTask *SeismicTaskService::startPagedTranscodeDetailed(
    const QString &sgyPath,
    const QString &sf3pPath,
    bool buildLod,
    std::function<void(bool, const QString &, const QString &)> onFinished,
    std::function<void(const SeismicTranscodeReport &)> onReport)
{
  // buildLod=true → 按体量自适应层数（D1.9）；false → 不建
  return startPagedTranscodeImpl(sgyPath, sf3pPath, buildLod ? 2 : 0, onFinished, onReport);
}

PaleoTask *SeismicTaskService::startPagedTranscodeImpl(
    const QString &sgyPath,
    const QString &sf3pPath,
    int lodMode,
    std::function<void(bool, const QString &, const QString &)> onFinished,
    std::function<void(const SeismicTranscodeReport &)> onReport)
{
  if (!taskService_)
  {
    if (onFinished)
      onFinished(false, sf3pPath, QStringLiteral("PaleoTaskService not set"));
    return nullptr;
  }

  const QString title = tr("转码分页工作区: %1").arg(QFileInfo(sgyPath).fileName());
  auto outPath = std::make_shared<QString>(sf3pPath);
  auto outPyramid = std::make_shared<engine::PagedPyramidResult>();
  auto outL3 = std::make_shared<engine::PagedBuildResult>();
  auto outLevels = std::make_shared<QStringList>();
  auto engineRan = std::make_shared<bool>(false);

  // D1.7 同输出互斥
  if (activeTranscodeOutputs_.contains(sf3pPath))
  {
    const QString error = tr("该分页工作区已有转码任务进行中（%1）").arg(sf3pPath);
    logTranscodeEvent({{"event", "rejected"},
                       {"kind", "sf3p"},
                       {"output", sf3pPath},
                       {"reason", "concurrent-transcode"}});
    if (onFinished)
      onFinished(false, sf3pPath, error);
    return nullptr;
  }
  activeTranscodeOutputs_.insert(sf3pPath);

  // D1.9 金字塔层数自适应体量（二进制头轻量估计，失真只影响层数选择）。
  // lodMode: 0=无 LOD；1=经典 L1+L2（旧 API 语义）；2=自适应。
  QStringList plannedLevels;
  if (lodMode == 1)
    plannedLevels = {QStringLiteral("L1"), QStringLiteral("L2")};
  else if (lodMode == 2)
    plannedLevels = planPagedLodLevels(estimateVolumeBytesFromSgy(sgyPath), true);

  auto work = [sgyPath, sf3pPath, plannedLevels, outPyramid, outL3, outLevels, engineRan](
                  PaleoTask *task) -> QString {
    engine::CancelToken cancel;
    cancel.SetPredicate([task]() { return task->cancelRequested(); });

    // D1.1：阶段权重按各层字节数（L0=1, L1=1/16, L2=1/64, L3=1/256）归一
    TranscodePhaseTracker tracker;
    tracker.addPhase(QStringLiteral("l0-transcode"), 1.0);
    if (plannedLevels.contains(QLatin1String("L1")))
      tracker.addPhase(QStringLiteral("l1-build"), 1.0 / 16.0);
    if (plannedLevels.contains(QLatin1String("L2")))
      tracker.addPhase(QStringLiteral("l2-build"), 1.0 / 64.0);
    if (plannedLevels.contains(QLatin1String("L3")))
      tracker.addPhase(QStringLiteral("l3-build"), 1.0 / 256.0);
    tracker.addPhase(QStringLiteral("finalizing"), 0.01);

    auto progress = [task, &tracker](const engine::PagedPipelineProgress &p) -> bool {
      if (task->cancelRequested())
        return false;
      const QString phase = QString::fromStdString(p.phase);
      const double frac = p.chunksTotal > 0
          ? double(p.chunksDone + p.chunksSkipped) / double(p.chunksTotal)
          : 0.0;
      if (phase != QLatin1String("finalizing"))
        tracker.setFrac(phase, frac);
      else
      {
        // 收尾阶段：把先前所有构建阶段视作完成
        tracker.finishPhase(QStringLiteral("l0-transcode"));
        tracker.finishPhase(QStringLiteral("l1-build"));
        tracker.finishPhase(QStringLiteral("l2-build"));
        tracker.finishPhase(QStringLiteral("l3-build"));
        tracker.setFrac(QStringLiteral("finalizing"), 0.5);
      }
      constexpr qint64 kUnits = 100000;
      task->reportBytes(qint64(tracker.overallFrac() * kUnits), kUnits);
      task->reportDetail(tr("阶段 %1 · 块 %2/%3 · 已读道 %4 · 总体 %5%")
                             .arg(phase)
                             .arg(p.chunksDone + p.chunksSkipped)
                             .arg(p.chunksTotal)
                             .arg(p.tracesRead)
                             .arg(int(tracker.overallFrac() * 100)));
      return true;
    };

    engine::PagedPipelineOptions options;
    options.buildLod1 = plannedLevels.contains(QLatin1String("L1"));
    options.buildLod2 = plannedLevels.contains(QLatin1String("L2"));
#ifdef SEISMIC_HAVE_ZSTD
    options.codec = engine::kPagedCodecZstd;
#endif

    const std::filesystem::path l0(sf3pPath.toStdString());
    *outPyramid = engine::BuildPagedPyramid(
        std::filesystem::path(sgyPath.toStdString()), l0, options, &cancel, progress);
    *engineRan = true;

    // D1.9 L3（16x16x1，超大体量档）：直接从 L0 构建
    if (plannedLevels.contains(QLatin1String("L3")) &&
        outPyramid->l0.status.ok())
    {
      *outL3 = engine::BuildPagedLodFromL0(
          l0, engine::PagedLodPath(l0, 3), 3, 16, 16, 1, engine::LodMethod::Average,
          options, &cancel, progress);
    }

    // 计划外的历史兄弟层级清理（旧计划留下的 L1/L2/L3 不再是事实）
    for (int level = 1; level <= 3; ++level)
    {
      const QString tag = QStringLiteral("L%1").arg(level);
      if (plannedLevels.contains(tag))
        continue;
      std::error_code ec;
      std::filesystem::remove(engine::PagedLodPath(l0, level), ec);
    }

    const auto phaseError = [](const engine::PagedBuildResult &phase,
                               const char *name) -> QString {
      if (phase.status.ok() || phase.reused)
        return QString();
      return QObject::tr("分页转码 %1 阶段失败：%2")
          .arg(QString::fromLatin1(name), QString::fromStdString(phase.status.message));
    };
    if (QString err = phaseError(outPyramid->l0, "L0"); !err.isEmpty())
    {
      if (outPyramid->l0.status.code == engine::StatusCode::Cancelled || task->cancelRequested())
        return QString();
      return err;
    }
    if (QString err = phaseError(outPyramid->l1, "L1"); !err.isEmpty())
    {
      if (outPyramid->l1.status.code == engine::StatusCode::Cancelled || task->cancelRequested())
        return QString();
      return err;
    }
    if (QString err = phaseError(outPyramid->l2, "L2"); !err.isEmpty())
    {
      if (outPyramid->l2.status.code == engine::StatusCode::Cancelled || task->cancelRequested())
        return QString();
      return err;
    }
    if (plannedLevels.contains(QLatin1String("L3")))
    {
      if (QString err = phaseError(*outL3, "L3"); !err.isEmpty())
      {
        if (outL3->status.code == engine::StatusCode::Cancelled || task->cancelRequested())
          return QString();
        return err;
      }
    }
    *outLevels = plannedLevels;
    return QString();
  };

  PaleoTask *task = taskService_->start(title, work);
  if (!task)
  {
    activeTranscodeOutputs_.remove(sf3pPath);
    if (onFinished)
      onFinished(false, sf3pPath, tr("任务提交失败"));
    return nullptr;
  }
  logTranscodeEvent({{"event", "start"},
                     {"kind", "sf3p"},
                     {"source", sgyPath},
                     {"output", sf3pPath},
                     {"lodPlan", QJsonArray::fromStringList(plannedLevels)}});

  connect(task, &PaleoTask::finished, this,
          [this, task, outPath, outPyramid, outL3, outLevels, engineRan, onFinished, onReport]() {
    activeTranscodeOutputs_.remove(*outPath);
    if (!onFinished && !onReport)
      return;

    const bool succeeded = task->state() == PaleoTask::State::Succeeded;
    const bool cancelled = task->state() == PaleoTask::State::Cancelled;

    SeismicTranscodeReport report;
    report.output = *outPath;
    report.ok = succeeded;
    report.cancelled = cancelled;
    if (*engineRan)
    {
      const engine::PagedBuildResult *l3 = outL3->status.code == engine::StatusCode::Ok ||
                                                    outL3->reused
                                                ? outL3.get()
                                                : nullptr;
      fillReportFromPaged(report, *outPyramid, *outLevels,
                          outLevels->contains(QLatin1String("L3")) ? outL3.get() : nullptr);
      (void)l3;
    }
    report.ok = succeeded;
    report.cancelled = cancelled;

    if (succeeded)
    {
      invalidateDataset(*outPath); // 热切换：下次读取按磁盘现状重开
      if (onFinished)
        onFinished(true, *outPath, QString());
    }
    else if (cancelled)
    {
      if (onFinished)
        onFinished(false, *outPath, tr("分页转码已取消（可续跑）"));
    }
    else if (onFinished)
    {
      onFinished(false, *outPath, task->errorText());
    }

    QJsonObject event = reportToJson(report);
    event.insert("event", succeeded ? "finished" : (cancelled ? "cancelled" : "failed"));
    if (!succeeded && !cancelled)
      event.insert("error", task->errorText());
    logTranscodeEvent(event);
    if (onReport)
      onReport(report);
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
  const QPointer<SeismicTaskService> self(this); // 仅作瓦片投递目标；条目走 registry
  const auto registry = registry_;

  auto work = [self, registry, sf3pPath, sampleIndex, tileSize, focusInline, focusXline, outImage](
                  PaleoTask *task) -> QString {
    const auto entry = datasetEntryFor(registry, sf3pPath, sdk::Backend::Paged);
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
  const auto registry = registry_;
  const bool paged = datasetPath.endsWith(QStringLiteral(".sf3p"), Qt::CaseInsensitive);

  auto work = [registry, datasetPath, paged, request, outWindow](PaleoTask *task) -> QString {
    const auto entry = datasetEntryFor(
        registry, datasetPath, paged ? sdk::Backend::Paged : sdk::Backend::Auto);
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
  const auto registry = registry_;

  auto work = [registry, sf3pPath, status](PaleoTask *task) -> QString {
    Q_UNUSED(task);
    const auto entry = datasetEntryFor(registry, sf3pPath, sdk::Backend::Paged);
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
  const auto registry = registry_;

  auto work = [registry, sf3pPath, lodLevel, quality](PaleoTask *task) -> QString {
    Q_UNUSED(task);
    const auto entry = datasetEntryFor(registry, sf3pPath, sdk::Backend::Paged);
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


// ---- D1.2/D1.6/D1.8 断点探测 -------------------------------------------------

SeismicWorkspaceProbe SeismicTaskService::probeWorkspace(const QString &sgyOrMetaPath) const
{
  SeismicWorkspaceProbe probe;
  QString base = sgyOrMetaPath;
  if (base.endsWith(QLatin1String(".sf3c.meta"), Qt::CaseInsensitive))
    base.chop(static_cast<int>(qstrlen(".sf3c.meta")));

  engine::WorkspaceMetaSummary summary;
  std::string error;
  engine::ProbeWorkspaceMeta(std::filesystem::path(base.toStdString()), summary, error);
  probe.exists = summary.exists;
  probe.complete = summary.complete;
  probe.resumable = summary.exists && summary.readable && !summary.complete;
  probe.readable = summary.readable;
  probe.formatVersion = int(summary.formatVersion);
  probe.algorithmVersion = int(summary.algorithmVersion);
  probe.chunksDone = qint64(summary.chunksCompleted);
  probe.chunksTotal = qint64(summary.chunkCount);
  probe.samples = qint64(summary.samples);
  probe.inlines = qint64(summary.inlines);
  probe.xlines = qint64(summary.xlines);
  probe.error = QString::fromStdString(summary.error);
  return probe;
}

SeismicWorkspaceProbe SeismicTaskService::probePagedWorkspace(const QString &sf3pPath) const
{
  SeismicWorkspaceProbe probe;
  const QFileInfo finalFile(sf3pPath);
  const QString partialPath = sf3pPath + QStringLiteral(".partial");

  if (finalFile.exists())
  {
    probe.exists = true;
    engine::PagedWorkspaceReader reader;
    std::string error;
    if (reader.Open(std::filesystem::path(sf3pPath.toStdString()), error, /*metadataOnly=*/true))
    {
      probe.readable = true;
      probe.complete = reader.Info().complete;
      probe.resumable = !probe.complete;
      probe.formatVersion = 7; // kPagedWorkspaceVersion（v6 legacy 可读）
      probe.algorithmVersion = int(reader.Info().algorithmVersion);
      probe.inlines = qint64(reader.Info().inlineAxis.count);
      probe.xlines = qint64(reader.Info().xlineAxis.count);
      probe.samples = qint64(reader.Info().samples);
    }
    else
    {
      probe.error = tr("分页工作区头不可读：%1").arg(QString::fromStdString(error));
    }
    return probe;
  }

  if (QFileInfo::exists(partialPath))
  {
    probe.exists = true;
    probe.resumable = true; // .partial 半成品：续跑从完成位图继续
    engine::PagedWorkspaceReader reader;
    std::string error;
    if (reader.Open(std::filesystem::path(partialPath.toStdString()), error, /*metadataOnly=*/true))
    {
      probe.readable = true;
      probe.formatVersion = 7;
      probe.algorithmVersion = int(reader.Info().algorithmVersion);
      probe.inlines = qint64(reader.Info().inlineAxis.count);
      probe.xlines = qint64(reader.Info().xlineAxis.count);
      probe.samples = qint64(reader.Info().samples);
    }
    else
    {
      probe.error = tr("分页工作区断点文件不可读：%1").arg(QString::fromStdString(error));
    }
  }
  return probe;
}

QString SeismicWorkspaceProbe::stateText() const
{
  if (!exists)
    return QObject::tr("未开始");
  if (complete)
    return QObject::tr("已完成");
  if (!readable)
    return QObject::tr("不可读（%1）").arg(error.isEmpty() ? QObject::tr("格式版本不支持") : error);
  return QObject::tr("已完成 %1/%2 块（可续跑）").arg(chunksDone).arg(chunksTotal);
}

QString SeismicTranscodeReport::summaryLine() const
{
  QString line = ok ? QObject::tr("转码完成") : (cancelled ? QObject::tr("已取消（可续跑）") : QObject::tr("失败"));
  line += QObject::tr(" · 道 %1 · 覆盖 %2% · 丢弃 %3%")
              .arg(tracesRead)
              .arg(int(coverage() * 100))
              .arg(QString::number(droppedRatio() * 100, 'f', 2));
  if (validValues)
    line += QObject::tr(" · 值域 [%1, %2]")
                .arg(QString::number(valueMin, 'g', 6))
                .arg(QString::number(valueMax, 'g', 6));
  if (!lodLevels.isEmpty())
    line += QObject::tr(" · 金字塔 %1").arg(lodLevels.join(QLatin1Char('/')));
  if (damagedTraces > 0)
    line += QObject::tr(" · 坏道 %1（%2…）")
                .arg(damagedTraces)
                .arg(damagedSample.isEmpty() ? QString() : damagedSample.first());
  return line;
}

QString SeismicTranscodeReport::toJsonLine() const
{
  return QStringLiteral("PALEO-SEISMIC-TRANSCODE ") +
         QString::fromUtf8(QJsonDocument(reportToJson(*this)).toJson(QJsonDocument::Compact));
}

// ---- D2.11 道头查询 -----------------------------------------------------------

SeismicTraceHeaderInfo SeismicTaskService::readTraceHeader(const QString &sgyPath, int traceIndex)
{
  SeismicTraceHeaderInfo info;
  info.traceIndex = traceIndex;
  if (sgyPath.isEmpty() || traceIndex < 0)
  {
    info.error = QStringLiteral("无效道序号");
    return info;
  }

  // 样本数/格式码：索引命中最好；未命中退二进制头（规则文件可靠）
  int sampleCount = 0;
  int formatCode = 5;
  double dtUs = 0.0;
  std::string reason;
  if (SgyIndexPtr index = SgyIndexCache::Load(std::filesystem::path(sgyPath.toStdString()), reason))
  {
    if (traceIndex >= static_cast<int>(index->traces.size()))
    {
      info.error = QStringLiteral("道序号 %1 超出索引范围（共 %2 道）").arg(traceIndex).arg(index->traces.size());
      return info;
    }
    sampleCount = index->sampleCount;
    formatCode = index->formatCode;
    dtUs = index->sampleIntervalUs;
  }

  QFile file(sgyPath);
  if (!file.open(QIODevice::ReadOnly))
  {
    info.error = QStringLiteral("无法打开 SEG-Y 文件");
    return info;
  }
  if (sampleCount <= 0)
  {
    QByteArray binHdr = file.read(3600);
    if (binHdr.size() < 3600)
    {
      info.error = QStringLiteral("SEG-Y 头读取失败");
      return info;
    }
    const auto be16 = [&](int at) -> int {
      return (quint8(binHdr[at]) << 8) | quint8(binHdr[at + 1]);
    };
    sampleCount = be16(3220);
    formatCode = be16(3224);
  }
  int bytesPerSample = 4;
  if (formatCode == 2 || formatCode == 3)
    bytesPerSample = formatCode == 3 ? 4 : 2;
  else if (formatCode == 8)
    bytesPerSample = 1;
  else if (formatCode == 1 || formatCode == 5)
    bytesPerSample = 4;

  const qint64 traceBytes = 240 + qint64(sampleCount) * bytesPerSample;
  info.fileOffset = 3600 + qint64(traceIndex) * traceBytes;
  if (!file.seek(info.fileOffset))
  {
    info.error = QStringLiteral("道偏移越界（文件 %1 字节）").arg(file.size());
    return info;
  }
  const QByteArray hdr = file.read(240);
  if (hdr.size() < 240)
  {
    info.error = QStringLiteral("道头读取不完整");
    return info;
  }
  file.close();

  const auto be16 = [&](int at) -> int {
    return qint16((quint8(hdr[at]) << 8) | quint8(hdr[at + 1]));
  };
  const auto be32 = [&](int at) -> qint32 {
    return qint32((quint32(quint8(hdr[at])) << 24) | (quint32(quint8(hdr[at + 1])) << 16) |
                   (quint32(quint8(hdr[at + 2])) << 8) | quint32(quint8(hdr[at + 3])));
  };
  // SEG-Y 字节序（1 基）→ 0 基偏移
  info.fieldRecord = be32(8);    // 9-12
  info.cdpEnsemble = be32(20);   // 21-24
  info.inlineNo = be32(188);     // 189-192 INLINE
  info.xlineNo = be32(192);      // 193-196 CROSSLINE
  info.sampleCount = be16(114);  // 115-116 ns
  info.sampleIntervalUs = dtUs > 0 ? int(dtUs) : be16(116); // 117-118 dt(μs)
  // 71-72 比例因子（负值 = 除以 |v|）
  double scalar = be16(70);
  if (scalar == 0.0)
    scalar = 1.0;
  const double rawX = static_cast<double>(be32(72)); // 73-76
  const double rawY = static_cast<double>(be32(76)); // 77-80
  info.cdpX = scalar > 0 ? rawX * scalar : rawX / -scalar;
  info.cdpY = scalar > 0 ? rawY * scalar : rawY / -scalar;
  info.ok = true;
  return info;
}

} // namespace seismic
