// 层：数据
#include "services/seismictaskservice.h"
#include "services/seismictaskservice_internal.h"
#include "services/paleotaskservice.h"

#include "catalog/datacatalog.h"
#include "domain/seismic/sectionaxis.h"
#include "domain/seismic/sgycoordinatemapper.h"
#include "domain/seismic/sgyio.h"
#include "algorithms/seismicattr.h"
#include "io/sattrio.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>

namespace seismic {

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
        timer->deleteLater(); // RUNTIME-04：不能在自身 timeout 栈上同步 delete
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
    // 输入形状诚实面：单采样时间切片不满足逐道族输入形状——瞬时族需整道
    // 谱（全样 Hilbert 变换）、时窗族需垂向窗 ±%1 样、相干需三维窗
    // （IL/XL 道窗 × 垂向窗）。体化扫描（startTimeSliceAttribute /
    // startAttributeVolume）按属性族各取所需窗口，请改走扫描入口。
    if (onFinished)
    {
      SeismicAttrResult r;
      r.error = QStringLiteral(
                    "时间切片（单采样面）不满足属性输入形状：瞬时族需整道谱、"
                    "时窗族需垂向窗 ±%1 样、相干需 IL/XL 道窗×垂向窗 ±%2 样——"
                    "请改用属性扫描（时间切片/属性体）")
                    .arg(params.windowHalfSamples)
                    .arg(params.coherenceTimeHalf);
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

  // SATR 容器（读写收敛在 io/sattrio——crossplot 读端共用同一格式定义）。
  paleo::sattr::SattrSectionHeader header;
  header.attrId = result.attrId;
  header.section = sectionKey;
  header.sectionIndex = result.sectionIndex;
  header.width = result.image->width;
  header.height = result.image->height;
  header.valueMin = result.image->valueMin;
  header.valueMax = result.image->valueMax;
  header.traceCount = result.traceCount;
  header.validTraceCount = result.validTraceCount;
  header.readMs = result.readMs;
  header.computeMs = result.computeMs;
  header.sourceSgyPath = sourceSgyPath;
  header.createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  header.windowHalfSamples = params.windowHalfSamples;
  header.coherenceIlHalf = params.coherenceIlHalf;
  header.coherenceXlHalf = params.coherenceXlHalf;
  header.coherenceTimeHalf = params.coherenceTimeHalf;
  header.coherenceWeighting = params.coherenceWeighting;
  QString writeErr;
  if (!paleo::sattr::writeSattrSection(filePath, header,
                                       QVector<float>(
                                           result.image->values.begin(),
                                           result.image->values.end()),
                                       &writeErr))
  {
    if (error)
      *error = writeErr;
    return QString();
  }

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

} // namespace seismic
