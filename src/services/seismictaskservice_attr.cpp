// 层：数据
#include "services/seismictaskservice.h"
#include "services/seismictaskservice_internal.h"
#include "services/fspathutils.h"
#include "services/paleotaskservice.h"

#include "algorithms/seismicattr.h"
#include "domain/seismic/sgycoordinatemapper.h"
#include "io/attrgridout.h"
#include "io/sattrio.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <thread>
#include <vector>

namespace seismic {

namespace
{

// 源 SEG-Y 首样延迟（t0，ms）：首道道头字节 109-110（big-endian i16）。
// 单次 240B 预读——比开整个 SegyReader 索引轻三个量级。
double readSgyStartTimeMs(const QString &sgyPath)
{
  QFile f(sgyPath);
  if (!f.open(QIODevice::ReadOnly))
    return 0.0;
  if (!f.seek(3600 + 108))
    return 0.0;
  uchar b[2] = {0, 0};
  if (f.read(reinterpret_cast<char *>(b), 2) != 2)
    return 0.0;
  return double(qFromBigEndian<qint16>(b));
}

// 轴均值步长（稀疏轴 georeference 近似；正步长由升序轴保证）。
double attrAxisMeanStep(const std::vector<int> &axis)
{
  if (axis.size() < 2)
    return 0.0;
  return double(axis.back() - axis.front()) / double(axis.size() - 1);
}

} // namespace

// 去重缓存产物名：<attrId>_<scope>_<hash12>.<ext>（参数包摘要前缀命名；
// 命中仍复验 sidecar/容器头内全量 hash——前缀只做寻址不做身份）。
QString attrCacheFilePath(const QString &outputDir, const QString &attrId,
                          const QString &scope, const QString &paramHash,
                          const QString &ext)
{
  return outputDir + QLatin1Char('/') + attrId + QLatin1Char('_') + scope +
         QLatin1Char('_') + paramHash.left(12) + QLatin1Char('.') + ext;
}

namespace
{

// GeoTIFF 缓存的身份 sidecar：<file>.prov.json——参数包摘要（栅格元数据
// PALEO_PARAM_HASH 双保险）。
bool writeProvSidecar(const QString &provPath, const QString &paramHash,
                      const QString &attrId, const QString &scope)
{
  QJsonObject o;
  o.insert(QStringLiteral("paramHash"), paramHash);
  o.insert(QStringLiteral("attrId"), attrId);
  o.insert(QStringLiteral("scope"), scope);
  const QString partial = provPath + QStringLiteral(".partial");
  QFile f(partial);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return false;
  f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
  f.close();
  if (QFileInfo::exists(provPath) && !QFile::remove(provPath))
    return false;
  return QFile::rename(partial, provPath);
}

QString readProvSidecarHash(const QString &provPath)
{
  QFile f(provPath);
  if (!f.open(QIODevice::ReadOnly))
    return QString();
  const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
  return o.value(QStringLiteral("paramHash")).toString();
}

// 时间切片逐道属性计算（瞬时族整道谱 / 时窗族垂向窗）——与剖面路径同一
// 核心序列（列 → 时间序 → 核 → 取样位），仅输出采样位而非整列。
float attrSampleOfTrace(seismic::SeismicTaskService::SeismicAttrKind kind,
                        const float *trace, int n,
                        const seismic::SeismicTaskService::SeismicAttrParams &params,
                        double dtMs, int sampleIndex)
{
  using K = seismic::SeismicTaskService::SeismicAttrKind;
  std::vector<float> out;
  switch (kind)
  {
  case K::Envelope:
  case K::InstPhase:
  case K::InstFreq:
  case K::InstQ:
  case K::Sweetness:
  {
    const paleo::seisattr::ComplexTraceResult ct =
        paleo::seisattr::complexTraceAnalysis(trace, n, dtMs);
    switch (kind)
    {
    case K::Envelope: return ct.envelope[std::size_t(sampleIndex)];
    case K::InstPhase: return ct.phaseDeg[std::size_t(sampleIndex)];
    case K::InstFreq: return ct.freqHz[std::size_t(sampleIndex)];
    case K::InstQ: return ct.quality[std::size_t(sampleIndex)];
    default: break;
    }
    out.assign(std::size_t(n), std::numeric_limits<float>::quiet_NaN());
    paleo::seisattr::sweetness(ct.envelope.data(), ct.freqHz.data(), n,
                               out.data());
    return out[std::size_t(sampleIndex)];
  }
  case K::Rms:
    out.assign(std::size_t(n), std::numeric_limits<float>::quiet_NaN());
    paleo::seisattr::windowedRms(trace, n, params.windowHalfSamples, out.data());
    return out[std::size_t(sampleIndex)];
  case K::MaxAbs:
    out.assign(std::size_t(n), std::numeric_limits<float>::quiet_NaN());
    paleo::seisattr::windowedMaxAbs(trace, n, params.windowHalfSamples,
                                    out.data());
    return out[std::size_t(sampleIndex)];
  case K::MeanEnergy:
    out.assign(std::size_t(n), std::numeric_limits<float>::quiet_NaN());
    paleo::seisattr::windowedMeanEnergy(trace, n, params.windowHalfSamples,
                                        out.data());
    return out[std::size_t(sampleIndex)];
  case K::Coherence:
    break; // 相干走采样 slab 三维窗，不在逐道路径
  }
  return std::numeric_limits<float>::quiet_NaN();
}

// 整道属性计算（体扫描逐道全样）：结果写满 attr（长度 n，时间序）。
void attrTraceOfTrace(seismic::SeismicTaskService::SeismicAttrKind kind,
                      const float *trace, int n,
                      const seismic::SeismicTaskService::SeismicAttrParams &params,
                      double dtMs, float *attr)
{
  using K = seismic::SeismicTaskService::SeismicAttrKind;
  switch (kind)
  {
  case K::Envelope:
  case K::InstPhase:
  case K::InstFreq:
  case K::InstQ:
  case K::Sweetness:
  {
    const paleo::seisattr::ComplexTraceResult ct =
        paleo::seisattr::complexTraceAnalysis(trace, n, dtMs);
    switch (kind)
    {
    case K::Envelope: std::copy(ct.envelope.begin(), ct.envelope.end(), attr); break;
    case K::InstPhase: std::copy(ct.phaseDeg.begin(), ct.phaseDeg.end(), attr); break;
    case K::InstFreq: std::copy(ct.freqHz.begin(), ct.freqHz.end(), attr); break;
    case K::InstQ: std::copy(ct.quality.begin(), ct.quality.end(), attr); break;
    default:
      paleo::seisattr::sweetness(ct.envelope.data(), ct.freqHz.data(), n, attr);
      break;
    }
    return;
  }
  case K::Rms:
    paleo::seisattr::windowedRms(trace, n, params.windowHalfSamples, attr);
    return;
  case K::MaxAbs:
    paleo::seisattr::windowedMaxAbs(trace, n, params.windowHalfSamples, attr);
    return;
  case K::MeanEnergy:
    paleo::seisattr::windowedMeanEnergy(trace, n, params.windowHalfSamples, attr);
    return;
  case K::Coherence:
    break; // 相干按滑窗分块处理，不走逐道路径
  }
  std::fill(attr, attr + n, std::numeric_limits<float>::quiet_NaN());
}

// NaN 感知统计累积。
struct AttrScanStats
{
  float vMin = std::numeric_limits<float>::max();
  float vMax = std::numeric_limits<float>::lowest();
  qint64 valid = 0;
  void add(float v)
  {
    if (!std::isfinite(v))
      return;
    ++valid;
    if (v < vMin)
      vMin = v;
    if (v > vMax)
      vMax = v;
  }
  bool any() const { return valid > 0; }
};

} // namespace

QString SeismicTaskService::attrScanParamHash(const QString &scope,
                                              SeismicAttrKind kind,
                                              const SeismicAttrParams &params,
                                              const QString &sgyPath, int sampleIndex)
{
  QCryptographicHash h(QCryptographicHash::Sha256);
  h.addData("paleo-attrscan-v1\n");
  h.addData(scope.toUtf8());
  h.addData("\n");
  h.addData(seismicAttrId(kind).toUtf8());
  h.addData("\n");
  std::string p;
  p += std::to_string(params.windowHalfSamples) + ",";
  p += std::to_string(params.coherenceIlHalf) + ",";
  p += std::to_string(params.coherenceXlHalf) + ",";
  p += std::to_string(params.coherenceTimeHalf) + ",";
  p += std::to_string(params.coherenceWeighting) + "\n";
  h.addData(QByteArray(p.data(), qsizetype(p.size())));
  if (scope == QLatin1String("ts"))
    h.addData(QByteArray::number(sampleIndex) + "\n");
  const QFileInfo fi(sgyPath);
  h.addData(fi.absoluteFilePath().toUtf8());
  h.addData("\n");
  h.addData(QByteArray::number(fi.size()) + "," +
            QByteArray::number(fi.lastModified().toSecsSinceEpoch()) + "\n");
  return QString::fromLatin1(h.result().toHex());
}

PaleoTask *SeismicTaskService::startTimeSliceAttribute(
    std::shared_ptr<const SgyVolume> volume, SeismicAttrKind kind,
    const SeismicAttrParams &params, int sampleIndex, const QString &outputDir,
    std::function<void(bool success, const SeismicAttrTimeSliceResult &result)> onFinished)
{
  if (!taskService_)
  {
    if (onFinished)
    {
      SeismicAttrTimeSliceResult r;
      r.error = QStringLiteral("PaleoTaskService not set");
      onFinished(false, r);
    }
    return nullptr;
  }
  if (!volume || !volume->IsLoaded())
  {
    if (onFinished)
    {
      SeismicAttrTimeSliceResult r;
      r.error = QStringLiteral("地震体未加载（先完成体加载）");
      onFinished(false, r);
    }
    return nullptr;
  }
  if (sampleIndex < 0 || sampleIndex >= volume->SampleCount())
  {
    if (onFinished)
    {
      SeismicAttrTimeSliceResult r;
      r.error = QStringLiteral("采样号 %1 越界（0..%2）")
                    .arg(sampleIndex)
                    .arg(volume->SampleCount() - 1);
      onFinished(false, r);
    }
    return nullptr;
  }
  if (kind == SeismicAttrKind::Coherence &&
      (volume->InlineCount() <= 2 * params.coherenceIlHalf ||
       volume->XlineCount() <= 2 * params.coherenceXlHalf))
  {
    if (onFinished)
    {
      SeismicAttrTimeSliceResult r;
      r.error = QStringLiteral(
                    "测网过小（%1×%2 道），相干窗 ±%3×±%4 道后无完整窗——"
                    "拒绝产出全 NaN 图")
                    .arg(volume->InlineCount())
                    .arg(volume->XlineCount())
                    .arg(params.coherenceIlHalf)
                    .arg(params.coherenceXlHalf);
      onFinished(false, r);
    }
    return nullptr;
  }

  auto result = std::make_shared<SeismicAttrTimeSliceResult>();
  result->attrId = seismicAttrId(kind);
  result->sampleIndex = sampleIndex;

  const QString title = tr("地震属性时间切片 %1（样 %2）")
                            .arg(seismicAttrDisplayName(kind))
                            .arg(sampleIndex);
  const QString volumePath = paleo::fromFsPath(volume->Path());
  const double dtMs = double(volume->SampleIntervalUs()) / 1000.0;
  const double t0Ms = readSgyStartTimeMs(volumePath);

  auto work = [volume, kind, params, sampleIndex, outputDir, result, volumePath,
               dtMs, t0Ms](PaleoTask *task) -> QString {
    if (task->cancelRequested())
      return QString();
    result->timeMs = t0Ms + double(sampleIndex) * dtMs;

    const QString paramHash =
        attrScanParamHash(QStringLiteral("ts"), kind, params, volumePath,
                          sampleIndex);
    result->paramHash = paramHash;

    // 同参数去重缓存：sidecar 全量 hash 复验 + 产物存在 + PALEO_* 身份
    // 元数据/地理参考可读回 → 命中不重算（统计从产物摘要回填——面板
    // 「有效 N/M」不撒谎）；摘要不全视同未命中重算。
    QString cachePath;
    if (!outputDir.isEmpty())
    {
      cachePath = attrCacheFilePath(outputDir, result->attrId,
                                    QStringLiteral("ts"), paramHash,
                                    QStringLiteral("tif"));
      if (QFileInfo::exists(cachePath) &&
          readProvSidecarHash(cachePath + QStringLiteral(".prov.json")) ==
              paramHash)
      {
        paleo::sattr::AttrTimeSliceGrid summary;
        QString summaryErr;
        if (paleo::sattr::readTimeSliceGeoTiffSummary(cachePath, &summary,
                                                      &summaryErr))
        {
          result->ok = true;
          result->cacheHit = true;
          result->cachePath = cachePath;
          result->nIl = summary.nIl;
          result->nXl = summary.nXl;
          result->valueMin = summary.valueMin;
          result->valueMax = summary.valueMax;
          result->validCells = summary.validCells;
          result->sampleIndex = summary.sampleIndex;
          task->reportBytes(1, 1);
          return QString();
        }
      }
    }

    // 地理参考前置校验（诚实面）：无坐标/拟合失败 → 如实失败，不产
    // 非地理参考的「栅格」去冒充层树条目。
    auto mapper = seismic::SgyCoordinateMapper::Fit(*volume->Index());
    if (!mapper.valid())
    {
      result->error = QStringLiteral("源测网无法地理参考（%1）——时间切片"
                                     "属性需地理参考栅格产物，拒绝产出")
                          .arg(QString::fromStdString(
                              mapper.fit().rejectionReason));
      return result->error;
    }

    const std::vector<int> ilVals = volume->InlineValues();
    const std::vector<int> xlVals = volume->XlineValues();
    const int nIl = volume->InlineCount();
    const int nXl = volume->XlineCount();
    const int nS = volume->SampleCount();
    result->ilValues = ilVals;
    result->xlValues = xlVals;
    result->nIl = nIl;
    result->nXl = nXl;
    result->values.assign(std::size_t(nIl) * nXl,
                          std::numeric_limits<float>::quiet_NaN());

    QElapsedTimer clock;
    clock.start();
    // 进度单位 = 列（读 nIl·nXl + 算 nIl·nXl），单调；相干算阶段按整段补足。
    const qint64 totalUnits = 2 * qint64(nIl) * nXl;
    qint64 done = 0;

    if (kind == SeismicAttrKind::Coherence)
    {
      // 采样 slab：[s-tH, s+tH] ∩ [0, nS)——恰为目标采样的完整垂直窗
      // （边缘缩窗与整体系数一致）。IL×XL 全覆盖 → slab 相干中心面。
      const int s0 = std::max(0, sampleIndex - params.coherenceTimeHalf);
      const int s1 = std::min(nS - 1, sampleIndex + params.coherenceTimeHalf);
      const int slabN = s1 - s0 + 1;
      if (qint64(nIl) * nXl * slabN > std::numeric_limits<int>::max())
      {
        result->error = QStringLiteral("相干 slab 过大（%1×%2×%3 采样）——"
                                       "拒绝分配越界缓冲")
                            .arg(nIl)
                            .arg(nXl)
                            .arg(slabN);
        return result->error;
      }
      std::vector<float> slab(std::size_t(nIl) * nXl * slabN,
                              std::numeric_limits<float>::quiet_NaN());
      for (int il = 0; il < nIl; ++il)
      {
        if (task->cancelRequested())
          return QString();
        auto sliceProgress = [&task, &done, totalUnits](int processed, int total) {
          if (total > 0)
            task->reportBytes(done + processed, totalUnits);
          return !task->cancelRequested();
        };
        SgySliceImage slice;
        std::string extractErr;
        if (!volume->ExtractSlice(SgySliceType::Inline, ilVals[std::size_t(il)],
                                  slice, extractErr, sliceProgress))
        {
          if (task->cancelRequested())
            return QString();
          result->error = QString::fromStdString(extractErr);
          return QStringLiteral("切片读取失败：%1").arg(result->error);
        }
        // 切片 [row][x]（row 0 = 最深）→ slab 道连续 [xl*slabN + w]。
        for (int xl = 0; xl < nXl; ++xl)
          for (int w = s0; w <= s1; ++w)
            slab[std::size_t((il * nXl + xl) * slabN + (w - s0))] =
                slice.values[std::size_t((nS - 1 - w) * nXl + xl)];
        done += nXl;
        task->reportStage(QStringLiteral("read"), int(done * 100 / totalUnits));
        task->reportBytes(done, totalUnits);
      }

      task->reportStage(QStringLiteral("compute"), 50);
      std::vector<float> coh(slab.size(),
                             std::numeric_limits<float>::quiet_NaN());
      const double ilSp = std::hypot(mapper.fit().a, mapper.fit().d) *
                          attrAxisMeanStep(ilVals);
      const double xlSp = std::hypot(mapper.fit().b, mapper.fit().e) *
                          attrAxisMeanStep(xlVals);
      paleo::seisattr::semblanceCoherenceWeighted(
          slab.data(), nIl, nXl, slabN, params.coherenceIlHalf,
          params.coherenceXlHalf, params.coherenceTimeHalf, ilSp, xlSp,
          params.coherenceWeighting == 1
              ? paleo::seisattr::CoherenceWeightMode::InverseDistance
              : paleo::seisattr::CoherenceWeightMode::Equal,
          coh.data());
      AttrScanStats stats;
      for (int il = 0; il < nIl; ++il)
        for (int xl = 0; xl < nXl; ++xl)
        {
          const float v = coh[std::size_t((il * nXl + xl) * slabN +
                                          (sampleIndex - s0))];
          result->values[std::size_t(il * nXl + xl)] = v;
          stats.add(v);
        }
      task->reportBytes(totalUnits, totalUnits); // 算阶段整段补足
      if (stats.any())
      {
        result->valueMin = stats.vMin;
        result->valueMax = stats.vMax;
      }
      result->validCells = stats.valid;
    }
    else
    {
      // 逐道族：逐 inline 读剖面 → 逐道整道谱/垂向窗 → 取采样位。
      // 列间独立——std::thread 分片并行（剖面路径同先例，≤min(4,hw) 片）。
      const unsigned int hw = std::thread::hardware_concurrency();
      const int nThreads = std::max(1, std::min<int>(4, int(hw)));
      AttrScanStats stats; // 主线程聚合（分片只写各自列段）
      for (int il = 0; il < nIl; ++il)
      {
        if (task->cancelRequested())
          return QString();
        auto sliceProgress = [&task, &done, totalUnits](int processed, int total) {
          if (total > 0)
            task->reportBytes(done + processed, totalUnits);
          return !task->cancelRequested();
        };
        SgySliceImage slice;
        std::string extractErr;
        if (!volume->ExtractSlice(SgySliceType::Inline, ilVals[std::size_t(il)],
                                  slice, extractErr, sliceProgress))
        {
          if (task->cancelRequested())
            return QString();
          result->error = QString::fromStdString(extractErr);
          return QStringLiteral("切片读取失败：%1").arg(result->error);
        }
        if (slice.width != nXl || slice.height != nS)
        {
          result->error = QStringLiteral("切片几何异常（%1×%2，预期 %3×%4）")
                              .arg(slice.width)
                              .arg(slice.height)
                              .arg(nXl)
                              .arg(nS);
          return result->error;
        }
        const int chunk = (nXl + nThreads - 1) / nThreads;
        auto runRange = [&](int x0, int xEnd) {
          std::vector<float> traceBuf(static_cast<std::size_t>(nS));
          for (int x = x0; x < xEnd; ++x)
          {
            for (int row = 0; row < nS; ++row)
              traceBuf[std::size_t(nS - 1 - row)] =
                  slice.values[std::size_t(row * nXl + x)];
            result->values[std::size_t(il * nXl + x)] =
                attrSampleOfTrace(kind, traceBuf.data(), nS, params, dtMs,
                                  sampleIndex);
          }
        };
        std::vector<std::thread> workers;
        for (int t = 1; t < nThreads; ++t)
        {
          const int x0 = std::min(nXl, t * chunk);
          const int x1 = std::min(nXl, x0 + chunk);
          if (x0 >= x1)
            break;
          workers.emplace_back(runRange, x0, x1);
        }
        runRange(0, std::min(nXl, chunk));
        for (auto &w : workers)
          w.join();
        if (task->cancelRequested())
          return QString();
        for (int x = 0; x < nXl; ++x)
          stats.add(result->values[std::size_t(il * nXl + x)]);
        done += 2 * qint64(nXl); // 读 + 算各一列段
        task->reportStage(QStringLiteral("scan"), int(done * 100 / totalUnits));
        task->reportBytes(done, totalUnits);
      }
      if (stats.any())
      {
        result->valueMin = stats.vMin;
        result->valueMax = stats.vMax;
      }
      result->validCells = stats.valid;
    }
    result->readMs = double(clock.elapsed()); // 读算一体计时（分块交错）

    // 地理参考栅格落盘（缓存命名 + 原子改名；取消/失败不留半成品）。
    if (!outputDir.isEmpty())
    {
      paleo::sattr::AttrTimeSliceGrid grid;
      grid.attrId = result->attrId;
      grid.nIl = result->nIl;
      grid.nXl = result->nXl;
      grid.ilValues = result->ilValues;
      grid.xlValues = result->xlValues;
      grid.values = result->values;
      grid.valueMin = result->valueMin;
      grid.valueMax = result->valueMax;
      grid.validCells = result->validCells;
      grid.sampleIndex = sampleIndex;
      grid.timeMs = result->timeMs;
      grid.sampleIntervalMs = dtMs;
      grid.startTimeMs = t0Ms;
      grid.sourceSgyPath = volumePath;
      grid.paramHash = paramHash;
      const double affine[6] = {mapper.fit().a, mapper.fit().b, mapper.fit().c,
                                mapper.fit().d, mapper.fit().e, mapper.fit().f};
      QDir().mkpath(outputDir);
      const QString partial = cachePath + QStringLiteral(".partial");
      // 失败路径同样不留半成品（取消路径本就不经此处；写失败/改名失败/
      // sidecar 失败都要清 .partial——「取消/失败不留残件」纪律）。
      const auto failCleanup = [&partial](const QString &what) {
        QFile::remove(partial);
        return what;
      };
      QString writeErr;
      if (!paleo::sattr::writeTimeSliceGeoTiff(partial, grid, affine, &writeErr))
      {
        result->error = writeErr;
        return failCleanup(
            QStringLiteral("时间切片栅格写入失败：%1").arg(writeErr));
      }
      if (QFileInfo::exists(cachePath) && !QFile::remove(cachePath))
      {
        result->error = QStringLiteral("无法替换缓存产物 %1").arg(cachePath);
        return failCleanup(result->error);
      }
      if (!QFile::rename(partial, cachePath))
      {
        result->error = QStringLiteral("缓存产物落名失败：%1").arg(cachePath);
        return failCleanup(result->error);
      }
      if (!writeProvSidecar(cachePath + QStringLiteral(".prov.json"), paramHash,
                            result->attrId, QStringLiteral("ts")))
      {
        result->error = QStringLiteral("缓存 sidecar 写入失败：%1").arg(cachePath);
        return result->error;
      }
      result->cachePath = cachePath;
    }
    result->ok = true;
    return QString();
  };

  // 同体顶替：新扫描取消旧在途扫描，旧回调静默丢弃。
  if (!volumePath.isEmpty())
  {
    const auto it = inFlightAttrScans_.constFind(volumePath);
    if (it != inFlightAttrScans_.constEnd() && it.value())
    {
      it.value()->requestCancel();
      if (attrScanSuperseded_.contains(volumePath))
        attrScanSuperseded_[volumePath]->store(true);
    }
  }
  const auto superseded = std::make_shared<std::atomic_bool>(false);
  if (!volumePath.isEmpty())
  {
    attrScanSuperseded_[volumePath] = superseded;
  }
  PaleoTask *task = startBounded(title, work, QString(), /*quiet=*/false);
  if (!volumePath.isEmpty())
  {
    inFlightAttrScans_[volumePath] = task;
  }
  connect(task, &PaleoTask::finished, this,
          [this, task, volumePath, superseded, result, onFinished]() {
            if (inFlightAttrScans_.value(volumePath) == task)
            {
              inFlightAttrScans_.remove(volumePath);
              attrScanSuperseded_.remove(volumePath);
            }
            if (superseded->load())
              return; // 被顶替：静默丢弃（cancelled≠failed）
            if (!onFinished)
              return;
            if (task->state() == PaleoTask::State::Succeeded)
              onFinished(true, *result);
            else if (task->state() == PaleoTask::State::Cancelled)
            {
              SeismicAttrTimeSliceResult r;
              r.attrId = result->attrId;
              r.sampleIndex = result->sampleIndex;
              r.error = QStringLiteral("时间切片属性扫描已取消");
              onFinished(false, r);
            }
            else
              onFinished(false, *result);
          });
  return task;
}

PaleoTask *SeismicTaskService::startAttributeVolume(
    std::shared_ptr<const SgyVolume> volume, SeismicAttrKind kind,
    const SeismicAttrParams &params, const QString &outputDir,
    std::function<void(bool success, const SeismicAttrVolumeResult &result)> onFinished)
{
  if (!taskService_)
  {
    if (onFinished)
    {
      SeismicAttrVolumeResult r;
      r.error = QStringLiteral("PaleoTaskService not set");
      onFinished(false, r);
    }
    return nullptr;
  }
  if (!volume || !volume->IsLoaded())
  {
    if (onFinished)
    {
      SeismicAttrVolumeResult r;
      r.error = QStringLiteral("地震体未加载（先完成体加载）");
      onFinished(false, r);
    }
    return nullptr;
  }
  if (kind == SeismicAttrKind::Coherence &&
      (volume->InlineCount() <= 2 * params.coherenceIlHalf ||
       volume->XlineCount() <= 2 * params.coherenceXlHalf))
  {
    if (onFinished)
    {
      SeismicAttrVolumeResult r;
      r.error = QStringLiteral(
                    "测网过小（%1×%2 道），相干窗 ±%3×±%4 道后无完整窗——"
                    "拒绝产出全 NaN 体")
                    .arg(volume->InlineCount())
                    .arg(volume->XlineCount())
                    .arg(params.coherenceIlHalf)
                    .arg(params.coherenceXlHalf);
      onFinished(false, r);
    }
    return nullptr;
  }
  // H1：属性体扫描必写 SATV 产物——无产物目录即如实拒绝（空目录会落根
  // 路径/跨体互相覆盖）。时间切片可无目录（纯扫描不落盘），体不行。
  if (outputDir.isEmpty())
  {
    if (onFinished)
    {
      SeismicAttrVolumeResult r;
      r.error = QStringLiteral("属性体扫描需产物目录（应用层未注入解释目录）");
      onFinished(false, r);
    }
    return nullptr;
  }
  // H2：道距加权需要物理道距（源自测网坐标仿射）——拟合失败时如实拒绝，
  // 不静默产出全 NaN 体并落缓存（与时间切片入口的地理参考校验同口径）。
  if (kind == SeismicAttrKind::Coherence && params.coherenceWeighting == 1)
  {
    auto mapper = SgyCoordinateMapper::Fit(*volume->Index());
    if (!mapper.valid())
    {
      if (onFinished)
      {
        SeismicAttrVolumeResult r;
        r.error = QStringLiteral(
                      "相干道距加权需测网地理参考拟合（%1）——等权档不受限，"
                      "或修复道头坐标后重试")
                      .arg(QString::fromStdString(
                          mapper.fit().rejectionReason));
        onFinished(false, r);
      }
      return nullptr;
    }
  }

  auto result = std::make_shared<SeismicAttrVolumeResult>();
  result->attrId = seismicAttrId(kind);

  const QString title =
      tr("地震属性体扫描 %1").arg(seismicAttrDisplayName(kind));
  const QString volumePath = paleo::fromFsPath(volume->Path());
  const double dtMs = double(volume->SampleIntervalUs()) / 1000.0;
  const double t0Ms = readSgyStartTimeMs(volumePath);

  auto work = [volume, kind, params, outputDir, result, volumePath, dtMs,
               t0Ms](PaleoTask *task) -> QString {
    if (task->cancelRequested())
      return QString();
    const QString paramHash = attrScanParamHash(
        QStringLiteral("vol"), kind, params, volumePath, 0);
    result->paramHash = paramHash;

    // 去重缓存：SATV 头全量 hash 复验（容器身份自带，无需 sidecar）。
    QString cachePath;
    if (!outputDir.isEmpty())
    {
      cachePath = attrCacheFilePath(outputDir, result->attrId,
                                    QStringLiteral("vol"), paramHash,
                                    QStringLiteral("sattr"));
      paleo::sattr::SattrVolumeReader probe;
      if (QFileInfo::exists(cachePath) && probe.open(cachePath) &&
          probe.info().paramHash == paramHash)
      {
        const auto &info = probe.info();
        result->ok = true;
        result->cacheHit = true;
        result->path = cachePath;
        result->nIl = info.nIl;
        result->nXl = info.nXl;
        result->nS = info.nS;
        result->ilValues.assign(info.ilValues.begin(), info.ilValues.end());
        result->xlValues.assign(info.xlValues.begin(), info.xlValues.end());
        result->sampleIntervalMs = info.sampleIntervalMs;
        result->startTimeMs = info.startTimeMs;
        result->valueMin = info.valueMin;
        result->valueMax = info.valueMax;
        result->validCells = info.validCells;
        task->reportBytes(1, 1);
        return QString();
      }
    }

    const std::vector<int> ilVals = volume->InlineValues();
    const std::vector<int> xlVals = volume->XlineValues();
    const int nIl = volume->InlineCount();
    const int nXl = volume->XlineCount();
    const int nS = volume->SampleCount();

    auto mapper = seismic::SgyCoordinateMapper::Fit(*volume->Index());
    double ilSp = 0.0, xlSp = 0.0;
    if (mapper.valid())
    {
      ilSp = std::hypot(mapper.fit().a, mapper.fit().d) *
             attrAxisMeanStep(ilVals);
      xlSp = std::hypot(mapper.fit().b, mapper.fit().e) *
             attrAxisMeanStep(xlVals);
    }
    const auto weightMode =
        params.coherenceWeighting == 1
            ? paleo::seisattr::CoherenceWeightMode::InverseDistance
            : paleo::seisattr::CoherenceWeightMode::Equal;

    // 块尺寸：目标 ≤32MiB/块（滑窗+写块共用；相干 mini 体 = 块 + 双侧窗）。
    // 相干档块上限收到 8MiB——块内核计算一口算完，取消响应延迟以一块
    // 计算时长为上界（读/写路径的协作取消点不受影响）。
    const qint64 sliceFloats = qint64(nXl) * nS;
    const qint64 blockBudget =
        (kind == SeismicAttrKind::Coherence ? qint64(8) : qint64(32)) * 1024 *
        1024;
    const int blockIl = int(std::max<qint64>(
        1, std::min<qint64>(nIl, blockBudget / (sliceFloats * 4))));

    paleo::sattr::SattrVolumeInfo info;
    info.attrId = result->attrId;
    info.nIl = nIl;
    info.nXl = nXl;
    info.nS = nS;
    info.ilValues = QVector<int>(ilVals.begin(), ilVals.end());
    info.xlValues = QVector<int>(xlVals.begin(), xlVals.end());
    info.sampleIntervalMs = dtMs;
    info.startTimeMs = t0Ms;
    info.blockIl = blockIl;
    info.sourceSgyPath = volumePath;
    info.createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    info.paramHash = paramHash;
    info.windowHalfSamples = params.windowHalfSamples;
    info.coherenceIlHalf = params.coherenceIlHalf;
    info.coherenceXlHalf = params.coherenceXlHalf;
    info.coherenceTimeHalf = params.coherenceTimeHalf;
    info.coherenceWeighting = params.coherenceWeighting;
    info.ilTraceSpacing = ilSp;
    info.xlTraceSpacing = xlSp;

    if (!outputDir.isEmpty())
      QDir().mkpath(outputDir);
    const bool useCache = !outputDir.isEmpty();
    const QString finalPath = useCache ? cachePath : QString();
    const QString writePath =
        useCache ? cachePath + QStringLiteral(".partial")
                 : outputDir + QLatin1Char('/') + result->attrId +
                       QStringLiteral("_vol.sattr");
    paleo::sattr::SattrVolumeWriter writer;
    QString err;
    if (!writer.begin(writePath, info, &err))
    {
      result->error = err;
      QFile::remove(writePath); // begin 失败不留截断残件
      return err;
    }
    // 取消/失败路径：弃半成品（关文件 + 删 .partial——取消不留下可误命中
    // 的残件；复跑从头写）。
    const auto abandonPartial = [&writer, &writePath]() {
      writer.abort();
      QFile::remove(writePath);
    };

    QElapsedTimer clock;
    clock.start();
    AttrScanStats stats;
    const bool coherence = kind == SeismicAttrKind::Coherence;
    const int ilHalf = coherence ? params.coherenceIlHalf : 0;
    // 进度单位 = 列：读（相干含滑窗重叠重读）+ 算 + 写。
    const int nChunks = (nIl + blockIl - 1) / blockIl;
    const qint64 readLines =
        coherence ? qint64(nIl) + qint64(2) * ilHalf * nChunks : qint64(nIl);
    const qint64 totalUnits = (readLines + qint64(nIl)) * nXl;
    qint64 done = 0;

    // 逐块扫描：读窗 [c0-ilHalf, c1+ilHalf)（非相干窗 = 块本身）。
    std::vector<float> mini; // 相干 mini 体（块 + 双侧邻线）
    if (coherence)
      mini.resize(std::size_t(blockIl + 2 * ilHalf) * nXl * nS);
    std::vector<float> coh;
    if (coherence)
      coh.resize(mini.size());

    for (int c0 = 0; c0 < nIl; c0 += blockIl)
    {
      const int c1 = std::min(nIl, c0 + blockIl);
      if (coherence)
      {
        const int w0 = c0 - ilHalf;
        const int w1 = c1 + ilHalf;
        const int miniRows = w1 - w0;
        const std::size_t traceLen = std::size_t(nXl) * nS;
        for (int L = w0; L < w1; ++L)
        {
          if (task->cancelRequested())
          {
            abandonPartial();
            return QString();
          }
          float *dst = mini.data() + std::size_t(L - w0) * traceLen;
          if (L < 0 || L >= nIl)
          {
            std::fill(dst, dst + traceLen,
                      std::numeric_limits<float>::quiet_NaN());
            continue;
          }
          auto sliceProgress = [&task, &done, totalUnits](int processed, int total) {
            if (total > 0)
              task->reportBytes(done + processed, totalUnits);
            return !task->cancelRequested();
          };
          SgySliceImage slice;
          std::string extractErr;
          if (!volume->ExtractSlice(SgySliceType::Inline, ilVals[std::size_t(L)],
                                    slice, extractErr, sliceProgress))
          {
            if (task->cancelRequested())
            {
              abandonPartial();
              return QString();
            }
            result->error = QString::fromStdString(extractErr);
            abandonPartial();
            return QStringLiteral("切片读取失败：%1").arg(result->error);
          }
          // [row][x] → 道连续 [xl*nS+s]（转置教训同剖面路径）
          for (int xl = 0; xl < nXl; ++xl)
            for (int s = 0; s < nS; ++s)
              dst[std::size_t(xl) * nS + std::size_t(s)] =
                  slice.values[std::size_t((nS - 1 - s) * nXl + xl)];
          done += nXl;
          task->reportStage(QStringLiteral("read"), int(done * 100 / totalUnits));
          task->reportBytes(done, totalUnits);
        }
        paleo::seisattr::semblanceCoherenceWeighted(
            mini.data(), miniRows, nXl, nS, ilHalf, params.coherenceXlHalf,
            params.coherenceTimeHalf, ilSp, xlSp, weightMode, coh.data());
        for (int r = 0; r < c1 - c0; ++r)
        {
          const float *line =
              coh.data() + std::size_t(ilHalf + r) * std::size_t(nXl) * nS;
          for (std::size_t i = 0; i < std::size_t(nXl) * nS; ++i)
            stats.add(line[i]);
          if (!writer.writeInline(line, &err))
          {
            result->error = err;
            abandonPartial();
            return err;
          }
          done += nXl;
        }
        task->reportStage(QStringLiteral("compute"), int(done * 100 / totalUnits));
        task->reportBytes(done, totalUnits);
      }
      else
      {
        const unsigned int hw = std::thread::hardware_concurrency();
        const int nThreads = std::max(1, std::min<int>(4, int(hw)));
        const int chunk = (nXl + nThreads - 1) / nThreads;
        std::vector<float> outTrace(std::size_t(nXl) * nS,
                                    std::numeric_limits<float>::quiet_NaN());
        for (int L = c0; L < c1; ++L)
        {
          if (task->cancelRequested())
          {
            abandonPartial();
            return QString();
          }
          auto sliceProgress = [&task, &done, totalUnits](int processed, int total) {
            if (total > 0)
              task->reportBytes(done + processed, totalUnits);
            return !task->cancelRequested();
          };
          SgySliceImage slice;
          std::string extractErr;
          if (!volume->ExtractSlice(SgySliceType::Inline, ilVals[std::size_t(L)],
                                    slice, extractErr, sliceProgress))
          {
            if (task->cancelRequested())
            {
              abandonPartial();
              return QString();
            }
            result->error = QString::fromStdString(extractErr);
            abandonPartial();
            return QStringLiteral("切片读取失败：%1").arg(result->error);
          }
          // 列分片并行：列 → 时间序 → 整道属性 → 道连续写出位。
          auto runRange = [&](int x0, int xEnd) {
            std::vector<float> traceBuf(static_cast<std::size_t>(nS));
            for (int x = x0; x < xEnd; ++x)
            {
              for (int row = 0; row < nS; ++row)
                traceBuf[std::size_t(nS - 1 - row)] =
                    slice.values[std::size_t(row * nXl + x)];
              attrTraceOfTrace(kind, traceBuf.data(), nS, params, dtMs,
                               outTrace.data() + std::size_t(x) * nS);
            }
          };
          std::vector<std::thread> workers;
          for (int t = 1; t < nThreads; ++t)
          {
            const int x0 = std::min(nXl, t * chunk);
            const int x1 = std::min(nXl, x0 + chunk);
            if (x0 >= x1)
              break;
            workers.emplace_back(runRange, x0, x1);
          }
          runRange(0, std::min(nXl, chunk));
          for (auto &w : workers)
            w.join();
          if (task->cancelRequested())
          {
            abandonPartial();
            return QString();
          }
          for (int x = 0; x < nXl; ++x)
            for (int s = 0; s < nS; ++s)
              stats.add(outTrace[std::size_t(x) * nS + std::size_t(s)]);
          if (!writer.writeInline(outTrace.data(), &err))
          {
            result->error = err;
            abandonPartial();
            return err;
          }
          done += 2 * qint64(nXl); // 读 + 算写各一列段
          task->reportStage(QStringLiteral("scan"), int(done * 100 / totalUnits));
          task->reportBytes(done, totalUnits);
        }
      }
    }
    if (!writer.finish(stats.any() ? double(stats.vMin) : 0.0,
                       stats.any() ? double(stats.vMax) : 0.0, stats.valid, &err))
    {
      result->error = err;
      QFile::remove(writePath);
      return err;
    }
    if (useCache && !QFile::rename(writePath, finalPath))
    {
      result->error = QStringLiteral("缓存产物落名失败：%1").arg(finalPath);
      QFile::remove(writePath);
      return result->error;
    }

    result->ok = true;
    result->path = useCache ? finalPath : writePath;
    result->nIl = nIl;
    result->nXl = nXl;
    result->nS = nS;
    result->ilValues = ilVals;
    result->xlValues = xlVals;
    result->sampleIntervalMs = dtMs;
    result->startTimeMs = t0Ms;
    if (stats.any())
    {
      result->valueMin = stats.vMin;
      result->valueMax = stats.vMax;
    }
    result->validCells = stats.valid;
    result->readMs = double(clock.elapsed());
    return QString();
  };

  // 同体顶替（时间切片扫描同口径）。
  if (!volumePath.isEmpty())
  {
    const auto it = inFlightAttrScans_.constFind(volumePath);
    if (it != inFlightAttrScans_.constEnd() && it.value())
    {
      it.value()->requestCancel();
      if (attrScanSuperseded_.contains(volumePath))
        attrScanSuperseded_[volumePath]->store(true);
    }
  }
  const auto superseded = std::make_shared<std::atomic_bool>(false);
  if (!volumePath.isEmpty())
  {
    attrScanSuperseded_[volumePath] = superseded;
  }
  PaleoTask *task = startBounded(title, work, QString(), /*quiet=*/false);
  if (!volumePath.isEmpty())
  {
    inFlightAttrScans_[volumePath] = task;
  }
  connect(task, &PaleoTask::finished, this,
          [this, task, volumePath, superseded, result, onFinished]() {
            if (inFlightAttrScans_.value(volumePath) == task)
            {
              inFlightAttrScans_.remove(volumePath);
              attrScanSuperseded_.remove(volumePath);
            }
            if (superseded->load())
              return; // 被顶替：静默丢弃（cancelled≠failed）
            if (!onFinished)
              return;
            if (task->state() == PaleoTask::State::Succeeded)
              onFinished(true, *result);
            else if (task->state() == PaleoTask::State::Cancelled)
            {
              SeismicAttrVolumeResult r;
              r.attrId = result->attrId;
              r.error = QStringLiteral("属性体扫描已取消");
              onFinished(false, r);
            }
            else
              onFinished(false, *result);
          });
  return task;
}

} // namespace seismic
