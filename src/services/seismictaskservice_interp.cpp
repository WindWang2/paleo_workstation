// 层：数据
#include "services/seismictaskservice.h"
#include "services/seismictaskservice_internal.h"
#include "services/paleotaskservice.h"

#include "algorithms/horizontrack.h"
#include "catalog/datacatalog.h"
#include "domain/seismic/sectionaxis.h"
#include "io/horizonbinner.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <vector>

namespace seismic {

namespace {

QString horizonCsvLine(const SeismicPick &p)
{
  return QStringLiteral("%1,%2,%3,%4")
      .arg(p.inlineNo).arg(p.xlineNo)
      .arg(QString::number(p.twtMs, 'f', 2))
      .arg(QString::number(p.confidence, 'f', 3));
}

} // namespace

QStringList SeismicInterpretationSession::horizonNames() const
{
  QStringList names;
  for (const SeismicPick &p : picks)
    if (!p.horizonName.isEmpty() && !names.contains(p.horizonName))
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
  case paleo::hztrack::StopReason::ReadFailure:
    return QObject::tr("剖面读取失败");
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
      options.correlationThreshold, 0.0, options.dipHistoryPicks};
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

PaleoTask *SeismicTaskService::startVolumePropagation(
    std::shared_ptr<const SgyVolume> volume,
    const VolumePropagationRequest &request,
    std::function<void(bool ok, const QList<SeismicPick> &picks,
                       const SeismicTrackReport &report,
                       const QString &error)> onFinished)
{
  const auto failNow = [onFinished](const QString &why) {
    if (onFinished)
      onFinished(false, {}, SeismicTrackReport{}, why);
  };
  if (!taskService_)
  {
    failNow(QStringLiteral("PaleoTaskService not set"));
    return nullptr;
  }
  if (!volume || !volume->IsLoaded())
  {
    failNow(tr("地震体未加载（先完成体加载）"));
    return nullptr;
  }
  if (request.seeds.isEmpty())
  {
    failNow(tr("无种子（先在种子剖面手动拾取）"));
    return nullptr;
  }
  const std::vector<int> &ilAxis = volume->InlineValues();
  const std::vector<int> &xlAxis = volume->XlineValues();
  const int nS = volume->SampleCount();
  if (ilAxis.empty() || xlAxis.empty() || nS <= 0)
  {
    failNow(tr("体测网轴为空"));
    return nullptr;
  }
  const auto ilIt = std::find(ilAxis.begin(), ilAxis.end(), request.seedInline);
  if (ilIt == ilAxis.end())
  {
    failNow(tr("inline %1 不在本体内（无最近线替代）").arg(request.seedInline));
    return nullptr;
  }
  const int seedIlIdx = int(ilIt - ilAxis.begin());
  std::vector<paleo::hztrack::SeedPoint> kernelSeeds;
  kernelSeeds.reserve(std::size_t(request.seeds.size()));
  for (const QPair<int, int> &s : request.seeds)
  {
    const auto xlIt = std::find(xlAxis.begin(), xlAxis.end(), s.first);
    if (xlIt == xlAxis.end())
    {
      failNow(tr("种子 crossline %1 不在本体内（无最近线替代）").arg(s.first));
      return nullptr;
    }
    if (s.second < 0 || s.second >= nS)
    {
      failNow(tr("种子采样 %1 越界（体采样 0..%2）").arg(s.second).arg(nS - 1));
      return nullptr;
    }
    kernelSeeds.push_back({int(xlIt - xlAxis.begin()), s.second});
  }

  // 内存闸（诚实面）：滑窗工作集 = 种子 + 扫掠相邻 2 剖面 ×（行主序像 +
  // 道主序副本两份当量）——超预算如实拒绝并报数，不 OOM 硬扛
  const qint64 sectionBytes =
      qint64(volume->XlineCount()) * qint64(nS) * 4;
  const qint64 workingSetBytes = sectionBytes * 6;
  const qint64 budgetBytes =
      request.workingSetBudgetBytes > 0
          ? request.workingSetBudgetBytes
          : std::min<qint64>(totalRamBytes() / 4,
                             qint64(2) * 1024 * 1024 * 1024);
  if (workingSetBytes > budgetBytes)
  {
    failNow(tr("内存超限：滑窗工作集约 %1 MB 超预算 %2 MB（剖面 %3 道 × %4 采样）")
                .arg(workingSetBytes / (1024 * 1024))
                .arg(budgetBytes / (1024 * 1024))
                .arg(volume->XlineCount())
                .arg(nS));
    return nullptr;
  }

  // 同体顶替：旧在途传播取消 + 静默标志（回调丢弃，cancelled≠failed 口径
  // 同切片顶替 A3）
  const QString pathKey = QString::fromStdString(volume->Path().string());
  if (PaleoTask *old = inFlightPropagation_.value(pathKey))
  {
    const auto oldFlag = propagationSuperseded_.value(pathKey);
    if (oldFlag)
      oldFlag->store(true);
    old->requestCancel();
  }
  const auto superseded = std::make_shared<std::atomic_bool>(false);
  propagationSuperseded_.insert(pathKey, superseded);

  const QString title = tr("层位体传播（IL%1，%2 种子）")
                            .arg(request.seedInline)
                            .arg(request.seeds.size());
  const paleo::hztrack::TrackOptions kernelOptions{
      request.windowSamples, request.maxSearchSamples,
      request.correlationThreshold, 0.0, request.dipHistoryPicks};
  const int maxInlineStep = request.maxInlineStep;
  const double dtMs = double(volume->SampleIntervalUs()) / 1000.0;
  const double startTimeMs = request.startTimeMs;
  const QString interpreter = request.interpreter;
  const QString horizonName = request.horizonName;
  auto picksOut = std::make_shared<QList<SeismicPick>>();
  auto reportOut = std::make_shared<SeismicTrackReport>();

  auto work = [volume, seedIlIdx, kernelSeeds, kernelOptions, maxInlineStep,
               dtMs, startTimeMs, interpreter, horizonName, picksOut,
               reportOut](PaleoTask *task) -> QString {
    const std::vector<int> &ilAxis = volume->InlineValues();
    const std::vector<int> &xlAxis = volume->XlineValues();
    const int nIl = int(ilAxis.size());
    const int nXl = int(xlAxis.size());
    const int nS = volume->SampleCount();

    // IL 滑窗供应器：至多 2 枚剖面驻留（道主序副本；行主序像即取即弃），
    // 按种子向两侧单调取数——体不全体驻留（Oracle#5 内存边界）
    struct SlidingSections
    {
      std::shared_ptr<const SgyVolume> vol;
      const std::vector<int> *ilAxis;
      PaleoTask *task;
      qint64 fetches = 0;
      qint64 totalFetchBudget = 0;
      int lastFailedIl = -1;
      QString lastError;
      struct Slot
      {
        int il = -1;
        std::vector<float> traceMajor;
      } a, b; // a = 最近取用
      const float *section(int il)
      {
        if (a.il == il)
          return a.traceMajor.data();
        if (b.il == il)
          return b.traceMajor.data();
        SgySliceImage img;
        std::string err;
        const auto progress = [this](int, int) {
          return !(task && task->cancelRequested());
        };
        if (!vol->ExtractSlice(SgySliceType::Inline,
                               (*ilAxis)[static_cast<std::size_t>(il)], img,
                               err, progress))
        {
          lastFailedIl = il;
          lastError = QString::fromStdString(err);
          return nullptr;
        }
        b = std::move(a);
        a.il = il;
        a.traceMajor = traceMajorSection(img);
        ++fetches;
        if (task && totalFetchBudget > 0)
          task->reportBytes(std::min(fetches, totalFetchBudget),
                            totalFetchBudget);
        return a.traceMajor.data();
      }
    } provider{volume, &ilAxis, task};
    const int spanL = maxInlineStep > 0
                          ? std::min(maxInlineStep, seedIlIdx)
                          : seedIlIdx;
    const int spanR = maxInlineStep > 0
                          ? std::min(maxInlineStep, nIl - 1 - seedIlIdx)
                          : nIl - 1 - seedIlIdx;
    provider.totalFetchBudget = 1 + spanL + spanR;

    const paleo::hztrack::PropagateResult r = paleo::hztrack::propagateVolumeWindowed(
        nIl, nXl, nS,
        [&provider](int il) { return provider.section(il); },
        seedIlIdx, kernelSeeds, kernelOptions, maxInlineStep,
        [task]() { return task && task->cancelRequested(); });
    if (task && task->cancelRequested())
      return QString(); // 取消不发布半成品
    if (r.stopReason == paleo::hztrack::StopReason::ReadFailure)
    {
      const QString where = provider.lastFailedIl >= 0
          ? QObject::tr("IL %1").arg(ilAxis[static_cast<std::size_t>(
                                             provider.lastFailedIl)])
          : QObject::tr("未知剖面");
      return QObject::tr("剖面读取失败（%1）：%2")
          .arg(where,
               provider.lastError.isEmpty()
                   ? QObject::tr("读取错误")
                   : provider.lastError);
    }
    if (r.stopReason == paleo::hztrack::StopReason::Invalid)
      return QObject::tr("传播输入无效（种子/几何）");

    SeismicTrackReport report;
    report.coveredTraces = int(r.picks.size());
    report.totalTraces = nIl * nXl;
    double confSum = 0.0;
    picksOut->reserve(int(r.picks.size()));
    for (const paleo::hztrack::VolumePick &p : r.picks)
    {
      SeismicPick pick;
      pick.inlineNo = ilAxis[static_cast<std::size_t>(p.il)];
      pick.xlineNo = xlAxis[static_cast<std::size_t>(p.xl)];
      pick.sampleIndex = p.sample;
      pick.twtMs = startTimeMs + double(p.sample) * dtMs;
      pick.confidence = p.confidence;
      pick.interpreter = interpreter;
      pick.horizonName = horizonName;
      picksOut->append(pick);
      confSum += p.confidence;
    }
    report.meanConfidence =
        r.picks.empty() ? 0.0f : float(confSum / double(r.picks.size()));
    report.ilMin = r.picks.empty() ? 0 : ilAxis[static_cast<std::size_t>(r.ilMin)];
    report.ilMax = r.picks.empty() ? 0 : ilAxis[static_cast<std::size_t>(r.ilMax)];
    switch (r.stopReason)
    {
    case paleo::hztrack::StopReason::Completed:
      report.stopSummary = r.picks.empty()
          ? QObject::tr("前沿空（种子剖面追踪失败）")
          : QObject::tr("到达体边界（IL %1..%2）").arg(report.ilMin).arg(report.ilMax);
      break;
    case paleo::hztrack::StopReason::CorrelationLoss:
      report.stopSummary = r.picks.empty()
          ? QObject::tr("种子剖面即失相关（无拾取）")
          : QObject::tr("前沿相关丢失（IL 覆盖 %1..%2，部分覆盖如实保留）")
                .arg(report.ilMin)
                .arg(report.ilMax);
      break;
    default:
      report.stopSummary = trackStopText(r.stopReason);
      break;
    }
    *reportOut = report;
    if (task && provider.totalFetchBudget > 0)
      task->reportBytes(provider.totalFetchBudget, provider.totalFetchBudget);
    return QString();
  };

  // 长任务：任务中心可见（进度/取消走既有任务面板）
  PaleoTask *task = startBounded(title, work, QString(), /*quiet=*/false);
  inFlightPropagation_.insert(pathKey, task);
  connect(task, &PaleoTask::finished, this,
          [this, task, superseded, picksOut, reportOut, onFinished, pathKey]() {
            if (inFlightPropagation_.value(pathKey) == task)
              inFlightPropagation_.remove(pathKey);
            if (propagationSuperseded_.value(pathKey) == superseded)
              propagationSuperseded_.remove(pathKey);
            if (superseded->load())
              return; // 被顶替：静默丢弃（不报取消不报错）
            if (!onFinished)
              return;
            if (task->state() == PaleoTask::State::Succeeded)
              onFinished(true, *picksOut, *reportOut, QString());
            else if (task->state() == PaleoTask::State::Cancelled)
              onFinished(false, {}, SeismicTrackReport{}, tr("体传播已取消"));
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

namespace {

// goal/horizon-3d — 层位资产文件体（CSV + 可选层位栅格 GeoTIFF +
// LayerDeclaration 回填）：registerHorizonAsset（IDW 网格化拾取）与
// registerPropagatedHorizonAsset（直接成格前沿）共用——同一文件格式与
// 上图管线，不开平行格式。返回 CSV 路径（空 = 失败，error 已填）。
QString writeHorizonAssetFiles(const SeismicHorizonGrid &grid,
                               const QString &horizonName,
                               const QString &outputDir, QString *error,
                               LayerDeclaration *layerOut)
{
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
  return filePath;
}

// goal/horizon-3d — DERIVED 版本登记体（parentVersionIds 锚源体版本）：
// 两类层位资产共用。返回登记后的版本路径（空 = 失败）。
QString registerHorizonVersionEntry(
    DataCatalog *catalog, const QString &seismicAssetId,
    const QString &seismicVersionId, const QString &horizonName,
    const QString &filePath, int pickCount, const QString &origin,
    QString *error)
{
  const QString fileName = QFileInfo(filePath).fileName();
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
  v.extra.insert(QStringLiteral("origin"), origin);
  v.extra.insert(QStringLiteral("pickCount"), pickCount);
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

} // namespace

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
  const QString filePath =
      writeHorizonAssetFiles(grid, horizonName, outputDir, error, layerOut);
  if (filePath.isEmpty())
    return QString();
  return registerHorizonVersionEntry(catalog, seismicAssetId,
                                     seismicVersionId, horizonName, filePath,
                                     picks.size(),
                                     QStringLiteral("seismic-interpretation"),
                                     error);
}

SeismicHorizonGrid SeismicTaskService::gridPropagated(const QList<SeismicPick> &picks)
{
  SeismicHorizonGrid grid;
  if (picks.isEmpty())
    return grid;
  // 轴推导与 gridPicks 同式（同轴最小间隔为步）
  int ilMin = picks.first().inlineNo, ilMax = ilMin;
  int xlMin = picks.first().xlineNo, xlMax = xlMin;
  for (const SeismicPick &p : picks)
  {
    ilMin = std::min(ilMin, p.inlineNo); ilMax = std::max(ilMax, p.inlineNo);
    xlMin = std::min(xlMin, p.xlineNo); xlMax = std::max(xlMax, p.xlineNo);
  }
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

  // 直接落格：拾取线号恰在格点上（规则测网恒成立；不规则网落不进格的
  // 拾取如实跳过，不臆造最近格归属）。同格点取最高置信。
  for (const SeismicPick &p : picks)
  {
    const int di = p.inlineNo - grid.inlineMin;
    const int dx = p.xlineNo - grid.xlineMin;
    if (di % grid.inlineStep != 0 || dx % grid.xlineStep != 0)
      continue;
    const int gi = di / grid.inlineStep;
    const int gx = dx / grid.xlineStep;
    if (gi < 0 || gi >= grid.inlineCount || gx < 0 || gx >= grid.xlineCount)
      continue;
    const std::size_t at = std::size_t(gi) * grid.xlineCount + gx;
    if (!std::isfinite(grid.twtMs[at]) || p.confidence > grid.confidence[at])
    {
      grid.twtMs[at] = p.twtMs;
      grid.confidence[at] = p.confidence;
    }
  }
  return grid;
}

QString SeismicTaskService::registerPropagatedHorizonAsset(
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
  const SeismicHorizonGrid grid = gridPropagated(picks);
  if (!grid.isValid())
  {
    if (error)
      *error = QStringLiteral("体传播拾取成格失败");
    return QString();
  }
  const QString filePath =
      writeHorizonAssetFiles(grid, horizonName, outputDir, error, layerOut);
  if (filePath.isEmpty())
    return QString();
  return registerHorizonVersionEntry(catalog, seismicAssetId,
                                     seismicVersionId, horizonName, filePath,
                                     picks.size(),
                                     QStringLiteral("seismic-propagation"),
                                     error);
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

} // namespace seismic
