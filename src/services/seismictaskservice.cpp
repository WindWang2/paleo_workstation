// 层：数据
#include "services/seismictaskservice.h"
#include "services/seismictaskservice_internal.h"
#include "services/fspathutils.h"
#include "services/paleotaskservice.h"

#include <QCryptographicHash>
#include <QFile>
#include <QMutexLocker>
#include <QStringList>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#elif defined(Q_OS_WINDOWS)
#include <windows.h>
#endif

#include <filesystem>

namespace seismic {

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

// 按需打开并缓存条目。registry 以值参 shared_ptr 传入：worker 与服务共用，
// 生命周期自动延伸过任何在途 worker。
std::shared_ptr<SeismicDatasetEntry> datasetEntryFor(
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
  auto dataset = sdk::Dataset::Open(paleo::toFsPath(path), options, openStatus);
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

SeismicTaskService::~SeismicTaskService() = default;

int SeismicTaskService::activeTaskCount() const
{
  return gate_ ? gate_->active.load() : 0;
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
