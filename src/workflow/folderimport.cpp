// 层：功能
#include "folderimport.h"

#include "../io/dataimportservice.h"
#include "../catalog/datacatalog.h"
#include "../services/paleotaskservice.h"

#include <QFileInfo>
#include <QPointer>

FolderImportWorkflow::FolderImportWorkflow(DataImportService *svc,
                                           PaleoTaskService *taskSvc,
                                           QObject *parent)
    : QObject(parent), m_svc(svc), m_taskSvc(taskSvc)
{
}

QVector<FolderPreviewRow>
FolderImportWorkflow::previewFolder(const QString &dir, QString *error)
{
  if (!m_svc)
  {
    if (error)
      *error = QStringLiteral("导入服务未就绪");
    return {};
  }
  return m_svc->previewFolder(dir, error);
}

FolderRowResult
FolderImportWorkflow::importFolderRow(const QString &path, const QString &forceType,
                                      QString *error)
{
  if (!m_svc)
  {
    if (error)
      *error = QStringLiteral("导入服务未就绪");
    return {};
  }
  emit importActiveChanged(true);
  const FolderRowResult res = m_svc->importFolderRow(path, forceType, error);
  emit importActiveChanged(false);
  return res;
}

void FolderImportWorkflow::importFolder(const QString &dir,
                                        const QMap<QString, QString> &overrides,
                                        const ImportDone &done)
{
  if (!m_svc)
  {
    if (done)
      done({}, QStringLiteral("导入服务未就绪"));
    return;
  }
  // D1b：任务池在场 → 整个文件夹导入（含 LAS 解析/层位装箱/SEG-Y 索引）
  // 跑 worker 线程，行进度回报到任务页；catalog 操作经服务内 marshal 回
  // GUI。worker 的 imported 信号排队顺序先于 finished——槽里抑制标签的
  // 标志在终态回调复位，不会漏开逐文件标签。
  if (m_taskSvc)
  {
    emit importActiveChanged(true);
    auto outRows = std::make_shared<QVector<FolderRowResult>>();
    auto outErr = std::make_shared<QString>();
    PaleoTask *task = m_taskSvc->start(
        tr("导入工区文件夹"),
        [svc = m_svc, dir, overrides, outRows, outErr](PaleoTask *t) -> QString {
          *outRows = svc->importFolder(
              dir, outErr.get(), overrides,
              [t](int d, int total, const QString &p) {
                t->reportBytes(d, total);
                t->reportDetail(p);
                return !t->cancelRequested();
              });
          return *outErr;
        });
    connect(task, &PaleoTask::finished, this,
            [this, done, outRows, outErr] {
              emit importActiveChanged(false);
              if (done)
                done(*outRows, *outErr);
            });
    return;
  }
  emit importActiveChanged(true);
  QString importErr;
  const auto res = m_svc->importFolder(dir, &importErr, overrides);
  emit importActiveChanged(false);
  if (done)
    done(res, importErr);
}

void FolderImportWorkflow::importFile(
    const QString &kind, const QString &path,
    std::function<void(const QString &assetId, const QString &error)> done)
{
  if (!m_svc)
  {
    if (done)
      done(QString(), QStringLiteral("导入服务未就绪"));
    return;
  }
  // D1b：LAS 解析/SEG-Y 索引等大文件在任务池跑——无任务服务时保持
  // 同步旧路径。imported 信号照常排队回 GUI（预览标签在终态后开）。
  if (m_taskSvc)
  {
    auto outErr = std::make_shared<QString>();
    auto outId = std::make_shared<QString>();
    PaleoTask *task = m_taskSvc->start(
        tr("导入 %1").arg(kind),
        [svc = m_svc, kind, path, outId, outErr](PaleoTask *) -> QString {
          *outId = svc->importFile(kind, path, outErr.get());
          return outId->isEmpty() ? *outErr : QString();
        });
    connect(task, &PaleoTask::finished, this,
            [done, outId, outErr] {
              if (done)
                done(*outId, *outErr);
            });
    return;
  }
  QString err;
  const QString assetId = m_svc->importFile(kind, path, &err);
  if (done)
    done(assetId, err);
}

QString FolderImportWorkflow::importedWellHeadAsset(const QString &rowPath) const
{
  // 找回刚入库的井口资产：按文件名在 catalog 里定位。
  DataCatalog *cat = m_svc ? m_svc->catalog() : nullptr;
  if (!cat)
    return {};
  const QString fileName = QFileInfo(rowPath).fileName();
  for (const auto &a : cat->assets())
    if (a.type == QLatin1String("well_head") &&
        cat->currentVersion(a.id).fileName == fileName)
      return a.id;
  return {};
}
