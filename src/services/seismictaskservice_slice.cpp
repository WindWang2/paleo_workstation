// 层：数据
#include "workerexceptionsink.h"
#include "seismicdataprojection.h"
#include "../io/areamodel.h"
#include "services/seismictaskservice.h"
#include "services/seismictaskservice_internal.h"
#include "services/fspathutils.h"
#include "services/paleotaskservice.h"

#include "catalog/datacatalog.h"
#include "domain/seismic/sectionaxis.h"
#include "domain/seismic/sgycoordinatemapper.h"
#include "domain/seismic/sgyio.h"
#include "algorithms/seismicattr.h"
#include "io/sattrio.h"

#include <QDateTime>
#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <atomic>
#include <thread>
#include <set>

namespace seismic {

PaleoTask *SeismicTaskService::startSliceExtraction(
    std::shared_ptr<SgyVolume> volume,
    SgySliceType type,
    int sliceIndex,
    std::function<void(bool, std::shared_ptr<const SgySliceImage>, const QString &)> onFinished,
    const QString &pagedPath, bool viewer3D)
{
  if (!volume || !volume->IsIndexComplete())
  {
    if (onFinished)
      onFinished(false, nullptr, tr("地震体未完成索引，无法提取切片"));
    return nullptr;
  }

  std::error_code identityError;
  const auto sourceSize=std::filesystem::file_size(volume->Path(),identityError);
  const auto sourceStamp=std::filesystem::last_write_time(volume->Path(),identityError);
  if(identityError||sourceSize!=volume->Index()->fileSize||sourceStamp.time_since_epoch().count()!=volume->Index()->modifiedTimeTicks) {
    if(onFinished)onFinished(false,nullptr,tr("地震来源已变化，请重新加载后浏览"));
    return nullptr;
  }

  const QFileInfo current(paleo::fromFsPath(volume->Path()));
  const QString sourceToken=QStringLiteral("|%1:%2:%3").arg(current.size()).arg(current.lastModified().toMSecsSinceEpoch()).arg(current.metadataChangeTime().toMSecsSinceEpoch())+paleo::area_model::cacheToken(current.absoluteFilePath());
  const char *typeStr = (type == SgySliceType::Inline) ? "inl" : ((type == SgySliceType::Xline) ? "xl" : "time");
  // #291：缓存键的路径字节走 UTF-16 通道（path::string() 在 MSVC 上按 ACP
  // 返回窄字节、遇无法表示的字符还会抛——键会错乱甚至崩）。
  // #391：键并入 mtime——同路径同尺寸原地重写的体不再命中旧切片
  //（sectionCacheKey 同款口径）。
  const std::string key =
      SgyDataCache::MakeKey(typeStr, volume->Index()->fileSize,
                            static_cast<std::uint64_t>(sliceIndex)) +
      ":" + std::to_string(static_cast<long long>(volume->Index()->modifiedTimeTicks)) +
      ":" + paleo::fromFsPath(volume->Path()).toStdString() + (viewer3D?":3d:":":data:") + (volume->IsRuleBased()?"preview":"full")+sourceToken.toStdString();

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

  auto work = [registry, volume, sgyPath, pagedPath, type, sliceIndex, outImage, viewer3D](PaleoTask *task) -> QString {
    if(!viewer3D) {
      QString error;
      if(!paleo::readSeismicDataSlice(sgyPath,type,sliceIndex,*outImage,error,[task]{return task->cancelRequested();}))return error;
      return {};
    }
    if(volume->IsRuleBased()) {
      std::string error;int columns=0,total=0;
      const auto progress=[task](int done,int count){task->reportBytes(done,count);return !task->cancelRequested();};
      const bool ok=type==SgySliceType::Time
        ?volume->ExtractTimeSlicePreview(sliceIndex,48,64,*outImage,error,progress)
        :volume->ExtractPreviewSlice(type,sliceIndex,128,*outImage,error,columns,total,true,progress);
      return ok?QString():QString::fromUtf8(error);
    }
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

  // D5.2 任意线缓存：同体同路径同选项重复提取即出（缓存查询+命中回调走事件循环）
  SgySectionStats hitStats;
  if (std::shared_ptr<const SgySliceImage> hit = cachedSection(pathPoints, volume, options, &hitStats))
  {
    // B-2（full-audit-20261021）：删除「命中但 distances 为空 → 按线号总长
    // 均匀重采样」的合成分支——直接违反头注「绝不合成近似值」，且产物是
    // 「均匀 + 线号单位」双重近似。空 distances 如实交给消费端的兜底口径。
    if (onFinished)
    {
      auto *timer = new QTimer(this);
      timer->setSingleShot(true);
      connect(timer, &QTimer::timeout, this, [timer, hit, hitStats, onFinished]() {
        timer->deleteLater(); // RUNTIME-04：不能在自身 timeout 栈上同步 delete
        onFinished(true, hit, hitStats, QString());
      });
      timer->start(0);
    }
    return nullptr;
  }

  auto outImage = std::make_shared<SgySliceImage>();
  auto outStats = std::make_shared<SgySectionStats>();

  const QString sgyPath = QString::fromStdString(sgyio::ToUtf8Path(volume->Index()->path));
  auto work = [sgyPath, pathPoints, options, outImage, outStats](PaleoTask *task) -> QString {
    paleo::SeismicDataAccess source;QString error;if(!source.open(sgyPath,&error))return error;
    std::vector<std::pair<int,int>> path;for(const auto& p:pathPoints)path.push_back({p.x,p.y});
    paleo::data::SeismicSectionResult result;std::string detail;
    if(!source.section(path,options.maxColumns,options.interpolate,options.keepOutsideColumns,result,detail,[task]{return task->cancelRequested();}))return QString::fromUtf8(detail);
    *outImage=paleo::seismicSectionImage(result);outStats->columns=result.columns;
    outStats->firstTimeMs=result.firstTime;outStats->sampleIntervalMs=result.sampleInterval;
    outStats->coordinateSystem=source.coordinateSystem().toStdString();
    std::set<long long> unique;
    for(auto ordinal:result.traceIndexes)if(ordinal>=0)unique.insert(ordinal);else ++outStats->missingColumns;
    outStats->uniqueTraces=outStats->tracesRead=int(unique.size());outStats->duplicateColumns=result.columns-outStats->missingColumns-int(unique.size());
    long long previous=-2;for(auto ordinal:unique){if(ordinal!=previous+1)++outStats->readRanges;previous=ordinal;}
    for(double distance:result.distances)outStats->columnDistances.push_back(float(distance));
    // Grid arc length stays separate from physical distance; use confirmed units only.
    if((source.coordinateUnit()=="m"||source.coordinateUnit()=="ft")&&result.pathX.size()==size_t(result.columns)&&result.pathY.size()==size_t(result.columns)) {
      double distance=0;const double scale=source.coordinateUnit()=="ft"?0.3048:1.;
      for(int col=0;col<result.columns;++col) {
        if(!std::isfinite(result.pathX[col])||!std::isfinite(result.pathY[col])){outStats->columnDistancesM.clear();break;}
        if(col)distance+=std::hypot(result.pathX[col]-result.pathX[col-1],result.pathY[col]-result.pathY[col-1])*scale;
        outStats->columnDistancesM.push_back(float(distance));
      }
    }
    task->reportBytes(result.columns,result.columns);return {};

  };

  // quiet：剖面条拖动是交互内嵌取数，不拉起任务中心。
  PaleoTask *task = startBounded(title, work, QString(), /*quiet=*/true); // D6.4 ≤4 并发闸
  connect(task, &PaleoTask::finished, this,
          [this, task, outImage, outStats, onFinished, volume, pathPoints, options]() {
    if (task->state() == PaleoTask::State::Succeeded)
    {
      cacheSection(pathPoints, volume, outImage, outStats, options); // D5.2 成功入 LRU
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
    const QString &path,std::function<void(bool,std::shared_ptr<SgyVolume>,const QString&)> done,bool quiet) {
  return startIndexing(path,false,[done](bool ok,SgyIndexPtr index,const QString& error){
    std::shared_ptr<SgyVolume> volume;
    if(ok&&index){volume=std::make_shared<SgyVolume>();volume->AdoptIndex(index);}
    if(done)done(ok&&bool(volume),volume,error);
  },{},quiet);
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

  // 邻线解析（相干窗）：稀疏测网按轴值表相邻位取，不臆造 ±1 号。窗宽由
  // 剖面法向半窗决定（IL 剖面法向=inline → coherenceIlHalf；XL 剖面反之）
  // ——面板可调参数如实兑现（旧实现恒 ±1 线，SATR 登记参数与实算不符）。
  std::vector<int> lineValues;
  const bool needsNeighbors = seismicAttrNeedsNeighbors(kind);
  int normalHalf = 1;
  if (needsNeighbors)
  {
    normalHalf = isInline ? params.coherenceIlHalf : params.coherenceXlHalf;
    const std::vector<int> &axis =
        isInline ? volume->InlineValues() : volume->XlineValues();
    const auto it = std::find(axis.begin(), axis.end(), lineValue);
    const std::size_t pos = std::size_t(it - axis.begin());
    if (it == axis.end() ||
        static_cast<std::size_t>(normalHalf) >= axis.size() ||
        pos < static_cast<std::size_t>(normalHalf) ||
        pos + static_cast<std::size_t>(normalHalf) >= axis.size())
    {
      if (onFinished)
      {
        SeismicAttrResult r;
        r.error = QStringLiteral("%1 %2 处于测网边缘，无双侧 ±%3 邻线，相干不可用")
                      .arg(isInline ? QStringLiteral("inline") : QStringLiteral("crossline"))
                      .arg(lineValue)
                      .arg(normalHalf);
        onFinished(false, r);
      }
      return nullptr;
    }
    lineValues.reserve(static_cast<std::size_t>(2 * normalHalf + 1));
    for (int d = -normalHalf; d <= normalHalf; ++d)
      lineValues.push_back(axis[pos + static_cast<std::size_t>(d)]);
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

  // 相干沿向窗超出剖面宽时内核循环界为空——静默产出全 NaN 的「成功」。
  // 与时间切片/属性体路径同口径：如实拒绝（_attr.cpp 测网过小守卫）。
  if (kind == SeismicAttrKind::Coherence)
  {
    const int alongHalf = isInline ? params.coherenceXlHalf : params.coherenceIlHalf;
    if (2 * alongHalf > width - 1)
    {
      if (onFinished)
      {
        SeismicAttrResult r;
        r.error = QStringLiteral("剖面过窄（%1 道），沿向相干窗 ±%2 道后无完整窗——拒绝产出全 NaN 图")
                      .arg(width)
                      .arg(alongHalf);
        onFinished(false, r);
      }
      return nullptr;
    }
  }

  // 相干道距（加权档用；等权档内核不消费）：与属性体扫描同口径——
  // 仿射拟合的轴向量长度 × 轴值表平均步长（_attr.cpp 同式）。mini 体两轴
  // = （法向, 剖面列向）：IL 剖→(inline, xline)，XL 剖→(xline, inline)。
  double cohIlSpacing = 0.0, cohXlSpacing = 0.0;
  if (kind == SeismicAttrKind::Coherence)
  {
    const auto mapper = seismic::SgyCoordinateMapper::Fit(*volume->Index());
    const auto meanStep = [](const std::vector<int> &axis) -> double {
      return axis.size() < 2
                 ? 0.0
                 : double(axis.back() - axis.front()) / double(axis.size() - 1);
    };
    if (mapper.valid())
    {
      const double ilSp = std::hypot(mapper.fit().a, mapper.fit().d) *
                          meanStep(volume->InlineValues());
      const double xlSp = std::hypot(mapper.fit().b, mapper.fit().e) *
                          meanStep(volume->XlineValues());
      cohIlSpacing = isInline ? ilSp : xlSp;
      cohXlSpacing = isInline ? xlSp : ilSp;
    }
    else if (params.coherenceWeighting == 1)
    {
      // 加权需要道距而体坐标映射不可用：如实拒绝（内核对
      // InverseDistance+非正道距是全 NaN，不静默降级等权）。
      if (onFinished)
      {
        SeismicAttrResult r;
        r.error = QStringLiteral("道距加权需要体坐标映射（%1），本体不可用——请改等权或先修坐标")
                      .arg(QString::fromStdString(mapper.fit().rejectionReason));
        onFinished(false, r);
      }
      return nullptr;
    }
  }

  auto work = [volume, kind, params, sliceType, isInline, lineValues, result,
               dtMs, width, height, sliceCount, normalHalf, cohIlSpacing,
               cohXlSpacing](PaleoTask *task) -> QString {
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
      // #416：相干分支按预期 width×height 直接索引每条邻线的 values——
      // 任一邻线几何不符即越界读，不能只校验中线切片。
      if (slices[std::size_t(li)].width != width ||
          slices[std::size_t(li)].height != height)
      {
        result->error = QStringLiteral("切片几何异常（%1×%2，预期 %3×%4）")
                            .arg(slices[std::size_t(li)].width)
                            .arg(slices[std::size_t(li)].height)
                            .arg(width).arg(height);
        return result->error;
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
      // 组装法向窗小体：布局 [(lineIdx * width + x) * height + row]（row 空间
      // ——垂直窗对称，行序与时序等价）。邻线轴 = mini 体 IL 轴（半窗 =
      // 法向半窗 normalHalf，恰覆 2·normalHalf+1 线）；沿剖轴的半窗按剖向
      // 映射：IL 剖→XL 向，XL 剖→IL 向。法向/沿向半窗与加权档全部兑现
      // 面板参数（与时间切片/属性体路径同语义；SATR 登记值自此可信）。
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
      paleo::seisattr::semblanceCoherenceWeighted(
          vol3.data(), sliceCount, width, height,
          /*ilHalf=*/normalHalf, /*xlHalf=*/alongHalf, params.coherenceTimeHalf,
          /*ilSpacing=*/cohIlSpacing, /*xlSpacing=*/cohXlSpacing,
          params.coherenceWeighting == 1
              ? paleo::seisattr::CoherenceWeightMode::InverseDistance
              : paleo::seisattr::CoherenceWeightMode::Equal,
          coh.data());
      // 中线输出即目标线属性图（同 row-major 布局）。
      // 中线（il=normalHalf）出图：coh 逐道连续 [x][row] → 图像行主序 [row][x]，
      // 与组装侧对称的第二次转置（平铺拷贝会行列互换——轮2 教训二连）。
      for (int x = 0; x < width; ++x)
        for (int row = 0; row < height; ++row)
          out->values[static_cast<std::size_t>(row * width + x)] =
              coh[static_cast<std::size_t>((normalHalf * width + x) * height + row)];
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
      // B-4（full-audit-20261021）：算阶段进度改为全分片共享的原子列计数
      // ——此前只有 0 号分片入账，4 线程封顶 ~62% 后任务直接成功（账本与
      // 实际工作量脱节，ETA 按假速率外推）。
      std::atomic<int> columnsDone{0};
      auto runRange = [&](int x0, int xEnd) {
        for (int x = x0; x < xEnd && !cancelled(); ++x)
        {
          processColumn(x);
          const int n = columnsDone.fetch_add(1, std::memory_order_relaxed) + 1;
          if (((n & 15) == 0 || n == width) && x0 == 0)
            task->reportBytes(totalUnits - sliceUnits +
                                  qint64(n) * height,
                              totalUnits);
        }
      };
      std::vector<std::thread> workers;
      // 异常路径也必须 join：主线程分片（processColumn/分配）抛异常时，
      // 仍 joinable 的 workers 析构会 std::terminate 杀掉整个进程——
      // 这里应是任务失败回执，不是进程崩溃（与 attr 扫描路径同根因纪律）。
      struct JoinWorkers
      {
        std::vector<std::thread> &ws;
        ~JoinWorkers()
        {
          for (auto &w : ws)
            if (w.joinable())
              w.join();
        }
      } joinWorkers{workers};
      WorkerExceptionSink workerErrors; // PW-014：工作分片异常转运，不 terminate
      for (int t = 1; t < nThreads; ++t)
      {
        const int x0 = std::min(width, t * chunk);
        const int x1 = std::min(width, x0 + chunk);
        if (x0 >= x1)
          break;
        workers.emplace_back(workerErrors.wrap(runRange), x0, x1);
      }
      runRange(0, std::min(width, chunk));
      for (auto &w : workers)
        w.join();
      workerErrors.rethrowIfAny();
      if (cancelled())
        return QString();
      // R2-SF-3：终态入账（与相干分支同款）——分片计数在取消/提前退出时
      // 不到 width，成功路径补满防进度停在 <100%。
      task->reportBytes(totalUnits, totalUnits);
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

  // finding-95：剖面属性登记幂等——SATR 容器内嵌 createdAt/readMs/computeMs，
  // 同名重写的字节必然不同（sha 口径在此失效），按参数指纹 + 产物在盘比对
  // 当前版本：命中即复用，不重复建版本行（对齐 viz.ts/vol 的 param_hash
  // 口径；参数变了才建新版本——旧版本行不再被同名覆盖搅得 sha256 失真、
  // 版本表不再随重复登记无限膨胀）。
  QCryptographicHash ph(QCryptographicHash::Sha256);
  ph.addData("paleo-attr-section-v1\n");
  ph.addData(result.attrId.toUtf8());
  ph.addData("\n");
  ph.addData(sectionKey.toUtf8());
  ph.addData(QByteArray::number(result.sectionIndex));
  ph.addData("\n");
  ph.addData(QByteArray::number(params.windowHalfSamples) + "," +
             QByteArray::number(params.coherenceIlHalf) + "," +
             QByteArray::number(params.coherenceXlHalf) + "," +
             QByteArray::number(params.coherenceTimeHalf) + "," +
             QByteArray::number(params.coherenceWeighting) + "\n");
  const QString paramHash = QString::fromLatin1(ph.result().toHex());

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
  v.extra.insert(QStringLiteral("param_hash"), paramHash);
  const CatalogVersion existing = catalog->currentVersion(asset.id);
  if (!existing.id.isEmpty() &&
      existing.extra.value(QStringLiteral("param_hash")).toString() ==
          paramHash &&
      QFileInfo::exists(existing.path))
    return existing.path; // 同参数版本已登记且产物在盘 → 复用
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
                                           std::shared_ptr<const SgyVolume> volume,
                                           const SgySectionOptions &options)
{
  // FNV-1a 状态用无符号：有符号乘法溢出是 UB（UBSan），无符号回绕良定义且位模式不变。
  quint64 h = 1469598103934665603ull;
  const auto mix = [&h](qint64 v) {
    h ^= static_cast<quint64>(v);
    h *= 1099511628211ull;
  };
  if (volume && volume->Index())
  {
    const SgyIndex &idx = *volume->Index();
    // 体身份：路径 + 大小 + mtime。同尺寸不同体（原始/滤波/属性体）不能只靠
    // fileSize 区分；体被重写后 mtime 变化使旧条目自然失配。
    // #320：走 UTF-16 → UTF-8。MSVC 上 path::string() 是 ACP 字节，遇 ACP
    // 无法表示的字符直接抛 std::system_error，缓存键构造会崩。
    const std::string p = paleo::fromFsPath(idx.path).toUtf8().constData();
    for (const char ch : p)
      mix(qint64(static_cast<quint8>(ch)));
    mix(qint64(idx.fileSize));
    mix(idx.modifiedTimeTicks);
    const QFileInfo source(paleo::fromFsPath(idx.path));
    mix(source.size());mix(source.lastModified().toMSecsSinceEpoch());mix(source.metadataChangeTime().toMSecsSinceEpoch());
    for(char c:paleo::area_model::cacheToken(source.absoluteFilePath()).toUtf8())mix(static_cast<unsigned char>(c));
  }
  // 提取选项：maxColumns/interpolate/keepOutsideColumns 不同则剖面不同
  mix(options.maxColumns);
  mix(options.interpolate ? 1 : 0);
  mix(options.keepOutsideColumns ? 1 : 0);
  for (const glm::ivec2 &pt : pathPoints)
  {
    mix(pt.x);
    mix(pt.y);
  }
  return static_cast<qint64>(h);
}

std::shared_ptr<const SgySliceImage> SeismicTaskService::cachedSection(
    const std::vector<glm::ivec2> &pathPoints,
    std::shared_ptr<const SgyVolume> volume,
    const SgySectionOptions &options,
    SgySectionStats *stats) const
{
  const qint64 key = sectionCacheKey(pathPoints, volume, options);
  for (auto &entry : sectionCache_)
  {
    if (entry.key == key)
    {
      entry.lastUse = ++sectionCacheClock_;
      if (stats && entry.stats)
        *stats = *entry.stats; // 真实 stats 随图像一起入缓存，绝不合成
      return entry.image;
    }
  }
  return nullptr;
}

void SeismicTaskService::cacheSection(const std::vector<glm::ivec2> &pathPoints,
                                      std::shared_ptr<const SgyVolume> volume,
                                      std::shared_ptr<const SgySliceImage> image,
                                      std::shared_ptr<const SgySectionStats> stats,
                                      const SgySectionOptions &options)
{
  const qint64 key = sectionCacheKey(pathPoints, volume, options);
  for (auto it = sectionCache_.begin(); it != sectionCache_.end();)
    it = (it->key == key) ? sectionCache_.erase(it) : it + 1;
  sectionCache_.push_back({key, image, stats, ++sectionCacheClock_});
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

namespace
{
  // 公共深度轴（两曲线深度并集排序去重）+ 线性插值对齐
  void mergeCurveDepths(const std::vector<double> &acDepthsM,
                        const std::vector<double> &denDepthsM, std::vector<double> *depths)
  {
    depths->reserve(acDepthsM.size() + denDepthsM.size());
    depths->insert(depths->end(), acDepthsM.begin(), acDepthsM.end());
    depths->insert(depths->end(), denDepthsM.begin(), denDepthsM.end());
    std::sort(depths->begin(), depths->end());
    depths->erase(std::unique(depths->begin(), depths->end()), depths->end());
  }
  const auto curveInterp = [](const std::vector<double> &xs, const std::vector<float> &ys,
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

  // 共用数值核：阻抗序列 + 任意时深映射 → 反射系数 → Ricker 褶积 → 归一化。
  // twtAt 返回非有限值（表外不外推等）→ 该界面跳过。
  SeismicTaskService::SeismicSyntheticResult syntheticCore(
      const std::vector<double> &acDepthsM, const std::vector<float> &acUsPerM,
      const std::vector<double> &denDepthsM, const std::vector<float> &denValues,
      const std::function<double(double)> &twtAt, double rickerHz)
  {
    SeismicTaskService::SeismicSyntheticResult result;
    std::vector<double> depths;
    mergeCurveDepths(acDepthsM, denDepthsM, &depths);

    // 波阻抗 Z = ρ · V（V = 1e6 / DT μs/m，m/s）
    std::vector<double> z;
    z.reserve(depths.size());
    for (double d : depths)
    {
      const float dtUs = curveInterp(acDepthsM, acUsPerM, d);
      const float rho = curveInterp(denDepthsM, denValues, d);
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
      const double twt = twtAt((depths[i] + depths[i - 1]) * 0.5);
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
    // B-1（full-audit-20261021）：w(t) = (1−2π²f²t²)·e^(−π²f²t²)——系数是
    // π²（≈9.87）不是 2π（≈6.28；对照 algorithms/inversion/wavelet.cpp
    // makeRicker 的权威实现）。写成 2π 时零点半宽 1/(2f√π)≈11.3ms（正确
    // 9.0ms）、等效主频 √(2/π)≈0.798×，比反演子波低 ~20%。
    const double piSq = std::acos(-1.0) * std::acos(-1.0);
    const double f2 = rickerHz * rickerHz;
    for (double t = std::max(0.0, tStart); t <= tEnd; t += dtMs)
    {
      double amp = 0.0;
      for (std::size_t k = 0; k < rcTwt.size(); ++k)
      {
        const double tau = t - rcTwt[k]; // 褶积：子波平移到反射点（ms→s 折算在 arg）
        const double arg = piSq * f2 * (tau / 1000.0) * (tau / 1000.0);
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
} // namespace

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
  return syntheticCore(acDepthsM, acUsPerM, denDepthsM, denValues,
                       [&tdModel](double d) { return tdModel.DepthToTwtMs(d); }, rickerHz);
}

SeismicTaskService::SeismicSyntheticResult SeismicTaskService::computeSyntheticSeismogram(
    const std::vector<double> &acDepthsM, const std::vector<float> &acUsPerM,
    const std::vector<double> &denDepthsM, const std::vector<float> &denValues,
    const std::vector<double> &tdDepthM, const std::vector<double> &tdTwtMs, double rickerHz)
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
  if (tdDepthM.size() != tdTwtMs.size() || tdDepthM.size() < 2)
  {
    result.reason = QStringLiteral("时深表缺失或无效——合成记录需时深标定");
    return result;
  }
  // 表内线性插值；表外 → NaN（不外推，界面如实跳过）。
  return syntheticCore(
      acDepthsM, acUsPerM, denDepthsM, denValues,
      [&tdDepthM, &tdTwtMs](double d) -> double {
        if (d < tdDepthM.front() || d > tdDepthM.back())
          return std::numeric_limits<double>::quiet_NaN();
        const auto it = std::lower_bound(tdDepthM.begin(), tdDepthM.end(), d);
        const std::size_t hi = std::size_t(it - tdDepthM.begin());
        if (hi == 0)
          return tdTwtMs.front();
        const std::size_t lo = hi - 1;
        if (lo + 1 >= tdTwtMs.size() || !(tdDepthM[hi] > tdDepthM[lo]))
          return tdTwtMs[lo];
        const double f = (d - tdDepthM[lo]) / (tdDepthM[hi] - tdDepthM[lo]);
        return tdTwtMs[lo] + f * (tdTwtMs[hi] - tdTwtMs[lo]);
      },
      rickerHz);
}

} // namespace seismic
