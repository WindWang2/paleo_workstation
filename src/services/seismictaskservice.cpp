// 层：数据
#include "services/seismictaskservice.h"
#include "domain/seismic/sectionaxis.h"

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

#if defined(Q_OS_UNIX)
#include <unistd.h>
#elif defined(Q_OS_WINDOWS)
#include <windows.h>
#endif

#include "Engine/PagedPipeline.h"
#include "Engine/PagedWorkspace.h"
#include "Engine/QuickOpen.h"
#include "Engine/Sdk.h"
#include "Engine/TranscodeJob.h"
#include "Engine/Types.h"
#include "Engine/WorkspaceFormat.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QUuid>
#include <QDir>

#include <algorithm>
#include <limits>
#include <thread>

#include "algorithms/seismicattr.h"
#include "algorithms/horizontrack.h"
#include "io/horizonbinner.h"
#include "catalog/datacatalog.h"
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

int SeismicTaskService::activeTaskCount() const
{
  return gate_ ? gate_->active.load() : 0;
}

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
    registry_(std::make_shared<SeismicDatasetRegistry>()),
    gate_(std::make_shared<SeismicConcurrencyGate>(kMaxConcurrentTasks))
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

  PaleoTask *task = startBounded(title, work, layerId); // D6.4 ≤4 并发闸
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
  const std::string key =
      SgyDataCache::MakeKey(typeStr, volume->Index()->fileSize,
                            static_cast<std::uint64_t>(sliceIndex)) +
      ":" + volume->Path().string();

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

  // quiet：切片提取是视口交互内嵌取数，不拉起任务中心。
  PaleoTask *task = startBounded(title, work, QString(), /*quiet=*/true); // D6.4 ≤4 并发闸
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

  // D5.2 任意线缓存：同体同路径重复提取即出（缓存查询+命中回调走事件循环）
  if (std::shared_ptr<const SgySliceImage> hit = cachedSection(pathPoints, volume))
  {
    SgySectionStats stats;
    stats.columnDistances.reserve(hit->width);
    for (int c = 0; c < hit->width; ++c)
      stats.columnDistances.push_back(float(c) * 25.0f); // 近似道距（缓存路径无 stats）
    if (onFinished)
    {
      auto *timer = new QTimer(this);
      timer->setSingleShot(true);
      connect(timer, &QTimer::timeout, this, [timer, hit, stats, onFinished]() {
        delete timer;
        onFinished(true, hit, stats, QString());
      });
      timer->start(0);
    }
    return nullptr;
  }

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

  // quiet：剖面条拖动是交互内嵌取数，不拉起任务中心。
  PaleoTask *task = startBounded(title, work, QString(), /*quiet=*/true); // D6.4 ≤4 并发闸
  connect(task, &PaleoTask::finished, this,
          [this, task, outImage, outStats, onFinished, volume, pathPoints]() {
    if (task->state() == PaleoTask::State::Succeeded)
    {
      cacheSection(pathPoints, volume, outImage); // D5.2 成功入 LRU
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

  PaleoTask *task = startBounded(title, work); // D6.4 ≤4 并发闸
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

  PaleoTask *task = startBounded(title, work); // D6.4 ≤4 并发闸
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

  PaleoTask *task = startBounded(title, work); // D6.4 ≤4 并发闸
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

  PaleoTask *task = startBounded(title, work); // D6.4 ≤4 并发闸
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

  // A3（wave/deepen-perf）同路径取代语义：拖动连发的新瓦片请求立即取消旧
  // 在途任务——引擎按瓦片粒度协作中止，被顶替的整图读取不再占并发闸排队
  // （消费侧另有采样号世代过滤，双保险）。
  if (QPointer<PaleoTask> stale = inFlightTiledTasks_.value(sf3pPath))
    stale->requestCancel();

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

  // quiet：时间片瓦片是拖动交互取数，不拉起任务中心。
  PaleoTask *task = startBounded(title, work, QString(), /*quiet=*/true); // D6.4 ≤4 并发闸
  inFlightTiledTasks_.insert(sf3pPath, task); // A3 取代登记（同键新值顶替旧句柄）
  connect(task, &PaleoTask::finished, this, [this, sf3pPath, task, outImage, onFinished]() {
    // A3：终态即出取代表（仅当仍指向本任务——后来者已顶替则留给后来者清理）
    if (inFlightTiledTasks_.value(sf3pPath) == task)
      inFlightTiledTasks_.remove(sf3pPath);
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

  // quiet：三维体窗取数是视口交互取数，不拉起任务中心。
  PaleoTask *task = startBounded(title, work, QString(), /*quiet=*/true); // D6.4 ≤4 并发闸
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

// A1（wave/deepen-perf）体窗平面平移：窗口布局
// values[((ilIdx)*xlineCount + xlIdx)*sampleCount + sIdx]（ilIdx 沿轴值升序），
// 时间片显示约定 width=XL 数 / height=IL 数 / 行 0=最大 inline →
// out.values[row*xlineCount + col]，row = inlineCount-1-ilIdx。
// 与引擎 ReadTimeSlice 逐位同构（tst_seismic_engine::voxelWindowPlanesMatchTimeSlice 锁定）。
bool SeismicTaskService::slicePlaneFromWindow(const engine::VoxelWindow &window,
                                              int sampleIndex, SgySliceImage &out)
{
  const engine::VoxelWindowRequest &box = window.box;
  if (sampleIndex < 0 || sampleIndex >= box.sampleCount || box.inlineCount <= 0 ||
      box.xlineCount <= 0 ||
      window.values.size() < std::size_t(box.inlineCount) * box.xlineCount * box.sampleCount)
    return false;

  out = SgySliceImage{};
  out.width = box.xlineCount;
  out.height = box.inlineCount;
  const std::size_t plane = std::size_t(box.inlineCount) * box.xlineCount;
  out.values.assign(plane, std::numeric_limits<float>::quiet_NaN());
  bool anyFinite = false;
  float vmin = std::numeric_limits<float>::max();
  float vmax = std::numeric_limits<float>::lowest();
  for (int il = 0; il < box.inlineCount; ++il)
  {
    const int row = box.inlineCount - 1 - il; // 行 0 = 最大 inline（显示向）
    for (int xl = 0; xl < box.xlineCount; ++xl)
    {
      const float v = window.values[(std::size_t(il) * box.xlineCount + xl) *
                                    box.sampleCount + sampleIndex];
      out.values[std::size_t(row) * box.xlineCount + xl] = v;
      if (std::isfinite(v))
      {
        anyFinite = true;
        vmin = std::min(vmin, v);
        vmax = std::max(vmax, v);
      }
    }
  }
  if (anyFinite)
  {
    out.valueMin = vmin;
    out.valueMax = vmax;
  }
  else
  {
    out.valueMin = 0.0f;
    out.valueMax = 1.0f;
  }
  return true;
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
    // D6.5：短操作的取消边界检查（入池即取消则跳过引擎调用）
        if (task->cancelRequested())
          return QString();
    const auto entry = datasetEntryFor(registry, sf3pPath, sdk::Backend::Paged);
    if (!entry || !entry->dataset)
      return QObject::tr("分页工作区不可用：%1").arg(sf3pPath);
    QMutexLocker lock(&entry->mutex);
    *status = statusFromDataset(*entry->dataset);
    return QString();
  };

  PaleoTask *task = startBounded(title, work); // D6.4 ≤4 并发闸
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
    // D6.5：短操作的取消边界检查（入池即取消则跳过引擎调用）
        if (task->cancelRequested())
          return QString();
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

  // quiet：拖动期 LOD 切换是视口交互取数，不拉起任务中心。
  PaleoTask *task = startBounded(title, work, QString(), /*quiet=*/true); // D6.4 ≤4 并发闸
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
    // D6.5：短操作的取消边界检查（入池即取消则跳过引擎调用）
        if (task->cancelRequested())
          return QString();
    sdk::OpenOptions options; // Auto：有 .sf3c 用工作区，否则直读
    engine::Status openStatus;
    auto dataset = sdk::Dataset::Open(std::filesystem::path(sgyPath.toStdString()),
                                      options, openStatus);
    if (!dataset || !openStatus.ok())
      return QObject::tr("打开数据集失败：%1").arg(QString::fromStdString(openStatus.message));
    *status = statusFromDataset(*dataset);
    return QString();
  };

  PaleoTask *task = startBounded(title, work); // D6.4 ≤4 并发闸
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


// ---- Phase 4 解释工具 ---------------------------------------------------------

namespace {

// 归一化互相关已下沉 algorithms/horizontrack（goal/horizon-autotrack），
// 服务层不再持有核副本。

QString horizonCsvLine(const SeismicPick &p)
{
  return QStringLiteral("%1,%2,%3,%4")
      .arg(p.inlineNo).arg(p.xlineNo)
      .arg(QString::number(p.twtMs, 'f', 2))
      .arg(QString::number(p.confidence, 'f', 3));
}

QString sha256OfFile(const QString &path)
{
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return QString();
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&f))
    return QString();
  return QString::fromLatin1(hash.result().toHex());
}

} // namespace

QStringList SeismicInterpretationSession::horizonNames() const
{
  QStringList names;
  for (const SeismicPick &p : picks)
    if (!names.contains(p.horizonName))
      names << p.horizonName;
  return names;
}

const SeismicPick *SeismicInterpretationSession::pickById(int id) const
{
  for (const SeismicPick &p : picks)
    if (p.id == id)
      return &p;
  return nullptr;
}

namespace {

// goal/horizon-autotrack — SgySliceImage（行主序 row*width+col，引擎契约
// 行 0 = 最大时间/时间底）→ 追踪核道主序、采样升序缓冲（trace*nS+s）。
// 两个不可省的变换（D4.2 修复：旧实现把行序当采样序，真切片上追踪时间轴
// 垂直镜像——种子（采样序）对不上行序事件，追踪诚实停）：
//   1) 行→采样翻转：s = height-1-row（SeismicPick.sampleIndex/twtMs 是
//      采样升序空间，画布/资产登记全链一致）；
//   2) 转置：核里一道垂直窗是内存连续段，行主序直接喂会把「横向跨道条」
//      当波形（seismic-attributes 轮2 同教训）。
std::vector<float> traceMajorSection(const SgySliceImage &slice)
{
  std::vector<float> out(static_cast<std::size_t>(slice.width) * slice.height);
  for (int col = 0; col < slice.width; ++col)
    for (int s = 0; s < slice.height; ++s)
      out[static_cast<std::size_t>(col) * slice.height + s] =
          slice.values[static_cast<std::size_t>(slice.height - 1 - s) * slice.width + col];
  return out;
}

QString trackStopText(paleo::hztrack::StopReason reason)
{
  switch (reason)
  {
  case paleo::hztrack::StopReason::CorrelationLoss:
    return QObject::tr("相关丢失");
  case paleo::hztrack::StopReason::CoherenceGate:
    return QObject::tr("相干门槛");
  case paleo::hztrack::StopReason::Cancelled:
    return QObject::tr("已取消");
  case paleo::hztrack::StopReason::Invalid:
    return QObject::tr("输入无效");
  case paleo::hztrack::StopReason::Completed:
    return QObject::tr("到达边界");
  }
  return QString();
}

// 剖面列号 → 测线号标注（IL 剖面列=XL，XL 剖面列=IL）；列轴见 sectionaxis.h（#147）
QString trackColLabel(int col, SgySliceType type, int colMin, const std::vector<int> &lines)
{
  int lineNo = colMin + col;
  seismic::sectionLineForColumn(lines, colMin, col, &lineNo);
  return type == SgySliceType::Inline
             ? QStringLiteral("@XL%1").arg(lineNo)
             : QStringLiteral("@IL%1").arg(lineNo);
}

struct KernelTrackOutput
{
  QList<SeismicPick> picks;
  SeismicTrackReport report;
};

// 追踪核编排（同步/异步任务共用）：逐种子 trackSection + 合并 + 域映射 +
// 报告。cancelled 可空；task 非空时报进度（每种子一步）。
KernelTrackOutput runKernelTracking(
    const SgySliceImage &slice, SgySliceType sectionType, int sectionIndex,
    int colMin, int colMax, const QList<QPair<int, int>> &seeds,
    const SeismicTrackOptions &options, const QString &interpreter,
    const QString &horizonName, float sampleIntervalMs,
    const std::function<bool()> &cancelled, PaleoTask *task)
{
  KernelTrackOutput out;
  out.report.totalTraces = slice.width;
  if (slice.width <= 0 || slice.height <= 0 || slice.values.empty() ||
      seeds.isEmpty())
  {
    out.report.stopSummary = QObject::tr("无剖面数据或无种子");
    return out;
  }
  const std::vector<float> section = traceMajorSection(slice);
  const paleo::hztrack::TrackOptions kernelOptions{
      options.windowSamples, options.maxSearchSamples,
      options.correlationThreshold, 0.0};
  std::vector<paleo::hztrack::TrackResult> runs;
  runs.reserve(static_cast<std::size_t>(seeds.size()));
  bool sawCancel = false;
  for (int i = 0; i < seeds.size(); ++i)
  {
    runs.push_back(paleo::hztrack::trackSection(
        section.data(), slice.width, slice.height,
        {seeds[i].first, seeds[i].second}, kernelOptions, nullptr, cancelled));
    if (task)
      task->reportBytes(i + 1, seeds.size());
    if (runs.back().stopLeft.reason == paleo::hztrack::StopReason::Cancelled ||
        runs.back().stopRight.reason == paleo::hztrack::StopReason::Cancelled)
      sawCancel = true;
  }
  const std::vector<paleo::hztrack::TracedPick> merged =
      paleo::hztrack::mergeTraced(runs);

  const auto toPick = [&](const paleo::hztrack::TracedPick &p) {
    SeismicPick pick;
    // 列 → 实际测线号（#147：非单位线距不能 colMin+col）；样点 → TWT 含记录延迟（#146）。
    int lineNo = colMin + p.trace;
    seismic::sectionLineForColumn(options.columnLines, colMin, p.trace, &lineNo);
    pick.inlineNo = sectionType == SgySliceType::Inline ? sectionIndex : lineNo;
    pick.xlineNo = sectionType == SgySliceType::Inline ? lineNo : sectionIndex;
    pick.sampleIndex = p.sample;
    pick.twtMs = seismic::sectionTwtForSample(p.sample, options.startTimeMs,
                                              double(sampleIntervalMs));
    pick.confidence = p.confidence;
    pick.interpreter = interpreter;
    pick.horizonName = horizonName;
    return pick; // id=0 由会话分配
  };
  double confSum = 0.0;
  out.picks.reserve(int(merged.size()));
  for (const paleo::hztrack::TracedPick &p : merged)
  {
    out.picks.append(toPick(p));
    confSum += p.confidence;
  }
  out.report.coveredTraces = int(merged.size());
  out.report.meanConfidence =
      merged.empty() ? 0.0f : float(confSum / double(merged.size()));
  // 停因：两侧首个未覆盖道，取止于该道的种子结果（覆盖失败区如实留空）
  if (sawCancel)
    out.report.stopSummary = QObject::tr("已取消");
  else if (out.report.coveredTraces == out.report.totalTraces)
    out.report.stopSummary = QObject::tr("全程覆盖");
  else
  {
    QStringList parts;
    const auto sideStop = [&](bool left) {
      if (merged.empty())
        return;
      const int gapTrace = left ? int(merged.front().trace) - 1
                                : int(merged.back().trace) + 1;
      if (gapTrace < 0 || gapTrace >= slice.width)
        return; // 该侧扫满（无缺口）
      for (const paleo::hztrack::TrackResult &r : runs)
      {
        const paleo::hztrack::TrackStop &stop =
            left ? r.stopLeft : r.stopRight;
        if (stop.trace == gapTrace)
        {
          parts << (left ? QObject::tr("左：%1%2")
                              .arg(trackStopText(stop.reason),
                                   trackColLabel(gapTrace, sectionType, colMin, options.columnLines))
                         : QObject::tr("右：%1%2")
                              .arg(trackStopText(stop.reason),
                                   trackColLabel(gapTrace, sectionType, colMin, options.columnLines)));
          return;
        }
      }
    };
    sideStop(true);
    sideStop(false);
    out.report.stopSummary =
        parts.isEmpty() ? QObject::tr("部分覆盖") : parts.join(QStringLiteral("；"));
  }
  return out;
}

} // namespace

QList<SeismicPick> SeismicTaskService::trackHorizon(
    const SgySliceImage &slice,
    SgySliceType sectionType, int sectionIndex,
    int colMin, int colMax,
    int seedTraceCol, int seedSample,
    const SeismicTrackOptions &options,
    const QString &interpreter, const QString &horizonName,
    float sampleIntervalMs)
{
  // 数值核在 algorithms/horizontrack（goal/horizon-autotrack 下沉）：
  // 单种子 = 多种子面的特例（含种子拾取，按道序升序）。
  return trackHorizonMultiSeeds(slice, sectionType, sectionIndex, colMin,
                                colMax, {{seedTraceCol, seedSample}}, options,
                                interpreter, horizonName, sampleIntervalMs);
}

QList<SeismicPick> SeismicTaskService::trackHorizonMultiSeeds(
    const SgySliceImage &slice,
    SgySliceType sectionType, int sectionIndex,
    int colMin, int colMax,
    const QList<QPair<int, int>> &seeds,
    const SeismicTrackOptions &options,
    const QString &interpreter, const QString &horizonName,
    float sampleIntervalMs,
    SeismicTrackReport *report)
{
  const KernelTrackOutput out =
      runKernelTracking(slice, sectionType, sectionIndex, colMin, colMax,
                        seeds, options, interpreter, horizonName,
                        sampleIntervalMs, nullptr, nullptr);
  if (report)
    *report = out.report;
  return out.picks;
}

PaleoTask *SeismicTaskService::startHorizonTracking(
    const SgySliceImage &slice,
    SgySliceType sectionType, int sectionIndex,
    int colMin, int colMax,
    const QList<QPair<int, int>> &seeds,
    const SeismicTrackOptions &options,
    const QString &interpreter, const QString &horizonName,
    float sampleIntervalMs,
    std::function<void(bool ok, const QList<SeismicPick> &picks,
                       const SeismicTrackReport &report,
                       const QString &error)> onFinished)
{
  if (!taskService_)
  {
    if (onFinished)
      onFinished(false, {}, SeismicTrackReport{},
                 QStringLiteral("PaleoTaskService not set"));
    return nullptr;
  }
  const QString title = tr("层位追踪（%1 种子）").arg(seeds.size());
  // worker 线程只取 values（rgba 不复制——追踪不需要着色缓冲）
  auto sliceValues = std::make_shared<SgySliceImage>();
  sliceValues->width = slice.width;
  sliceValues->height = slice.height;
  sliceValues->valueMin = slice.valueMin;
  sliceValues->valueMax = slice.valueMax;
  sliceValues->values = slice.values;
  auto picksOut = std::make_shared<QList<SeismicPick>>();
  auto reportOut = std::make_shared<SeismicTrackReport>();
  auto work = [sliceValues, sectionType, sectionIndex, colMin, colMax, seeds,
               options, interpreter, horizonName, sampleIntervalMs, picksOut,
               reportOut](PaleoTask *task) -> QString {
    const KernelTrackOutput out = runKernelTracking(
        *sliceValues, sectionType, sectionIndex, colMin, colMax, seeds,
        options, interpreter, horizonName, sampleIntervalMs,
        [task]() { return task && task->cancelRequested(); }, task);
    if (task && task->cancelRequested())
      return QString(); // 取消不发布半成品
    *picksOut = out.picks;
    *reportOut = out.report;
    return QString();
  };
  // 追踪是交互触发（面板按钮），不拉起任务中心；取消经任务句柄。
  PaleoTask *task = startBounded(title, work, QString(), /*quiet=*/true);
  connect(task, &PaleoTask::finished, this,
          [task, picksOut, reportOut, onFinished]() {
            if (!onFinished)
              return;
            if (task->state() == PaleoTask::State::Succeeded)
              onFinished(true, *picksOut, *reportOut, QString());
            else if (task->state() == PaleoTask::State::Cancelled)
              onFinished(false, {}, SeismicTrackReport{}, tr("追踪已取消"));
            else
              onFinished(false, {}, SeismicTrackReport{}, task->errorText());
          });
  return task;
}

SeismicHorizonGrid SeismicTaskService::gridPicks(const QList<SeismicPick> &picks)
{
  SeismicHorizonGrid grid;
  if (picks.isEmpty())
    return grid;
  int ilMin = picks.first().inlineNo, ilMax = ilMin;
  int xlMin = picks.first().xlineNo, xlMax = xlMin;
  for (const SeismicPick &p : picks)
  {
    ilMin = std::min(ilMin, p.inlineNo); ilMax = std::max(ilMax, p.inlineNo);
    xlMin = std::min(xlMin, p.xlineNo); xlMax = std::max(xlMax, p.xlineNo);
  }
  // 步长：同轴不同值之间的最小间隔（全部相同则 1）
  const auto axisStep = [](QList<int> values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    if (values.size() < 2)
      return 1;
    int step = values[1] - values[0];
    for (int i = 2; i < values.size(); ++i)
      step = std::min(step, values[i] - values[i - 1]);
    return std::max(1, step);
  };
  QList<int> ils, xls;
  for (const SeismicPick &p : picks)
  {
    ils << p.inlineNo;
    xls << p.xlineNo;
  }
  grid.inlineStep = axisStep(ils);
  grid.xlineStep = axisStep(xls);
  grid.inlineMin = ilMin;
  grid.inlineCount = (ilMax - ilMin) / grid.inlineStep + 1;
  grid.xlineMin = xlMin;
  grid.xlineCount = (xlMax - xlMin) / grid.xlineStep + 1;
  const std::size_t n = std::size_t(grid.inlineCount) * grid.xlineCount;
  grid.twtMs.assign(n, std::numeric_limits<double>::quiet_NaN());
  grid.confidence.assign(n, 0.0f);

  // IDW：power=2；样本即拾取点
  for (int gi = 0; gi < grid.inlineCount; ++gi)
  {
    for (int gx = 0; gx < grid.xlineCount; ++gx)
    {
      const int il = grid.inlineMin + gi * grid.inlineStep;
      const int xl = grid.xlineMin + gx * grid.xlineStep;
      double num = 0.0, den = 0.0, bestConf = 0.0;
      for (const SeismicPick &p : picks)
      {
        const double dil = double(p.inlineNo - il) / std::max(1, grid.inlineStep);
        const double dxl = double(p.xlineNo - xl) / std::max(1, grid.xlineStep);
        const double d2 = dil * dil + dxl * dxl;
        if (d2 < 1e-12)
        {
          num = p.twtMs;
          den = 1.0;
          bestConf = p.confidence;
          break;
        }
        const double wgt = 1.0 / d2;
        num += wgt * p.twtMs;
        den += wgt;
        bestConf = std::max(bestConf, double(p.confidence));
      }
      if (den > 0)
      {
        grid.twtMs[std::size_t(gi) * grid.xlineCount + gx] = num / den;
        grid.confidence[std::size_t(gi) * grid.xlineCount + gx] = float(bestConf);
      }
    }
  }
  return grid;
}

QString SeismicTaskService::registerHorizonAsset(
    DataCatalog *catalog, const QString &seismicAssetId,
    const QString &seismicVersionId, const QString &horizonName,
    const QList<SeismicPick> &picks, const QString &outputDir,
    QString *error, LayerDeclaration *layerOut)
{
  if (!catalog || picks.isEmpty())
  {
    if (error)
      *error = QStringLiteral("catalog 未设置或拾取集为空");
    return QString();
  }
  const SeismicHorizonGrid grid = gridPicks(picks);
  if (!grid.isValid())
  {
    if (error)
      *error = QStringLiteral("拾取网格化失败");
    return QString();
  }
  QDir().mkpath(outputDir);
  const QString fileName = QStringLiteral("%1_%2_horizon.csv")
                               .arg(QFileInfo(outputDir).fileName() == QStringLiteral("interpretation")
                                        ? QStringLiteral("seismic")
                                        : QFileInfo(outputDir).fileName())
                               .arg(horizonName);
  const QString filePath = outputDir + QLatin1Char('/') + fileName;
  QFile f(filePath);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    if (error)
      *error = QStringLiteral("无法写层位文件 %1").arg(filePath);
    return QString();
  }
  f.write("inline,xline,twt_ms,confidence\n");
  for (int gi = 0; gi < grid.inlineCount; ++gi)
    for (int gx = 0; gx < grid.xlineCount; ++gx)
    {
      const double twt = grid.twtMs[std::size_t(gi) * grid.xlineCount + gx];
      if (!std::isfinite(twt))
        continue;
      f.write(QStringLiteral("%1,%2,%3,%4\n")
                  .arg(grid.inlineMin + gi * grid.inlineStep)
                  .arg(grid.xlineMin + gx * grid.xlineStep)
                  .arg(QString::number(twt, 'f', 2))
                  .arg(QString::number(grid.confidence[std::size_t(gi) * grid.xlineCount + gx], 'f', 3))
                  .toUtf8());
    }
  f.close();

  // goal/horizon-autotrack — 层位栅格 GeoTIFF（拾取网格 → 既有 horizonbinner
  // 管线装箱，不开平行层位格式）+ LayerDeclaration 回填供调用方声明上图。
  if (layerOut)
  {
    BinnedHorizon binned;
    binned.rows = grid.inlineCount;
    binned.cols = grid.xlineCount;
    // 局部测网：像元 = 测线步长（IL/XL 索引空间），P1 角 = (最小 IL, 最小 XL)
    // 映射到局部 XY 原点——与 binHorizon 同装箱约定（行 0 = 最大 inline 北向上）
    binned.dx = grid.xlineStep;
    binned.dy = grid.inlineStep;
    binned.originX = 0.0;
    binned.originY = double(grid.inlineCount - 1) * binned.dy;
    binned.hasInlineRange = binned.hasXlineRange = true;
    binned.inlineMin = grid.inlineMin;
    binned.inlineMax =
        grid.inlineMin + (grid.inlineCount - 1) * grid.inlineStep;
    binned.xlineMin = grid.xlineMin;
    binned.xlineMax =
        grid.xlineMin + (grid.xlineCount - 1) * grid.xlineStep;
    binned.z.assign(std::size_t(binned.rows) * binned.cols, -9999.0f);
    double zMin = std::numeric_limits<double>::infinity();
    double zMax = -std::numeric_limits<double>::infinity();
    for (int gi = 0; gi < grid.inlineCount; ++gi)
      for (int gx = 0; gx < grid.xlineCount; ++gx)
      {
        const double twt = grid.twtMs[std::size_t(gi) * grid.xlineCount + gx];
        if (!std::isfinite(twt))
          continue; // 无控制点：nodata 如实留空（诚实失败，不插值填充）
        const int row = (grid.inlineCount - 1) - gi; // 行 0 = 最大 inline
        binned.z[std::size_t(row) * binned.cols + gx] = float(twt);
        zMin = std::min(zMin, twt);
        zMax = std::max(zMax, twt);
      }
    binned.zMin = std::isfinite(zMin) ? zMin : 0.0;
    binned.zMax = std::isfinite(zMax) ? zMax : 0.0;
    const QString tifName = fileName.chopped(4) + QStringLiteral(".tif");
    const QString tifPath = outputDir + QLatin1Char('/') + tifName;
    QString tifError;
    if (!writeHorizonGeoTiff(binned, tifPath, &tifError))
    {
      if (error)
        *error = QStringLiteral("层位 GeoTIFF 写出失败：%1").arg(tifError);
      return QString();
    }
    layerOut->layerId = QStringLiteral("horizon.%1").arg(horizonName);
    layerOut->horizon = horizonName;
    layerOut->type = QStringLiteral("raster");
    layerOut->source = tifPath;
    layerOut->group = QStringLiteral("00_Data");
    layerOut->title = QStringLiteral("%1（地震追踪层位）").arg(horizonName);
  }

  // DERIVED 版本登记（外链托管：解释产物在工程 interpretation/ 目录）
  CatalogAsset asset;
  asset.id = QStringLiteral("seis_horizon_%1_%2").arg(seismicAssetId).arg(horizonName);
  asset.type = QStringLiteral("horizon");
  asset.format = QStringLiteral("csv");
  asset.displayName = QStringLiteral("%1（地震拾取）").arg(horizonName);
  catalog->addAsset(asset); // 已存在则失败被忽略（幂等语义）

  CatalogVersion v;
  v.id = QStringLiteral("ver_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
  v.assetId = asset.id;
  v.stage = QStringLiteral("DERIVED");
  v.versionNumber = 1;
  v.managed = false;
  v.path = filePath;
  v.sourceUri = seismicAssetId;
  v.sha256 = sha256OfFile(filePath);
  v.fileName = fileName;
  v.parentVersionIds = QStringList{seismicVersionId};
  v.extra.insert(QStringLiteral("origin"), QStringLiteral("seismic-interpretation"));
  v.extra.insert(QStringLiteral("pickCount"), picks.size());
  if (!catalog->addVersion(v))
  {
    // 版本可能已存在（重复登记）——按 (asset, version) 幂等返回路径
    const CatalogVersion existing = catalog->versionBySha256(v.sha256);
    if (!existing.id.isEmpty())
      return DataCatalog::resolvedVersionPath(QString(), existing);
    if (error)
      *error = QStringLiteral("catalog 版本登记失败");
    return QString();
  }
  return filePath;
}

QString SeismicTaskService::registerFaultAsset(
    DataCatalog *catalog, const QString &seismicAssetId,
    const QString &seismicVersionId, const QString &faultName,
    const QList<SeismicFaultSegment> &faults, const QString &outputDir,
    QString *error)
{
  if (!catalog || faults.isEmpty())
  {
    if (error)
      *error = QStringLiteral("catalog 未设置或断层集为空");
    return QString();
  }
  QDir().mkpath(outputDir);
  const QString fileName = QStringLiteral("seismic_%1_fault.csv").arg(faultName);
  const QString filePath = outputDir + QLatin1Char('/') + fileName;
  QFile f(filePath);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    if (error)
      *error = QStringLiteral("无法写断层文件 %1").arg(filePath);
    return QString();
  }
  f.write("segment_id,section_type,section_index,trace_frac,twt_ms\n");
  for (const SeismicFaultSegment &seg : faults)
  {
    const QString st = seg.sectionType == SgySliceType::Inline
        ? QStringLiteral("inline")
        : (seg.sectionType == SgySliceType::Xline ? QStringLiteral("xline")
                                                  : QStringLiteral("time"));
    for (const auto &pt : seg.points)
      f.write(QStringLiteral("%1,%2,%3,%4,%5\n")
                  .arg(seg.id).arg(st).arg(seg.sectionIndex)
                  .arg(QString::number(pt.first, 'f', 4))
                  .arg(QString::number(pt.second, 'f', 2))
                  .toUtf8());
  }
  f.close();

  CatalogAsset asset;
  asset.id = QStringLiteral("seis_fault_%1_%2").arg(seismicAssetId).arg(faultName);
  asset.type = QStringLiteral("fault");
  asset.format = QStringLiteral("csv");
  asset.displayName = QStringLiteral("%1（地震断层）").arg(faultName);
  catalog->addAsset(asset);

  CatalogVersion v;
  v.id = QStringLiteral("ver_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
  v.assetId = asset.id;
  v.stage = QStringLiteral("DERIVED");
  v.versionNumber = 1;
  v.managed = false;
  v.path = filePath;
  v.sourceUri = seismicAssetId;
  v.sha256 = sha256OfFile(filePath);
  v.fileName = fileName;
  v.parentVersionIds = QStringList{seismicVersionId};
  v.extra.insert(QStringLiteral("origin"), QStringLiteral("seismic-interpretation"));
  if (!catalog->addVersion(v))
  {
    if (error)
      *error = QStringLiteral("catalog 版本登记失败");
    return QString();
  }
  return filePath;
}

// ---- 地震属性（goal/seismic-attributes）------------------------------------

QString SeismicTaskService::seismicAttrId(SeismicAttrKind kind)
{
  switch (kind)
  {
  case SeismicAttrKind::Envelope:   return QStringLiteral("envelope");
  case SeismicAttrKind::InstPhase:  return QStringLiteral("instphase");
  case SeismicAttrKind::InstFreq:   return QStringLiteral("instfreq");
  case SeismicAttrKind::InstQ:      return QStringLiteral("instq");
  case SeismicAttrKind::Rms:        return QStringLiteral("rms");
  case SeismicAttrKind::MaxAbs:     return QStringLiteral("maxabs");
  case SeismicAttrKind::MeanEnergy: return QStringLiteral("energy");
  case SeismicAttrKind::Coherence:  return QStringLiteral("coherence");
  case SeismicAttrKind::Sweetness:  return QStringLiteral("sweetness");
  }
  return QStringLiteral("unknown");
}

QString SeismicTaskService::seismicAttrDisplayName(SeismicAttrKind kind)
{
  switch (kind)
  {
  case SeismicAttrKind::Envelope:   return QStringLiteral("包络");
  case SeismicAttrKind::InstPhase:  return QStringLiteral("瞬时相位");
  case SeismicAttrKind::InstFreq:   return QStringLiteral("瞬时频率");
  case SeismicAttrKind::InstQ:      return QStringLiteral("瞬时Q（原型）");
  case SeismicAttrKind::Rms:        return QStringLiteral("RMS 振幅");
  case SeismicAttrKind::MaxAbs:     return QStringLiteral("最大绝对振幅");
  case SeismicAttrKind::MeanEnergy: return QStringLiteral("平均能量");
  case SeismicAttrKind::Coherence:  return QStringLiteral("相干（semblance）");
  case SeismicAttrKind::Sweetness:  return QStringLiteral("甜点");
  }
  return QStringLiteral("未知属性");
}

bool SeismicTaskService::seismicAttrNeedsNeighbors(SeismicAttrKind kind)
{
  return kind == SeismicAttrKind::Coherence;
}

PaleoTask *SeismicTaskService::startAttributeSlice(
    std::shared_ptr<const SgyVolume> volume,
    SeismicAttrKind kind,
    const SeismicAttrParams &params,
    SgySliceType sliceType,
    int sliceIndex,
    std::function<void(bool success, const SeismicAttrResult &result)> onFinished)
{
  if (!taskService_)
  {
    if (onFinished)
    {
      SeismicAttrResult r;
      r.error = QStringLiteral("PaleoTaskService not set");
      onFinished(false, r);
    }
    return nullptr;
  }
  if (!volume || !volume->IsLoaded())
  {
    if (onFinished)
    {
      SeismicAttrResult r;
      r.error = QStringLiteral("地震体未加载（先完成体加载）");
      onFinished(false, r);
    }
    return nullptr;
  }
  if (sliceType == SgySliceType::Time)
  {
    // 瞬时族需整道谱、时窗族需垂向窗——时间切片（单采样面）不满足两者
    // 的输入形状；整体扫描属性体是独立工作项（TODOS 递延）。
    if (onFinished)
    {
      SeismicAttrResult r;
      r.error = QStringLiteral("时间切片属性暂不支持（需整体扫描，见 TODOS）");
      onFinished(false, r);
    }
    return nullptr;
  }

  const bool isInline = sliceType == SgySliceType::Inline;
  int lineValue = 0;
  if (!(isInline ? volume->ExactInlineValue(sliceIndex, lineValue)
                 : volume->ExactXlineValue(sliceIndex, lineValue)))
  {
    if (onFinished)
    {
      SeismicAttrResult r;
      r.error = isInline
          ? QStringLiteral("inline %1 不在本体内（无最近线替代）").arg(sliceIndex)
          : QStringLiteral("crossline %1 不在本体内（无最近线替代）").arg(sliceIndex);
      onFinished(false, r);
    }
    return nullptr;
  }

  // 邻线解析（相干 3 线窗）：稀疏测网按轴值表相邻位取，不臆造 ±1 号。
  std::vector<int> lineValues;
  const bool needsNeighbors = seismicAttrNeedsNeighbors(kind);
  if (needsNeighbors)
  {
    const std::vector<int> &axis =
        isInline ? volume->InlineValues() : volume->XlineValues();
    const auto it = std::find(axis.begin(), axis.end(), lineValue);
    const std::size_t pos = std::size_t(it - axis.begin());
    if (axis.size() < 3 || pos == 0 || pos + 1 >= axis.size())
    {
      if (onFinished)
      {
        SeismicAttrResult r;
      r.error = QStringLiteral("%1 %2 处于测网边缘，无双侧邻线，相干不可用")
                    .arg(isInline ? QStringLiteral("inline") : QStringLiteral("crossline"))
                    .arg(lineValue);
        onFinished(false, r);
      }
      return nullptr;
    }
    lineValues = {axis[pos - 1], lineValue, axis[pos + 1]};
  }
  else
  {
    lineValues = {lineValue};
  }

  auto result = std::make_shared<SeismicAttrResult>();
  result->attrId = seismicAttrId(kind);
  result->sectionType = sliceType;
  result->sectionIndex = lineValue;

  const QString title = tr("地震属性 %1（%2 %3）")
                            .arg(seismicAttrDisplayName(kind))
                            .arg(isInline ? QStringLiteral("Inline") : QStringLiteral("Crossline"))
                            .arg(lineValue);
  const double dtMs = double(volume->SampleIntervalUs()) / 1000.0;
  const int width = isInline ? volume->XlineCount() : volume->InlineCount();
  const int height = volume->SampleCount();
  const int sliceCount = int(lineValues.size());

  auto work = [volume, kind, params, sliceType, isInline, lineValues, result,
               dtMs, width, height, sliceCount](PaleoTask *task) -> QString {
    if (task->cancelRequested())
      return QString();

    const qint64 sliceUnits = qint64(width) * height;
    const qint64 totalUnits = sliceUnits * sliceCount + sliceUnits; // 读 + 算
    qint64 done = 0;

    // ---- 读阶段：目标线（+相干邻线）逐列提取 ----
    QElapsedTimer clock;
    clock.start();
    std::vector<SgySliceImage> slices(static_cast<std::size_t>(sliceCount));
    for (int li = 0; li < sliceCount; ++li)
    {
      if (task->cancelRequested())
        return QString();
      auto sliceProgress = [&task, &done, sliceUnits, totalUnits](int processed,
                                                                  int total) {
        const qint64 col = total > 0 ? qint64(processed) * sliceUnits / total : 0;
        task->reportBytes(done + col, totalUnits);
        return !task->cancelRequested();
      };
      std::string extractErr;
      if (!volume->ExtractSlice(sliceType, lineValues[std::size_t(li)],
                                slices[std::size_t(li)], extractErr, sliceProgress))
      {
        if (task->cancelRequested())
          return QString();
        result->error = QString::fromStdString(extractErr);
        return QStringLiteral("切片读取失败：%1").arg(result->error);
      }
      done += sliceUnits;
      task->reportStage(QStringLiteral("read"), int(done * 100 / totalUnits));
      task->reportBytes(done, totalUnits);
    }
    result->readMs = double(clock.elapsed());
    result->traceCount = width;

    // ---- 算阶段 ----
    task->reportStage(QStringLiteral("compute"), 0);
    clock.restart();
    auto out = std::make_shared<SgySliceImage>();
    out->width = slices[0].width;
    out->height = slices[0].height;
    out->values.assign(std::size_t(out->width) * out->height,
                       std::numeric_limits<float>::quiet_NaN());
    if (out->width != width || out->height != height)
    {
      result->error = QStringLiteral("切片几何异常（%1×%2，预期 %3×%4）")
                          .arg(out->width).arg(out->height).arg(width).arg(height);
      return result->error;
    }

    if (kind == SeismicAttrKind::Coherence)
    {
      // 组装 3 线小体：布局 [(lineIdx * width + x) * height + row]（row 空间
      // ——垂直窗对称，行序与时序等价）。邻线轴 = mini 体 IL 轴（半窗恒 1，
      // 恰覆 3 线）；沿剖轴的半窗按剖向映射：IL 剖→XL 向，XL 剖→IL 向。
      std::vector<float> vol3(static_cast<std::size_t>(sliceCount * width * height));
      // 切片是 [row][x] 行主序，必须转置拷入（内核按「道连续」读，平铺
      // 拷贝会把行列搅混——轮2 实测教训：S=1/3 恒定假象）。
      for (int li = 0; li < sliceCount; ++li)
        for (int x = 0; x < width; ++x)
          for (int row = 0; row < height; ++row)
            vol3[static_cast<std::size_t>((li * width + x) * height + row)] =
                slices[static_cast<std::size_t>(li)]
                    .values[static_cast<std::size_t>(row * width + x)];
      std::vector<float> coh(vol3.size(),
                             std::numeric_limits<float>::quiet_NaN());
      const int alongHalf =
          isInline ? params.coherenceXlHalf : params.coherenceIlHalf;
      paleo::seisattr::semblanceCoherence(
          vol3.data(), sliceCount, width, height,
          /*ilHalf=*/1, /*xlHalf=*/alongHalf, params.coherenceTimeHalf,
          coh.data());
      // 中线输出即目标线属性图（同 row-major 布局）。
      // 中线（il=1）出图：coh 逐道连续 [x][row] → 图像行主序 [row][x]，
      // 与组装侧对称的第二次转置（平铺拷贝会行列互换——轮2 教训二连）。
      for (int x = 0; x < width; ++x)
        for (int row = 0; row < height; ++row)
          out->values[static_cast<std::size_t>(row * width + x)] =
              coh[static_cast<std::size_t>((width + x) * height + row)];
      task->reportBytes(totalUnits, totalUnits);
    }
    else
    {
// 逐道族：列 → 时间序（切片 row 0 = 最深样）→ 核计算 → 写回 row 序。
      // 列间独立——std::thread 分片并行（≤min(4,hw) 片，SgyVolume 时间片多
      // 线程同先例）；进度由 0 号分片单调上报；取消全分片协作（原子标志）。
      const SgySliceImage &src = slices[0];
      const auto cancelled = [task]() { return task->cancelRequested(); };

      // 单列：读列（翻时间序）→ 核 → 写回（翻回 row 序）。
      auto processColumn = [&](int x) {
        std::vector<float> trace(static_cast<std::size_t>(height));
        std::vector<float> attr(static_cast<std::size_t>(height));
        for (int row = 0; row < height; ++row)
          trace[static_cast<std::size_t>(height - 1 - row)] =
              src.values[static_cast<std::size_t>(row * width + x)];

        switch (kind)
        {
        case SeismicAttrKind::Envelope:
        case SeismicAttrKind::InstPhase:
        case SeismicAttrKind::InstFreq:
        case SeismicAttrKind::InstQ:
        case SeismicAttrKind::Sweetness:
        {
          const paleo::seisattr::ComplexTraceResult ct =
              paleo::seisattr::complexTraceAnalysis(trace.data(), height, dtMs);
          if (kind == SeismicAttrKind::Envelope)
            attr = ct.envelope;
          else if (kind == SeismicAttrKind::InstPhase)
            attr = ct.phaseDeg;
          else if (kind == SeismicAttrKind::InstFreq)
            attr = ct.freqHz;
          else if (kind == SeismicAttrKind::InstQ)
            attr = ct.quality;
          else
            paleo::seisattr::sweetness(ct.envelope.data(), ct.freqHz.data(),
                                       height, attr.data());
          break;
        }
        case SeismicAttrKind::Rms:
          paleo::seisattr::windowedRms(trace.data(), height,
                                       params.windowHalfSamples, attr.data());
          break;
        case SeismicAttrKind::MaxAbs:
          paleo::seisattr::windowedMaxAbs(trace.data(), height,
                                          params.windowHalfSamples, attr.data());
          break;
        case SeismicAttrKind::MeanEnergy:
          paleo::seisattr::windowedMeanEnergy(trace.data(), height,
                                              params.windowHalfSamples,
                                              attr.data());
          break;
        case SeismicAttrKind::Coherence:
          break; // 已在上面整体处理
        }

        for (int row = 0; row < height; ++row)
          out->values[static_cast<std::size_t>(row * width + x)] =
              attr[static_cast<std::size_t>(height - 1 - row)];
      };

      const unsigned int hw = std::thread::hardware_concurrency();
      const int nThreads = std::max(1, std::min<int>(4, int(hw)));
      const int chunk = (width + nThreads - 1) / nThreads;
      auto runRange = [&](int x0, int xEnd, bool report) {
        for (int x = x0; x < xEnd && !cancelled(); ++x)
        {
          processColumn(x);
          if (report && ((x & 15) == 0 || x == xEnd - 1))
            task->reportBytes(totalUnits - sliceUnits +
                                  qint64(x - x0 + 1) * height,
                              totalUnits);
        }
      };
      std::vector<std::thread> workers;
      for (int t = 1; t < nThreads; ++t)
      {
        const int x0 = std::min(width, t * chunk);
        const int x1 = std::min(width, x0 + chunk);
        if (x0 >= x1)
          break;
        workers.emplace_back(runRange, x0, x1, /*report=*/false);
      }
      runRange(0, std::min(width, chunk), /*report=*/true);
      for (auto &w : workers)
        w.join();
      if (cancelled())
        return QString();
    }
    result->computeMs = double(clock.elapsed());

    // 道统计（列粒度）+ 值域（NaN 感知）——色标/Recolorize 用；全 NaN 时
    // 值域保持 0/0。
    result->validTraceCount = 0;
    float vMin = std::numeric_limits<float>::max();
    float vMax = std::numeric_limits<float>::lowest();
    for (int x = 0; x < width; ++x)
    {
      bool anyFinite = false;
      for (int row = 0; row < height; ++row)
      {
        const float v = out->values[std::size_t(row * width + x)];
        if (!std::isfinite(v))
          continue;
        anyFinite = true;
        vMin = v < vMin ? v : vMin;
        vMax = v > vMax ? v : vMax;
      }
      if (anyFinite)
        ++result->validTraceCount;
    }
    if (vMin <= vMax)
    {
      out->valueMin = vMin;
      out->valueMax = vMax;
    }
    result->ok = true;
    result->image = out;
    return QString();
  };

  PaleoTask *task = startBounded(title, work, QString(), /*quiet=*/false);
  connect(task, &PaleoTask::finished, this, [task, result, onFinished]() {
    if (!onFinished)
      return;
    if (task->state() == PaleoTask::State::Succeeded)
      onFinished(true, *result);
    else if (task->state() == PaleoTask::State::Cancelled)
    {
      SeismicAttrResult r;
      r.attrId = result->attrId;
      r.sectionType = result->sectionType;
      r.sectionIndex = result->sectionIndex;
      r.error = QStringLiteral("属性计算已取消");
      onFinished(false, r);
    }
    else
      onFinished(false, *result);
  });
  return task;
}

QString SeismicTaskService::registerAttributeSliceAsset(
    DataCatalog *catalog, const QString &seismicAssetId,
    const QString &seismicVersionId, const SeismicAttrResult &result,
    const SeismicAttrParams &params, const QString &sourceSgyPath,
    const QString &outputDir, QString *error)
{
  if (!catalog || !result.ok || !result.image)
  {
    if (error)
      *error = QStringLiteral("catalog 未设置或属性结果无效");
    return QString();
  }
  QDir().mkpath(outputDir);
  const QString sectionKey =
      result.sectionType == SgySliceType::Inline
          ? QStringLiteral("il")
          : QStringLiteral("xl");
  const QString fileName = QStringLiteral("%1_%2_%3.sattr")
                               .arg(result.attrId, sectionKey)
                               .arg(result.sectionIndex);
  const QString filePath = outputDir + QLatin1Char('/') + fileName;

  // SATR 容器：魔数 + 版本 + width/height + JSON 头长 + JSON + 小端 f32 值块。
  QJsonObject header;
  header.insert(QStringLiteral("attrId"), result.attrId);
  header.insert(QStringLiteral("section"), sectionKey);
  header.insert(QStringLiteral("sectionIndex"), result.sectionIndex);
  header.insert(QStringLiteral("width"), result.image->width);
  header.insert(QStringLiteral("height"), result.image->height);
  header.insert(QStringLiteral("valueMin"), double(result.image->valueMin));
  header.insert(QStringLiteral("valueMax"), double(result.image->valueMax));
  header.insert(QStringLiteral("traceCount"), result.traceCount);
  header.insert(QStringLiteral("validTraceCount"), result.validTraceCount);
  header.insert(QStringLiteral("readMs"), result.readMs);
  header.insert(QStringLiteral("computeMs"), result.computeMs);
  header.insert(QStringLiteral("sourceSgyPath"), sourceSgyPath);
  header.insert(QStringLiteral("createdAt"),
                QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
  QJsonObject p;
  p.insert(QStringLiteral("windowHalfSamples"), params.windowHalfSamples);
  p.insert(QStringLiteral("coherenceIlHalf"), params.coherenceIlHalf);
  p.insert(QStringLiteral("coherenceXlHalf"), params.coherenceXlHalf);
  p.insert(QStringLiteral("coherenceTimeHalf"), params.coherenceTimeHalf);
  header.insert(QStringLiteral("params"), p);
  const QByteArray json = QJsonDocument(header).toJson(QJsonDocument::Compact);

  QFile f(filePath);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    if (error)
      *error = QStringLiteral("无法写属性文件 %1").arg(filePath);
    return QString();
  }
  QDataStream ds(&f);
  ds.setByteOrder(QDataStream::LittleEndian);
  ds.setFloatingPointPrecision(QDataStream::SinglePrecision);
  ds.writeRawData("SATR", 4);
  ds << quint32(1) << qint32(result.image->width) << qint32(result.image->height)
     << quint32(json.size());
  ds.writeRawData(json.constData(), json.size());
  for (const float v : result.image->values)
    ds << v;
  f.close();

  CatalogAsset asset;
  asset.id = QStringLiteral("seis_attr_%1_%2_%3_%4")
                 .arg(seismicAssetId, result.attrId, sectionKey)
                 .arg(result.sectionIndex);
  asset.type = QStringLiteral("seismic_attribute");
  asset.format = QStringLiteral("sattr");
  asset.displayName = QStringLiteral("%1 %2 %3（地震属性）")
                          .arg(result.attrId, sectionKey)
                          .arg(result.sectionIndex);
  catalog->addAsset(asset);

  CatalogVersion v;
  v.id = QStringLiteral("ver_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
  v.assetId = asset.id;
  v.stage = QStringLiteral("DERIVED");
  v.versionNumber = 1;
  v.managed = false; // 外链：产物在调用方指定目录
  v.path = filePath;
  v.sourceUri = seismicAssetId;
  v.sha256 = sha256OfFile(filePath);
  v.fileName = fileName;
  v.parentVersionIds = QStringList{seismicVersionId};
  v.extra.insert(QStringLiteral("origin"), QStringLiteral("seismic-attributes"));
  v.extra.insert(QStringLiteral("attrId"), result.attrId);
  v.extra.insert(QStringLiteral("section"), sectionKey);
  v.extra.insert(QStringLiteral("sectionIndex"), result.sectionIndex);
  if (!catalog->addVersion(v))
  {
    if (error)
      *error = QStringLiteral("catalog 版本登记失败");
    return QString();
  }
  return filePath;
}

bool SeismicTaskService::saveSession(const SeismicInterpretationSession &session, QString *error)
{
  if (session.sourceSgyPath.isEmpty())
  {
    if (error)
      *error = QStringLiteral("会话无源 SEG-Y 锚");
    return false;
  }
  const QString path = session.sourceSgyPath + QStringLiteral(".seispicks.json");
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    if (error)
      *error = QStringLiteral("无法写会话文件 %1").arg(path);
    return false;
  }
  QJsonObject root;
  root.insert("format", QStringLiteral("paleo-seis-interpretation"));
  root.insert("version", 1);
  root.insert("name", session.name);
  root.insert("source", session.sourceSgyPath);
  root.insert("interpreters", QJsonArray::fromStringList(session.interpreters));
  root.insert("nextId", session.nextId);
  QJsonArray pickArr;
  for (const SeismicPick &p : session.picks)
  {
    QJsonObject o;
    o.insert("id", p.id);
    o.insert("inline", p.inlineNo);
    o.insert("xline", p.xlineNo);
    o.insert("twtMs", p.twtMs);
    o.insert("sample", p.sampleIndex);
    o.insert("confidence", double(p.confidence));
    o.insert("interpreter", p.interpreter);
    o.insert("horizon", p.horizonName);
    pickArr.append(o);
  }
  root.insert("picks", pickArr);
  QJsonArray faultArr;
  for (const SeismicFaultSegment &seg : session.faults)
  {
    QJsonObject o;
    o.insert("id", seg.id);
    o.insert("sectionType", int(seg.sectionType));
    o.insert("sectionIndex", seg.sectionIndex);
    o.insert("interpreter", seg.interpreter);
    o.insert("name", seg.name);
    QJsonArray pts;
    for (const auto &pt : seg.points)
    {
      pts.append(QJsonArray{pt.first, pt.second});
    }
    o.insert("points", pts);
    faultArr.append(o);
  }
  root.insert("faults", faultArr);
  f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
  f.close();
  return true;
}

bool SeismicTaskService::loadSession(const QString &sgyPath,
                                     SeismicInterpretationSession &out, QString *error)
{
  const QString path = sgyPath + QStringLiteral(".seispicks.json");
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
  {
    if (error)
      *error = QStringLiteral("无会话文件 %1").arg(path);
    return false;
  }
  const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
  if (!doc.isObject())
  {
    if (error)
      *error = QStringLiteral("会话文件损坏");
    return false;
  }
  const QJsonObject root = doc.object();
  out = SeismicInterpretationSession{};
  out.name = root.value("name").toString();
  out.sourceSgyPath = sgyPath;
  out.nextId = root.value("nextId").toInt(1);
  for (const auto &v : root.value("interpreters").toArray())
    out.interpreters << v.toString();
  for (const auto &v : root.value("picks").toArray())
  {
    const QJsonObject o = v.toObject();
    SeismicPick p;
    p.id = o.value("id").toInt();
    p.inlineNo = o.value("inline").toInt();
    p.xlineNo = o.value("xline").toInt();
    p.twtMs = o.value("twtMs").toDouble();
    p.sampleIndex = o.value("sample").toInt();
    p.confidence = float(o.value("confidence").toDouble(1.0));
    p.interpreter = o.value("interpreter").toString();
    p.horizonName = o.value("horizon").toString();
    out.picks.append(p);
  }
  for (const auto &v : root.value("faults").toArray())
  {
    const QJsonObject o = v.toObject();
    SeismicFaultSegment seg;
    seg.id = o.value("id").toInt();
    seg.sectionType = static_cast<SgySliceType>(o.value("sectionType").toInt());
    seg.sectionIndex = o.value("sectionIndex").toInt();
    seg.interpreter = o.value("interpreter").toString();
    seg.name = o.value("name").toString();
    for (const auto &pv : o.value("points").toArray())
    {
      const QJsonArray pa = pv.toArray();
      if (pa.size() == 2)
        seg.points.append({pa[0].toDouble(), pa[1].toDouble()});
    }
    out.faults.append(seg);
  }
  return true;
}

bool SeismicTaskService::exportPicksCsv(const QList<SeismicPick> &picks,
                                        const QString &filePath, QString *error)
{
  QFile f(filePath);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    if (error)
      *error = QStringLiteral("无法写 %1").arg(filePath);
    return false;
  }
  f.write("id,inline,xline,twt_ms,sample,confidence,interpreter,horizon\n");
  for (const SeismicPick &p : picks)
    f.write(QStringLiteral("%1,%2\n").arg(p.id).arg(horizonCsvLine(p)).toUtf8());
  f.close();
  return true;
}

// ---- D5.2 任意线提取缓存 -------------------------------------------------------

qint64 SeismicTaskService::sectionCacheKey(const std::vector<glm::ivec2> &pathPoints,
                                           std::shared_ptr<const SgyVolume> volume)
{
  // FNV-1a 状态用无符号：有符号乘法溢出是 UB（UBSan），无符号回绕良定义且位模式不变。
  quint64 h = 1469598103934665603ull;
  const auto mix = [&h](qint64 v) {
    h ^= static_cast<quint64>(v);
    h *= 1099511628211ull;
  };
  if (volume)
    mix(qint64(volume->Index() ? volume->Index()->fileSize : 0));
  for (const glm::ivec2 &pt : pathPoints)
  {
    mix(pt.x);
    mix(pt.y);
  }
  return static_cast<qint64>(h);
}

std::shared_ptr<const SgySliceImage> SeismicTaskService::cachedSection(
    const std::vector<glm::ivec2> &pathPoints,
    std::shared_ptr<const SgyVolume> volume) const
{
  const qint64 key = sectionCacheKey(pathPoints, volume);
  for (auto &entry : sectionCache_)
  {
    if (entry.key == key)
    {
      entry.lastUse = ++sectionCacheClock_;
      return entry.image;
    }
  }
  return nullptr;
}

void SeismicTaskService::cacheSection(const std::vector<glm::ivec2> &pathPoints,
                                      std::shared_ptr<const SgyVolume> volume,
                                      std::shared_ptr<const SgySliceImage> image)
{
  const qint64 key = sectionCacheKey(pathPoints, volume);
  for (auto it = sectionCache_.begin(); it != sectionCache_.end();)
    it = (it->key == key) ? sectionCache_.erase(it) : it + 1;
  sectionCache_.push_back({key, image, ++sectionCacheClock_});
  while (sectionCache_.size() > 4)
  {
    // 淘汰最久未用
    auto oldest = sectionCache_.begin();
    for (auto it = sectionCache_.begin(); it != sectionCache_.end(); ++it)
      if (it->lastUse < oldest->lastUse)
        oldest = it;
    sectionCache_.erase(oldest);
  }
}

// ---- D5.4 合成记录 -------------------------------------------------------------

SeismicTaskService::SeismicSyntheticResult SeismicTaskService::computeSyntheticSeismogram(
    const std::vector<double> &acDepthsM, const std::vector<float> &acUsPerM,
    const std::vector<double> &denDepthsM, const std::vector<float> &denValues,
    const TimeDepthModel &tdModel, double rickerHz)
{
  SeismicSyntheticResult result;
  if (acDepthsM.size() < 2 || acDepthsM.size() != acUsPerM.size())
  {
    result.reason = QStringLiteral("声波曲线（AC）缺失或不足 2 个采样点");
    return result;
  }
  if (denDepthsM.size() < 2 || denDepthsM.size() != denValues.size())
  {
    result.reason = QStringLiteral("密度曲线（DEN）缺失或不足 2 个采样点");
    return result;
  }
  if (!tdModel.isValid())
  {
    result.reason = QStringLiteral("时深表缺失或无效——合成记录需时深标定");
    return result;
  }

  // 公共深度轴（两曲线深度并集排序去重）+ 线性插值对齐
  std::vector<double> depths;
  depths.reserve(acDepthsM.size() + denDepthsM.size());
  depths.insert(depths.end(), acDepthsM.begin(), acDepthsM.end());
  depths.insert(depths.end(), denDepthsM.begin(), denDepthsM.end());
  std::sort(depths.begin(), depths.end());
  depths.erase(std::unique(depths.begin(), depths.end()), depths.end());
  const auto interp = [](const std::vector<double> &xs, const std::vector<float> &ys,
                         double x) -> float {
    if (x <= xs.front())
      return ys.front();
    if (x >= xs.back())
      return ys.back();
    const auto it = std::lower_bound(xs.begin(), xs.end(), x);
    const std::size_t hi = std::size_t(it - xs.begin());
    const std::size_t lo = hi - 1;
    const double t = (x - xs[lo]) / std::max(1e-9, xs[hi] - xs[lo]);
    return float(ys[lo] + t * (ys[hi] - ys[lo]));
  };

  // 波阻抗 Z = ρ · V（V = 1e6 / DT μs/m，m/s）
  std::vector<double> z;
  z.reserve(depths.size());
  for (double d : depths)
  {
    const float dtUs = interp(acDepthsM, acUsPerM, d);
    const float rho = interp(denDepthsM, denValues, d);
    const double v = dtUs > 1e-6 ? 1e6 / double(dtUs) : 0.0;
    z.push_back(double(rho) * v);
  }

  // 反射系数（层间）+ 时深转换（顶底 TWT 中点）
  std::vector<double> rcTwt;
  std::vector<float> rc;
  for (std::size_t i = 1; i < z.size(); ++i)
  {
    const double denom = z[i] + z[i - 1];
    if (denom < 1e-9)
      continue;
    const double r = (z[i] - z[i - 1]) / denom;
    if (std::abs(r) < 1e-10)
      continue;
    const double twt = tdModel.DepthToTwtMs((depths[i] + depths[i - 1]) * 0.5);
    if (!std::isfinite(twt) || twt <= 0.0)
      continue;
    rcTwt.push_back(twt);
    rc.push_back(float(r));
  }
  if (rcTwt.empty())
  {
    result.reason = QStringLiteral("反射系数序列为空（曲线平直或时深超出范围）");
    return result;
  }

  // Ricker 子波褶积：输出 2ms 采样网格
  const double dtMs = 2.0;
  const double tStart = *std::min_element(rcTwt.begin(), rcTwt.end()) - 100.0;
  const double tEnd = *std::max_element(rcTwt.begin(), rcTwt.end()) + 100.0;
  const double pi2 = 2.0 * std::acos(-1.0);
  const double f2 = rickerHz * rickerHz;
  for (double t = std::max(0.0, tStart); t <= tEnd; t += dtMs)
  {
    double amp = 0.0;
    for (std::size_t k = 0; k < rcTwt.size(); ++k)
    {
      const double tau = t - rcTwt[k]; // 褶积：子波平移到反射点
      const double a = pi2 * f2 * tau * tau / 1e6; // ms² → s² 折算在分子
      (void)a;
      const double arg = pi2 * f2 * (tau / 1000.0) * (tau / 1000.0);
      amp += double(rc[k]) * (1.0 - 2.0 * arg) * std::exp(-arg);
    }
    result.twtMs.push_back(t);
    result.amplitude.push_back(float(amp));
  }
  // 归一化到 [-1,1]
  float maxAbs = 1e-12f;
  for (float v : result.amplitude)
    maxAbs = std::max(maxAbs, std::abs(v));
  for (float &v : result.amplitude)
    v /= maxAbs;
  result.sampleCount = int(result.amplitude.size());
  result.ok = result.sampleCount > 4;
  if (!result.ok)
    result.reason = QStringLiteral("褶积输出为空");
  return result;
}

// ---- D6 性能与可靠性 -----------------------------------------------------------

SeismicTaskService::SeismicErrorCategory SeismicTaskService::SeismicErrorCategory::classify(
    const QString &error, bool glContextFailed)
{
  SeismicErrorCategory out;
  if (glContextFailed)
  {
    out.kind = Kind::GlUnavailable;
    out.userText = QStringLiteral("OpenGL 不可用（驱动/软渲染缺失）——三维视口已回退 2D 拼接视图");
    return out;
  }
  if (error.isEmpty())
  {
    out.kind = Kind::None;
    return out;
  }
  if (error.contains(QStringLiteral("不存在")) || error.contains(QStringLiteral("无法打开")) ||
      error.contains(QStringLiteral("No such file")) || error.contains(QStringLiteral("cannot open")))
  {
    out.kind = Kind::FileMissing;
    out.userText = QStringLiteral("地震文件缺失或不可读：%1").arg(error);
    return out;
  }
  if (error.contains(QStringLiteral("索引")) || error.contains(QStringLiteral("index")) ||
      error.contains(QStringLiteral("corrupt")) || error.contains(QStringLiteral("损坏")))
  {
    out.kind = Kind::IndexCorrupt;
    out.userText = QStringLiteral("索引损坏或不完整（将自动重建）：%1").arg(error);
    return out;
  }
  if (error.contains(QStringLiteral("内存")) || error.contains(QStringLiteral("memory")) ||
      error.contains(QStringLiteral("bad_alloc")))
  {
    out.kind = Kind::MemoryBudget;
    out.userText = QStringLiteral("内存预算超限——建议启用 .sf3p 分页通道（按页取数）");
    return out;
  }
  if (error.contains(QStringLiteral("取消")))
  {
    out.kind = Kind::Cancelled;
    out.userText = error;
    return out;
  }
  out.kind = Kind::Other;
  out.userText = error;
  return out;
}

qint64 SeismicTaskService::totalRamBytes()
{
#if defined(Q_OS_UNIX)
  const long pages = sysconf(_SC_PHYS_PAGES);
  const long pageSize = sysconf(_SC_PAGESIZE);
  if (pages > 0 && pageSize > 0)
    return qint64(pages) * pageSize;
  return 0;
#elif defined(Q_OS_WINDOWS)
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (GlobalMemoryStatusEx(&status))
    return qint64(status.ullTotalPhys);
  return 0;
#else
  return 0;
#endif
}

SeismicTaskService::SeismicMemoryReport SeismicTaskService::assessMemoryBudget(
    qint64 volumeBytes, qint64 totalRamOverride)
{
  SeismicMemoryReport report;
  report.volumeBytes = volumeBytes;
  report.totalRamBytes = totalRamOverride > 0 ? totalRamOverride : totalRamBytes();
  report.budgetBytes = report.totalRamBytes / 2;
  report.overBudget = report.totalRamBytes > 0 && volumeBytes > report.budgetBytes;
  if (report.overBudget)
    report.recommendation = QStringLiteral(
        "体 %1 GB 超过内存预算（RAM/2 ≈ %2 GB）——建议转码 .sf3p 分页工作区并启用分页通道")
        .arg(volumeBytes / 1073741824.0, 0, 'f', 1)
        .arg(report.budgetBytes / 1073741824.0, 0, 'f', 1);
  return report;
}

qint64 SeismicTaskService::estimatedMemoryBytes() const
{
  qint64 total = 0;
  // 数据缓存按预算上限估（LRU 上界）
  total += qint64(dataCache_.Budget());
  // 任意线缓存按实占
  for (const SectionCacheEntry &e : sectionCache_)
    if (e.image)
      total += qint64(e.image->values.size()) * 4 + qint64(e.image->rgba.size());
  // 数据集条目：每条 chunk 256MB + slice 64MB 缓存预算（引擎契约上限）
  QMutexLocker lock(&registry_->mutex);
  total += qint64(registry_->entries.size()) * (256 + 64) * 1024 * 1024;
  return total;
}

// D6.4 并发闸：≤4 个地震任务同时执行（信号量槽位；排队者阻塞在信号量上
// 不耗 CPU，获取后先查取消再干活——取消不悬挂）。闸以 shared_ptr 由 worker
// 携带：服务析构时在途 worker 安全退出（同 registry 析构竞态模式）。
PaleoTask *SeismicTaskService::startBounded(const QString &title,
                                            const std::function<QString(PaleoTask *)> &work,
                                            const QString &layerId, bool quiet)
{
  const auto gate = gate_; // shared_ptr：析构安全
  gate->active.fetch_add(1);
  const std::function<QString(PaleoTask *)> gated =
      [gate, work](PaleoTask *task) -> QString {
        gate->slotSemaphore.acquire();
        struct SemaphoreGuard {
          QSemaphore &sem;
          ~SemaphoreGuard() { sem.release(); }
        } guard{gate->slotSemaphore};

        return (task && task->cancelRequested()) ? QString() : (work ? work(task) : QString());
      };
  PaleoTask *task = taskService_->start(title, gated, layerId, quiet);
  QObject::connect(task, &PaleoTask::finished, task, [gate]() {
    gate->active.fetch_sub(1);
  });
  return task;
}

} // namespace seismic
