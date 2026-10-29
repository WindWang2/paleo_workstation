// 层：数据
#include "previewdoc.h"
#include "catalog/datacatalog.h"
#include "io/dataimportservice.h"
#include "io/segyreader.h"
#include "paleotaskservice.h"
#include <paleo_data/service.h>
#include <QDir>
#include <QFileInfo>
#include <cmath>
#include <functional>

namespace
{
// 角点写回：探测/全扫拿到的测区角点同步进目录实体——看板与下次开页直接
// 命中，不再每页重探（工程实体是「测区范围」的权威归宿，探测只是补漏）。
void storeSurveyCorners(DataCatalog *catalog, const QString &assetId,
                        const QVector<QPair<double, double>> &corners)
{
  if (!catalog || !catalog->isOpen() || corners.size() < 3)
    return;
  for (const EntityAssetLink &link : catalog->linksForAsset(assetId))
  {
    if (link.unresolved || link.entityType != QLatin1String("seismic_survey"))
      continue;
    CatalogEntity entity = catalog->entityById(link.entityId);
    if (entity.id.isEmpty() || entity.corners == corners)
      continue;
    entity.corners = corners;
    QString error;
    catalog->updateEntity(entity, &error);
    break;
  }
}
} // namespace

void PreviewDocService::requestSurveyBounds(const QString &assetId,
    std::function<void(const QVector<QPair<double, double>> &, const QString &)> done)
{
  const int generation = ++m_surveyGeneration;
  if (m_surveyTask)
    m_surveyTask->requestCancel();
  const QString path = assetSource(assetId);
  const QString cacheDir = m_svc ? m_svc->indexCacheDir() : QString();
  const auto cached = m_segyReaders.value(assetId);
  auto reader = std::make_shared<std::shared_ptr<SegyReader>>(cached);
  auto corners = std::make_shared<QVector<QPair<double, double>>>();
  const auto collectCorners = [corners](const SegyGeometry &geometry) {
    for (int corner = 0; corner < 4; ++corner)
      if (std::isfinite(geometry.cornerX[corner]) && std::isfinite(geometry.cornerY[corner]))
        corners->append({geometry.cornerX[corner], geometry.cornerY[corner]});
    return corners->size() == 4 ? QString() : QObject::tr("完整测区角点不可用");
  };
  // 轻量探测优先：规则网格（外委 SEG-Y 常态）下只读几个道头即得四角，
  // 196 GB 体秒回；探测不过（变道长/方言/非规则）回退 openCached 全扫
  //（磁盘索引命中则免扫，否则后台慢扫——任务面板照常出进度）。
  const auto work = [path, cacheDir, reader, corners, collectCorners](PaleoTask *task) -> QString {
    if (*reader) {
      const auto error = collectCorners((*reader)->geometry());
      return error;
    }
    {
      SegyGeometry probed;
      QString error;
      if (SegyReader::probeGeometry(path, &probed, &error))
        return collectCorners(probed);
    }
    SegyOptions options;
    options.cancel = [task] { return task && task->cancelRequested(); };
    options.progress = [task](qint64 done, qint64 total) {
      if (task) task->reportBytes(done, total);
    };
    *reader = std::make_shared<SegyReader>();
    QString error;
    if (!(*reader)->openCached(path, cacheDir, &error, &options))
      return error;
    return collectCorners((*reader)->geometry());
  };
  const auto apply = [this, assetId, generation, reader, corners, done](bool ok, const QString &error) {
    if (generation != m_surveyGeneration)
      return;
    m_surveyTask.clear();
    if (ok)
      m_segyReaders.insert(assetId, *reader);
    if (ok)
      storeSurveyCorners(catalog(), assetId, *corners);
    if (done)
      done(ok ? *corners : QVector<QPair<double, double>>(), error);
  };
  if (m_taskSvc) {
    auto *task = m_taskSvc->start(tr("恢复完整测区范围"), work, QString(), true);
    m_surveyTask = task;
    connect(task, &PaleoTask::finished, this, [apply, task] {
      // 取消也走 apply：done 必须恰好回调一次，否则调用方 UI 永远挂在
      // 「正在后台恢复」（旧实现 Cancelled 分支直接 return，done 永不执行）。
      // 世代判定在 apply 内已有陈旧保护——新一代请求的结果照常丢弃；取消
      // 态给空角点 + 错误文本，与 requestSection 的
      // seismicSectionCancelled 同口径（完成通知，不是静默丢弃）。
      apply(task->state() == PaleoTask::State::Succeeded, task->errorText());
    });
  } else {
    const QString error = work(nullptr);
    apply(error.isEmpty(), error);
  }
}

namespace
{
// area-model 库 stamp：尺寸+mtime（wal 并入）——导入/编辑后变化，触发重探。
QString planarRangeStamp(const QString &root)
{
  QString stamp;
  for (const auto &suffix : {"/data.sqlite", "/data.sqlite-wal"})
  {
    const QFileInfo file(root + suffix);
    stamp += QString(":%1:%2").arg(file.size()).arg(file.lastModified().toMSecsSinceEpoch());
  }
  return stamp;
}

AreaPlanarRange toAreaRange(const paleo::data::PlanarRange &range)
{
  AreaPlanarRange out;
  out.valid = range.valid;
  out.minX = range.minX; out.minY = range.minY;
  out.maxX = range.maxX; out.maxY = range.maxY;
  return out;
}
} // namespace

void PreviewDocService::requestProjectPlanarRange(std::function<void(const AreaPlanarRanges &)> done)
{
  DataCatalog *catalog = this->catalog();
  const QString root = catalog && catalog->isOpen()
                           ? areaModelDirectory(catalog->projectDir())
                           : QString();
  if (root.isEmpty() || !QFileInfo::exists(root + QStringLiteral("/data.sqlite")))
  {
    done(AreaPlanarRanges{});
    return;
  }
  const QString stamp = planarRangeStamp(root);
  if (stamp == m_planarRangeStamp)
  {
    done(m_planarRangeCache);
    return;
  }
  const int generation = ++m_planarGeneration;
  if (m_planarTask)
    m_planarTask->requestCancel();
  const auto result = std::make_shared<AreaPlanarRanges>();
  const auto work = [root, result](PaleoTask *) -> QString {
    paleo::data::DataService service;
    std::string error;
    paleo::data::ProjectPlanarRange range;
    if (!service.openProjectReadOnly(root.toUtf8().toStdString(), error) ||
        !service.readProjectPlanarRange(range, error))
      return QString::fromUtf8(error);
    result->wells = toAreaRange(range.wells);
    result->seismic = toAreaRange(range.seismic);
    result->merged = toAreaRange(range.merged);
    return QString();
  };
  const auto apply = [this, generation, result, stamp, done](bool ok, const QString &error) {
    if (generation != m_planarGeneration)
      return;
    m_planarTask.clear();
    if (ok)
    {
      m_planarRangeStamp = stamp;
      m_planarRangeCache = *result;
    }
    if (done)
      done(ok ? *result : AreaPlanarRanges{});
    if (!ok && !error.isEmpty())
      qWarning() << "project planar range:" << error;
  };
  if (m_taskSvc)
  {
    auto *task = m_taskSvc->start(QObject::tr("读取工区范围"), work, QString(), true);
    m_planarTask = task;
    connect(task, &PaleoTask::finished, this, [apply, task] {
      apply(task->state() == PaleoTask::State::Succeeded, task->errorText());
    });
  }
  else
  {
    const QString error = work(nullptr);
    apply(error.isEmpty(), error);
  }
}

void PreviewDocService::requestSurveyLinePreview(long long modelVolumeId, const QString &direction,
                                                 int lineNumber,
                                                 std::function<void(const SurveyLinePreview &)> done)
{
  DataCatalog *catalog = this->catalog();
  const QString root = catalog && catalog->isOpen()
                           ? areaModelDirectory(catalog->projectDir())
                           : QString();
  if (root.isEmpty() || modelVolumeId <= 0)
  {
    done(SurveyLinePreview{});
    return;
  }
  const auto result = std::make_shared<SurveyLinePreview>();
  const auto work = [root, modelVolumeId, direction, lineNumber, result](PaleoTask *task) -> QString {
    paleo::data::DataService service;
    std::string error;
    if (!service.openProjectReadOnly(root.toUtf8().toStdString(), error))
      return QString::fromUtf8(error);
    paleo::data::SectionReadOptions options;
    options.maxColumns = 384;         // 预览列数（服务侧按振幅预算再抽稀）
    options.maxValues = 6000000;
    paleo::data::SeismicSectionResult section;
    if (!service.readSeismicLine(modelVolumeId, direction.toStdString(), lineNumber,
                                 options, section, error,
                                 [task] { return task && task->cancelRequested(); }))
      return QString::fromUtf8(error);
    result->ok = true;
    result->columns = section.columns;
    result->samples = section.samples;
    result->firstTimeMs = section.firstTime;
    result->sampleIntervalMs = section.sampleInterval;
    result->amplitudes = QVector<float>(section.amplitudes.begin(), section.amplitudes.end());
    result->title = QObject::tr("%1 %2 时间剖面 (%3 道)")
                        .arg(direction == QLatin1String("inline") ? QStringLiteral("INLINE")
                                                                  : QStringLiteral("XLINE"),
                             QString::number(lineNumber))
                        .arg(section.columns);
    return QString();
  };
  const auto apply = [this, result, done](bool ok, const QString &error) {
    if (!ok)
    {
      SurveyLinePreview failed;
      failed.error = error.isEmpty() ? QObject::tr("测线读取失败") : error;
      if (done)
        done(failed);
      return;
    }
    if (done)
      done(*result);
  };
  if (m_taskSvc)
  {
    auto *task = m_taskSvc->start(QObject::tr("读取地震时间剖面"), work, QString(), true);
    connect(task, &PaleoTask::finished, this, [apply, task] {
      apply(task->state() == PaleoTask::State::Succeeded, task->errorText());
    });
  }
  else
  {
    const QString error = work(nullptr);
    apply(error.isEmpty(), error);
  }
}
