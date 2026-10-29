// 层：数据
#include "services/seismictaskservice.h"
#include "services/seismictaskservice_internal.h"
#include "services/fspathutils.h"
#include "seismicdataaccess.h"
#include "services/paleotaskservice.h"

#include "domain/seismic/sgyindexbuilder.h"
#include "Data/Sgy/SgyIndexService.h"
#include "domain/seismic/sgyindexcache.h"
#include "Engine/PagedPipeline.h"
#include "Engine/PagedWorkspace.h"
#include "Engine/TranscodeJob.h"
#include "Engine/Types.h"
#include "Engine/WorkspaceFormat.h"
// readTraceHeader 首道偏移与 vendor 读取通道同源（segyio metadata.trace0）
#include <segyio/segy.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>

#include <algorithm>
#include <cmath>

namespace seismic {

namespace {

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
  int bytesPerSample = 4; // 1=IBM 2=int32 5=IEEE 均 4 字节
  if (format == 3)
    bytesPerSample = 2;   // int16
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
  report.sanitizedSamples = qint64(result.sanitizedSampleCount);
  report.sanitizedTraces = qint64(result.sanitizedTraceCount);
  report.damagedSample = damagedListFromEngine(result.damagedTraceSample);
  if (std::isfinite(result.valueMin) && std::isfinite(result.valueMax))
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
  report.sanitizedSamples = qint64(l0.sanitizedSampleCount);
  report.sanitizedTraces = qint64(l0.sanitizedTraceCount);
  report.damagedSample = damagedListFromEngine(l0.damagedTraceSample);
  if (std::isfinite(l0.valueMin) && std::isfinite(l0.valueMax))
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
  o.insert("sanitizedSamples", double(r.sanitizedSamples));
  o.insert("sanitizedTraces", double(r.sanitizedTraces));
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

PaleoTask *SeismicTaskService::startIndexing(
    const QString &sgyPath,
    bool forceReindex,
    std::function<void(bool, SgyIndexPtr, const QString &)> onFinished,
    const QString &layerId, bool quiet)
{
  if (!taskService_)
  {
    if (onFinished)
      onFinished(false, nullptr, QStringLiteral("PaleoTaskService not set"));
    return nullptr;
  }

  const QString title = tr("加载地震体索引: %1").arg(QFileInfo(sgyPath).fileName());
  auto resultIndex = std::make_shared<SgyIndexPtr>();

  auto work = [sgyPath, forceReindex, resultIndex](PaleoTask *task) -> QString {
    paleo::SeismicDataAccess source;QString error;if(!source.open(sgyPath,&error))return error;
    const auto stdPath=paleo::toFsPath(source.sourcePath());
    if(task->cancelRequested())return {};
    task->reportDetail(QCoreApplication::translate("seismic::SeismicTaskService","检查索引缓存并顺序建立完整道头索引…"));
    SgyIndexBuildRequest request;request.path=stdPath;request.useCache=!forceReindex;request.verbose=false;
    request.windowBytes=8u<<20;request.readQueueDepth=2;
    auto checkpointName=SgyIndexCache::CachePathFor(stdPath).filename();checkpointName+=".ckpt";
    if(!forceReindex)request.checkpointPath=SgyIndexCache::CacheDirectory()/"scan-checkpoints"/checkpointName;
    const auto outcome=BuildSgyIndexAuto(request,[task](int done,int total){task->reportBytes(done,total);return !task->cancelRequested();},[task]{return task->cancelRequested();});
    if(outcome.cancelled||task->cancelRequested())return {};
    const auto index=outcome.index;
    if(!index)return QString::fromUtf8(outcome.message);
    if(!index->FullyScanned())return QObject::tr("未获得完整道头索引");
    std::error_code ec;const auto size=std::filesystem::file_size(stdPath,ec);
    if(ec||size!=index->fileSize)return QObject::tr("索引期间地震来源已变化，请重新加载");
    const auto stamp=std::filesystem::last_write_time(stdPath,ec);
    if(ec||stamp.time_since_epoch().count()!=index->modifiedTimeTicks)return QObject::tr("索引期间地震来源已变化，请重新加载");
    task->reportDetail(QString::fromUtf8(outcome.strategy+" · "+outcome.cacheNote+" · "+outcome.checkpointNote));
    *resultIndex=index;

    return QString();
  };

  PaleoTask *task = startBounded(title, work, layerId, quiet); // D6.4 ≤4 并发闸
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
        paleo::toFsPath(sgyPath),
        paleo::toFsPath(base),
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

    const std::filesystem::path l0 = paleo::toFsPath(sf3pPath);
    *outPyramid = engine::BuildPagedPyramid(
        paleo::toFsPath(sgyPath), l0, options, &cancel, progress);
    *engineRan = true;

    // D1.9 L3（16x16x1，超大体量档）：直接从 L0 构建
    if (plannedLevels.contains(QLatin1String("L3")) &&
        outPyramid->l0.status.ok())
    {
      *outL3 = engine::BuildPagedLodFromL0(
          l0, engine::PagedLodPath(l0, 3), 3, 16, 16, 1, engine::LodMethod::Average,
          options, &cancel, progress);
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
    // 计划外的历史兄弟层级清理（旧计划留下的 L1/L2/L3 不再是事实）——
    // 必须在全部阶段校验通过之后：失败/取消路径上新金字塔未建成，先删
    // 旧层级会把可重建的派生产物销毁掉（工作区 LOD 静默降级）。
    for (int level = 1; level <= 3; ++level)
    {
      const QString tag = QStringLiteral("L%1").arg(level);
      if (plannedLevels.contains(tag))
        continue;
      std::error_code ec;
      std::filesystem::remove(engine::PagedLodPath(l0, level), ec);
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

// ---- D1.2/D1.6/D1.8 断点探测 -------------------------------------------------

SeismicWorkspaceProbe SeismicTaskService::probeWorkspace(const QString &sgyOrMetaPath) const
{
  SeismicWorkspaceProbe probe;
  QString base = sgyOrMetaPath;
  if (base.endsWith(QLatin1String(".sf3c.meta"), Qt::CaseInsensitive))
    base.chop(static_cast<int>(qstrlen(".sf3c.meta")));

  engine::WorkspaceMetaSummary summary;
  std::string error;
  engine::ProbeWorkspaceMeta(paleo::toFsPath(base), summary, error);
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
    if (reader.Open(paleo::toFsPath(sf3pPath), error, /*metadataOnly=*/true))
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
    if (reader.Open(paleo::toFsPath(partialPath), error, /*metadataOnly=*/true))
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
  if (sanitizedSamples > 0)
    line += QObject::tr(" · 清洗 %1 个非有限样点（%2 道）").arg(sanitizedSamples).arg(sanitizedTraces);
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

SeismicTraceHeaderInfo SeismicTaskService::readTraceHeader(const QString &sgyPath, int traceIndex,
                                                           const SgyIndexPtr &index)
{
  SeismicTraceHeaderInfo info;
  info.traceIndex = traceIndex;
  if (sgyPath.isEmpty() || traceIndex < 0)
  {
    info.error = QStringLiteral("无效道序号");
    return info;
  }

  // finding-94：本函数曾被剖面 dock 的道头卡在主线程调用，旧实现在此
  // SgyIndexCache::Load 全量加载 .sgyidx 只为道数上界检查——大体量测秒级
  // 冻结 UI。索引改由调用方传入其已在途的副本（dock 持 m_volume->Index()），
  // 无索引时样本元数据退二进制头、道数上界按 文件长/道长 纯头推算（与
  // sampleCount/formatCode 同源），彻底不再触碰磁盘索引。
  QFile file(sgyPath);
  if (!file.open(QIODevice::ReadOnly))
  {
    info.error = QStringLiteral("无法打开 SEG-Y 文件");
    return info;
  }
  // 3200 文本头 + 400 二进制头总是要读：样本元数据可走调用方在途索引，否则
  // 退二进制头（规则文件可靠）。首道偏移 trace0 只在二进制头里（rev2 首道
  // 偏移字段 3521 / rev0-1 扩展文本头数 3505→3200+400+N·3200）。旧实现硬编码
  // 3600——带扩展文本头的文件读到错误道的头（inline/xline/坐标全错）。规则与
  // vendor 读取通道同源（SgyVolume 走 segyio metadata.trace0），此处直接复用
  // segy_trace0。
  QByteArray binHdr = file.read(3600);
  if (binHdr.size() < 3600)
  {
    info.error = QStringLiteral("SEG-Y 头读取失败");
    return info;
  }

  // 样本数/格式码：调用方在途索引优先；未命中退二进制头（规则文件可靠）
  int sampleCount = 0;
  int formatCode = 5;
  double dtUs = 0.0;
  if (index)
  {
    sampleCount = index->sampleCount;
    formatCode = index->formatCode;
    dtUs = index->sampleIntervalUs;
  }
  if (sampleCount <= 0)
  {
    const auto be16h = [&](int at) -> int {
      return (quint8(binHdr[at]) << 8) | quint8(binHdr[at + 1]);
    };
    sampleCount = be16h(3220);
    formatCode = be16h(3224);
  }
  int bytesPerSample = 4; // 1=IBM 2=int32 5=IEEE 均 4 字节
  if (formatCode == 3)
    bytesPerSample = 2;   // int16
  else if (formatCode == 8)
    bytesPerSample = 1;   // int8
  else if (formatCode == 1 || formatCode == 5)
    bytesPerSample = 4;

  unsigned long long trace0 = 3600;
  segy_trace0(binHdr.constData() + 3200, &trace0, /*ext_textheader_count=*/-1);
  if (trace0 < 3600)
    trace0 = 3600; // 异常字段不放大成负/零偏移：回落常轨布局
  const qint64 traceBytes = 240 + qint64(sampleCount) * bytesPerSample;
  // 道数上界：在途索引给精确值；否则按 (文件长 − 首道偏移)/道长 纯头推算。
  qint64 traceCount = -1;
  if (index)
    traceCount = qint64(index->traces.size());
  else if (traceBytes > 0)
    traceCount = (file.size() - qint64(trace0)) / traceBytes;
  if (traceCount >= 0 && traceIndex >= traceCount)
  {
    info.error = QStringLiteral("道序号 %1 超出道数范围（共 %2 道）")
                     .arg(traceIndex)
                     .arg(traceCount);
    return info;
  }
  info.fileOffset = qint64(trace0) + qint64(traceIndex) * traceBytes;
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
