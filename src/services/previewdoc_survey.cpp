// 层：数据
#include "previewdoc.h"
#include "catalog/datacatalog.h"
#include "io/areamodel.h"
#include "paleotaskservice.h"
#include <paleo_data/service.h>
#include <QFileInfo>
#include <cmath>
#include <functional>

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
  out.coordinateSystem=QString::fromUtf8(extent.coordinateSystem);
  out.coordinateUnit=QString::fromUtf8(extent.coordinateUnit);
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
    // areamodel 包装的数据服务口径：井/地震/合并三块范围一次读出，
    // 合并语义（约定确认与一致性）由服务侧裁决。
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
