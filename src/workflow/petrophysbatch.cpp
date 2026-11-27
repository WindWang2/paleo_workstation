// 层：功能
#include "services/petrophyscomputeservice.h"
#include "services/paleotaskservice.h"
#include "catalog/datacatalog.h"
#include <QElapsedTimer>
#include <QPointer>

namespace paleo::petrophys {
PaleoTask *PetroPhysTaskService::startBatch(
    const BatchRequest &req, DataCatalog *catalog, const QString &outputDir,
    std::function<void(bool ok, const BatchResult &)> onFinished)
{
  const QString validation = validateRequest(req);
  if (!validation.isEmpty())
  {
    BatchResult r;
    r.error = validation;
    if (onFinished)
      onFinished(false, r);
    return nullptr;
  }
  if (!m_taskSvc)
  {
    BatchResult r;
    r.error = QStringLiteral("任务服务不可用");
    if (onFinished)
      onFinished(false, r);
    return nullptr;
  }

  const auto request = std::make_shared<BatchRequest>(req);
  const auto result = std::make_shared<BatchResult>();
  const auto plans = prepareInputs(catalog, *request);
  auto *store = m_store;
  PaleoTask *task = m_taskSvc->start(
      QStringLiteral("测井计算：%1 × %2 井")
          .arg(formulaName(req.formula))
          .arg(req.wells.size()),
      [request, result, outputDir, store, plans](PaleoTask *task) -> QString {
        QElapsedTimer totalClock;
        totalClock.start();
        const qint64 totalUnits = qint64(request->wells.size()) * 1000;
        task->reportStage(QStringLiteral("scan"));
        task->reportBytes(0, totalUnits);

        for (int w = 0; w < request->wells.size(); ++w)
        {
          if (task->cancelRequested())
          {
            result->error = QStringLiteral("cancelled");
            result->totalMs = double(totalClock.nsecsElapsed()) / 1.0e6;
            return QString(); // 已完成井的成果保留在 result->wells
          }
          const WellRef &well = request->wells.at(w);
          task->reportStage(QStringLiteral("parse"));
          WellResult r = processWell(*plans, *request, well, outputDir, store,
              [task] { task->reportStage(QStringLiteral("compute")); });
          result->wells.append(r);
          task->reportBytes(qint64(w + 1) * 1000, totalUnits);
          QString noteSuffix;
          if (!r.notes.isEmpty())
            noteSuffix = QStringLiteral("；") + r.notes.join(QStringLiteral("；"));
          task->reportDetail(r.ok
                                 ? QStringLiteral("%1：完成（%2 样本，null %3%）%4")
                                       .arg(well.wellId)
                                       .arg(r.stats.n)
                                       .arg(r.stats.nullRate * 100.0, 0, 'f', 1)
                                       .arg(noteSuffix)
                                 : QStringLiteral("%1：%2%3")
                                       .arg(well.wellId, r.error, noteSuffix));
        }
        result->totalMs = double(totalClock.nsecsElapsed()) / 1.0e6;
        return QString(); // 单井失败记入 wells（分井处置），任务本身不算败
      },
      QString(), false);

  if (!task)
  {
    BatchResult r;
    r.error = QStringLiteral("任务提交失败");
    if (onFinished)
      onFinished(false, r);
    return nullptr;
  }

  const QPointer<DataCatalog> destination(catalog);
  const auto project = catalog ? catalog->projectDir() : QString{};
  connect(task, &PaleoTask::finished, this,
          [task, request, result, destination, project, hadCatalog = bool(catalog), onFinished]() {
            // catalog 登记在服务线程做（catalog 非线程安全，worker 不碰；
            // 取消时已完成井也登记——部分成果如实交付）
            if (hadCatalog && request->writeProduct)
            {
              for (WellResult &r : result->wells)
              {
                if (!r.ok || r.productPath.isEmpty())
                  continue;
                if (!destination || !destination->isOpen() || destination->projectDir() != project)
                {
                  r.ok = false;
                  r.error = QStringLiteral("工程已切换，成果文件保留在原工程，尚未登记");
                  continue;
                }
                WellRef well;
                for (const WellRef &w : request->wells)
                {
                  if (w.wellId == r.wellId)
                  {
                    well = w;
                    break;
                  }
                }
                QString err;
                const QString asset =
                    registerComputedCurveAsset(destination, well, *request, r, &err);
                if (asset.isEmpty())
                {
                  r.ok = false;
                  r.error = QStringLiteral("catalog 登记失败：%1").arg(err);
                }
                else
                {
                  r.assetId = asset;
                }
              }
            }
            const bool cancelled = task->cancelRequested();
            result->succeeded = 0;
            result->failed = 0;
            for (const WellResult &r : result->wells)
              r.ok ? ++result->succeeded : ++result->failed;
            result->ok = !cancelled && result->failed == 0 && result->succeeded > 0;
            if (result->error.isEmpty() && result->failed > 0 && !cancelled)
              result->error =
                  QStringLiteral("%1/%2 井失败").arg(result->failed).arg(result->wells.size());
            if (onFinished)
              onFinished(result->ok, *result);
          });
  return task;
}

} // namespace paleo::petrophys
