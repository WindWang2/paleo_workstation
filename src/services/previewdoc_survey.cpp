// 层：数据
#include "previewdoc.h"
#include "catalog/datacatalog.h"
#include "io/areamodel.h"
#include "paleotaskservice.h"
#include <paleo_data/seismic.h>
#include <paleo_data/service.h>
#include <QFileInfo>
#include <cmath>
#include <functional>

namespace {
// 数据服务 status + 首两条提醒拼成界面直显文案；不在这里改写语义。
QString surveyDetail(const paleo::data::SeismicCoordinateRange &range)
{
  QStringList parts;
  if (!range.status.empty())
    parts << QString::fromUtf8(range.status);
  for (size_t i = 0; i < range.warnings.size() && i < 2; ++i)
    parts << QString::fromUtf8(range.warnings[i]);
  return parts.join(QStringLiteral("；"));
}
}

void PreviewDocService::requestSurveyBounds(const QString &assetId,
    std::function<void(const SurveyBounds &)> done)
{
  const int generation = ++m_surveyGeneration;
  if (m_surveyTask)
    m_surveyTask->requestCancel();
  const QString path = assetSource(assetId);
  auto bounds = std::make_shared<SurveyBounds>();
  const auto work = [path, bounds](PaleoTask *task) -> QString {
    paleo::data::SeismicCoordinateRange range;
    QString error;
    const auto cancelled = [task] { return task && task->cancelRequested(); };
    const auto state = paleo::area_model::surveyRange(path, range, &error, cancelled);
    if (state == paleo::area_model::Read::Unbound)
    {
      // 未登记进工区模型：不是失败，如实报 Unbound，由界面说明原因，
      // 不回落到重新解析 SEG-Y（那正是本次要退掉的重复几何计算）。
      bounds->state = SurveyBounds::State::Unbound;
      bounds->detail = error;
      return QString();
    }
    if (state == paleo::area_model::Read::Failed)
      return error;
    bounds->coordinateUnit = QString::fromUtf8(range.coordinateUnit);
    bounds->rmsResidual = range.fitRmsResidual;
    bounds->detail = surveyDetail(range);
    if (!std::isfinite(range.minX) || !std::isfinite(range.minY)
        || !std::isfinite(range.maxX) || !std::isfinite(range.maxY))
    {
      bounds->state = SurveyBounds::State::Unavailable;
      return QString();
    }
    bounds->state = SurveyBounds::State::Ready;
    bounds->minX = range.minX;
    bounds->minY = range.minY;
    bounds->maxX = range.maxX;
    bounds->maxY = range.maxY;
    return QString();
  };
  const auto apply = [this, generation, bounds, done](bool ok, const QString &error) {
    if (generation != m_surveyGeneration)
      return;
    m_surveyTask.clear();
    if (!ok && bounds->state != SurveyBounds::State::Unavailable)
    {
      bounds->state = SurveyBounds::State::Unavailable;
      if (bounds->detail.isEmpty())
        bounds->detail = error;
    }
    if (done)
      done(*bounds);
  };
  if (m_taskSvc) {
    auto *task = m_taskSvc->start(tr("读取测区平面范围"), work, QString(), true);
    m_surveyTask = task;
    connect(task, &PaleoTask::finished, this, [apply, task] {
      // 取消也走 apply：done 必须恰好回调一次，否则调用方 UI 永远挂在
      // 「正在后台恢复」（旧实现 Cancelled 分支直接 return，done 永不执行）。
      // 世代判定在 apply 内已有陈旧保护——新一代请求的结果照常丢弃；取消
      // 态给 Unavailable + 错误文本，与 requestSection 的
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

AreaPlanarRange toAreaRange(const paleo::data::PlanarExtent &extent)
{
  AreaPlanarRange out;
  out.valid = extent.valid;
  out.minX = extent.minX; out.minY = extent.minY;
  out.maxX = extent.maxX; out.maxY = extent.maxY;
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
    // 与 requestSurveyBounds 同一数据服务口径（areamodel 包装）：井/地震/
    // 合并三块范围一次读出，合并语义（约定确认与一致性）由服务侧裁决。
    paleo::data::ProjectPlanarRange range;
    QString error;
    if (paleo::area_model::projectRange(root, range, &error) == paleo::area_model::Read::Failed)
      return error;
    result->wells = toAreaRange(range.wells);
    result->seismic = toAreaRange(range.seismic);
    result->merged = toAreaRange(range.merged);
    result->status = QString::fromUtf8(range.status);
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
