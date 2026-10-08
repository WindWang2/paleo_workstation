// 层：数据
#include "services/seismictaskservice.h"
#include "services/seismictaskservice_internal.h"
#include "services/fspathutils.h"
#include "services/paleotaskservice.h"

#include "catalog/datacatalog.h"
#include "io/sattrio.h"
#include "Engine/QuickOpen.h"
#include "Engine/PagedWorkspace.h"
#include "Engine/Sdk.h"
#include "Engine/Types.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutexLocker>
#include <QPointer>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace seismic {

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
        paleo::toFsPath(sgyPath), maxColumns, &cancel);

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
    auto dataset = sdk::Dataset::Open(paleo::toFsPath(sgyPath),
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

// ---- 3D 体视消费（goal/attr-volume）----------------------------------------

namespace
{

// NaN 感知面值域（显示归一化用；全 NaN 面 valueMin/Max 保持 0/0）。
void attrPlaneRange(const float *v, std::size_t n, float *vmin, float *vmax)
{
  float lo = std::numeric_limits<float>::max();
  float hi = std::numeric_limits<float>::lowest();
  for (std::size_t i = 0; i < n; ++i)
  {
    if (!std::isfinite(v[i]))
      continue;
    if (v[i] < lo)
      lo = v[i];
    if (v[i] > hi)
      hi = v[i];
  }
  if (lo <= hi)
  {
    *vmin = lo;
    *vmax = hi;
  }
}

} // namespace

SeismicTaskService::AttributeVolumePreview
SeismicTaskService::loadAttributeVolumePreview(const QString &path)
{
  AttributeVolumePreview preview;
  paleo::sattr::SattrVolumeReader reader;
  QString err;
  if (!reader.open(path, &err))
  {
    preview.error = err;
    return preview;
  }
  const paleo::sattr::SattrVolumeInfo &info = reader.info();
  preview.nIl = info.nIl;
  preview.nXl = info.nXl;
  preview.nS = info.nS;
  const int ilMid = info.nIl / 2;
  const int xlMid = info.nXl / 2;
  const int sMid = info.nS / 2;
  preview.inlineIdx = ilMid;
  preview.xlineIdx = xlMid;
  preview.sampleIdx = sMid;

  // 三中位面（引擎切片图像约定：剖面 row0=最深；时间片 row0=最大 inline）。
  std::vector<float> plane;
  if (!reader.extractInline(ilMid, &plane, &err))
  {
    preview.error = err;
    return preview;
  }
  preview.inlineSlice.width = info.nXl;
  preview.inlineSlice.height = info.nS;
  preview.inlineSlice.values.resize(std::size_t(info.nXl) * info.nS);
  for (int row = 0; row < info.nS; ++row)
    for (int xl = 0; xl < info.nXl; ++xl)
      preview.inlineSlice.values[std::size_t(row * info.nXl + xl)] =
          plane[std::size_t(xl) * info.nS + std::size_t(info.nS - 1 - row)];
  attrPlaneRange(preview.inlineSlice.values.data(),
                 preview.inlineSlice.values.size(),
                 &preview.inlineSlice.valueMin, &preview.inlineSlice.valueMax);

  if (!reader.extractXline(xlMid, &plane, &err))
  {
    preview.error = err;
    return preview;
  }
  preview.xlineSlice.width = info.nIl;
  preview.xlineSlice.height = info.nS;
  preview.xlineSlice.values.resize(std::size_t(info.nIl) * info.nS);
  for (int row = 0; row < info.nS; ++row)
    for (int il = 0; il < info.nIl; ++il)
      preview.xlineSlice.values[std::size_t(row * info.nIl + il)] =
          plane[std::size_t(il) * info.nS + std::size_t(info.nS - 1 - row)];
  attrPlaneRange(preview.xlineSlice.values.data(),
                 preview.xlineSlice.values.size(),
                 &preview.xlineSlice.valueMin, &preview.xlineSlice.valueMax);

  // 堆叠层采样位（含中位面，均布 ≤16 层；单遍块读）。
  const int layers = std::min(kMaxPropertyStackLayers, info.nS);
  std::vector<int> kIdx;
  kIdx.reserve(std::size_t(layers));
  for (int k = 0; k < layers; ++k)
  {
    const int s = (info.nS - 1) * k / (layers - 1 > 0 ? layers - 1 : 1);
    kIdx.push_back(s);
  }
  std::vector<std::vector<float>> timePlanes;
  if (!reader.extractTimePlanes(kIdx, &timePlanes, &err))
  {
    preview.error = err;
    return preview;
  }
  preview.stackLayerCount = layers;
  for (int k = 0; k < layers; ++k)
  {
    preview.stackKIndexes[k] = kIdx[std::size_t(k)];
    SgySliceImage &img = preview.stackLayers[k];
    img.width = info.nXl;
    img.height = info.nIl;
    img.values = std::move(timePlanes[std::size_t(k)]); // [il*nXl+xl]，row0=il0
    // 时间片槽约定 row0=最大 inline：整行交换翻序（列序不动）。
    for (int r = 0; r < info.nIl / 2; ++r)
      std::swap_ranges(
          img.values.begin() + std::ptrdiff_t(r) * info.nXl,
          img.values.begin() + std::ptrdiff_t(r + 1) * info.nXl,
          img.values.begin() + std::ptrdiff_t(info.nIl - 1 - r) * info.nXl);
    attrPlaneRange(img.values.data(), img.values.size(), &img.valueMin,
                   &img.valueMax);
  }
  // 中位时间面 = 最接近 sMid 的堆叠层（同一约定，复用）。
  int best = 0;
  int bestDist = std::abs(kIdx[0] - sMid);
  for (int k = 1; k < layers; ++k)
  {
    const int d = std::abs(kIdx[std::size_t(k)] - sMid);
    if (d < bestDist)
      bestDist = d;
    best = k;
  }
  preview.timeSlice = preview.stackLayers[std::size_t(best)];
  preview.sampleIdx = preview.stackKIndexes[best];
  preview.ok = true;
  return preview;
}

QString SeismicTaskService::registerTimeSliceAttributeAsset(
    DataCatalog *catalog, const QString &seismicAssetId,
    const QString &seismicVersionId, SeismicAttrKind kind,
    const SeismicAttrParams &params,
    const SeismicAttrTimeSliceResult &result, const QString &sourceSgyPath,
    const QString &outputDir, QString *error, LayerDeclaration *layerOut)
{
  if (!catalog || !result.ok)
  {
    if (error)
      *error = QStringLiteral("catalog 未设置或扫描结果无效");
    return QString();
  }
  const QString tifPath = result.cacheHit
      ? result.cachePath
      : attrCacheFilePath(outputDir, result.attrId,
                          QStringLiteral("ts"), result.paramHash,
                          QStringLiteral("tif"));
  if (tifPath.isEmpty() || !QFileInfo::exists(tifPath))
  {
    if (error)
      *error = QStringLiteral("时间切片栅格产物缺失（%1）——无缓存目录的"
                              "扫描不产栅格，无法登记上图")
                   .arg(tifPath);
    return QString();
  }

  const QString assetId = QStringLiteral("seis_attr_%1_%2_ts_%3")
                              .arg(seismicAssetId, result.attrId)
                              .arg(result.sampleIndex);
  CatalogAsset asset;
  asset.id = assetId;
  asset.type = QStringLiteral("seismic_attribute");
  asset.format = QStringLiteral("tif");
  asset.displayName = QStringLiteral("%1 时间切片 t=%2ms（地震属性）")
                          .arg(seismicAttrDisplayName(kind))
                          .arg(int(result.timeMs));
  catalog->addAsset(asset);

  // 同参数版本已登记 → 复用（不重复建版本）；命中事实在 extra 可见。
  // 复用既有版本的前提：param_hash 一致**且**版本产物仍在盘上（跨会话
  // 产物可能被清理——死 URI 不冒充成功，走重新登记指向当前产物）。
  const CatalogVersion existing = catalog->currentVersion(assetId);
  if (!existing.id.isEmpty() &&
      existing.extra.value(QStringLiteral("param_hash")).toString() ==
          result.paramHash &&
      QFileInfo::exists(existing.path))
  {
    if (layerOut)
    {
      layerOut->layerId = QStringLiteral("seisattr_ts.%1.t%2")
                              .arg(result.attrId)
                              .arg(result.sampleIndex);
      layerOut->type = QStringLiteral("raster");
      layerOut->source = existing.path;
      layerOut->group = QStringLiteral("00_Data");
      layerOut->title = asset.displayName;
      layerOut->horizon = QStringLiteral("seisattr.%1").arg(result.attrId);
    }
    return existing.path;
  }

  const QString fileName = QFileInfo(tifPath).fileName();
  CatalogVersion v;
  v.id = QStringLiteral("ver_%1").arg(
      QUuid::createUuid().toString(QUuid::WithoutBraces));
  v.assetId = assetId;
  v.stage = QStringLiteral("DERIVED");
  v.versionNumber = 1;
  v.managed = false; // 外链：产物在调用方指定目录
  v.path = tifPath;
  v.sourceUri = seismicAssetId;
  v.sha256 = sha256OfFile(tifPath);
  v.fileName = fileName;
  v.parentVersionIds = QStringList{seismicVersionId};
  v.extra.insert(QStringLiteral("origin"), QStringLiteral("seismic-attributes"));
  v.extra.insert(QStringLiteral("attrId"), result.attrId);
  v.extra.insert(QStringLiteral("scope"), QStringLiteral("time-slice"));
  v.extra.insert(QStringLiteral("timeSampleIndex"), result.sampleIndex);
  v.extra.insert(QStringLiteral("timeMs"), result.timeMs);
  v.extra.insert(QStringLiteral("param_hash"), result.paramHash);
  v.extra.insert(QStringLiteral("cache_hit"), result.cacheHit);
  v.extra.insert(QStringLiteral("params_windowHalfSamples"),
                params.windowHalfSamples);
  v.extra.insert(QStringLiteral("params_coherenceIlHalf"),
                params.coherenceIlHalf);
  v.extra.insert(QStringLiteral("params_coherenceXlHalf"),
                params.coherenceXlHalf);
  v.extra.insert(QStringLiteral("params_coherenceTimeHalf"),
                params.coherenceTimeHalf);
  v.extra.insert(QStringLiteral("params_coherenceWeighting"),
                params.coherenceWeighting);
  v.extra.insert(QStringLiteral("source_sgy"), sourceSgyPath);
  if (!catalog->addVersion(v))
  {
    if (error)
      *error = QStringLiteral("catalog 版本登记失败");
    return QString();
  }
  if (layerOut)
  {
    layerOut->layerId = QStringLiteral("seisattr_ts.%1.t%2")
                            .arg(result.attrId)
                            .arg(result.sampleIndex);
    layerOut->type = QStringLiteral("raster");
    layerOut->source = tifPath;
    layerOut->group = QStringLiteral("00_Data");
    layerOut->title = asset.displayName;
    layerOut->horizon = QStringLiteral("seisattr.%1").arg(result.attrId);
  }
  return tifPath;
}

QString SeismicTaskService::registerAttributeVolumeAsset(
    DataCatalog *catalog, const QString &seismicAssetId,
    const QString &seismicVersionId, SeismicAttrKind kind,
    const SeismicAttrParams &params, const SeismicAttrVolumeResult &result,
    const QString &sourceSgyPath, QString *error)
{
  if (!catalog || !result.ok || result.path.isEmpty() ||
      !QFileInfo::exists(result.path))
  {
    if (error)
      *error = QStringLiteral("catalog 未设置或属性体产物无效");
    return QString();
  }
  const QString assetId = QStringLiteral("seis_attr_%1_%2_vol")
                              .arg(seismicAssetId, result.attrId);
  CatalogAsset asset;
  asset.id = assetId;
  asset.type = QStringLiteral("seismic_attribute");
  asset.format = QStringLiteral("sattr");
  asset.displayName =
      QStringLiteral("%1 属性体（地震属性）").arg(seismicAttrDisplayName(kind));
  catalog->addAsset(asset);

  const CatalogVersion existing = catalog->currentVersion(assetId);
  if (!existing.id.isEmpty() &&
      existing.extra.value(QStringLiteral("param_hash")).toString() ==
          result.paramHash &&
      QFileInfo::exists(existing.path))
  {
    return existing.path; // 同参数版本已登记且产物在盘 → 复用
  }

  const QString fileName = QFileInfo(result.path).fileName();
  CatalogVersion v;
  v.id = QStringLiteral("ver_%1").arg(
      QUuid::createUuid().toString(QUuid::WithoutBraces));
  v.assetId = assetId;
  v.stage = QStringLiteral("DERIVED");
  v.versionNumber = 1;
  v.managed = false;
  v.path = result.path;
  v.sourceUri = seismicAssetId;
  v.sha256 = sha256OfFile(result.path);
  v.fileName = fileName;
  v.parentVersionIds = QStringList{seismicVersionId};
  v.extra.insert(QStringLiteral("origin"), QStringLiteral("seismic-attributes"));
  v.extra.insert(QStringLiteral("attrId"), result.attrId);
  v.extra.insert(QStringLiteral("scope"), QStringLiteral("volume"));
  v.extra.insert(QStringLiteral("volume"), true);
  v.extra.insert(QStringLiteral("param_hash"), result.paramHash);
  v.extra.insert(QStringLiteral("cache_hit"), result.cacheHit);
  v.extra.insert(QStringLiteral("params_windowHalfSamples"),
                params.windowHalfSamples);
  v.extra.insert(QStringLiteral("params_coherenceIlHalf"),
                params.coherenceIlHalf);
  v.extra.insert(QStringLiteral("params_coherenceXlHalf"),
                params.coherenceXlHalf);
  v.extra.insert(QStringLiteral("params_coherenceTimeHalf"),
                params.coherenceTimeHalf);
  v.extra.insert(QStringLiteral("params_coherenceWeighting"),
                params.coherenceWeighting);
  v.extra.insert(QStringLiteral("source_sgy"), sourceSgyPath);
  if (!catalog->addVersion(v))
  {
    if (error)
      *error = QStringLiteral("catalog 版本登记失败");
    return QString();
  }
  return result.path;
}

} // namespace seismic
