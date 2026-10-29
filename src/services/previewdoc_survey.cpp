// 层：数据
#include "previewdoc.h"
#include "catalog/datacatalog.h"
#include "io/areamodel.h"
#include "paleotaskservice.h"
#include <paleo_data/seismic.h>
#include <cmath>

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
