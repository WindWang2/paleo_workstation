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

void FolderImportWorkflow::previewFolderAsync(
    const QString &dir,
    const std::function<void(const QVector<FolderPreviewRow> &,
                             const QString &)> &done)
{
  if (!m_svc)
  {
    if (done)
      done({}, QStringLiteral("导入服务未就绪"));
    return;
  }
  // T2：任务池在场 → 扫描/分类/哈希跑 worker（plan 经 COW 快照构建，
  // catalog 不被 worker 触碰），扫描进度回报任务页；GUI 零同步整目录扫描。
  // 无任务服务 → 同步旧路径（小环境/测试行为不变）。
  if (m_taskSvc)
  {
    emit previewActiveChanged(true);
    auto outRows = std::make_shared<QVector<FolderPreviewRow>>();
    auto outErr = std::make_shared<QString>();
    PaleoTask *task = m_taskSvc->start(
        tr("扫描工区文件夹"),
        [svc = m_svc, dir, outRows, outErr](PaleoTask *t) -> QString {
          *outRows = svc->previewFolder(
              dir, outErr.get(),
              [t](int seen, const QString &path) {
                t->reportDetail(tr("扫描 %1").arg(QFileInfo(path).fileName()));
                return !t->cancelRequested();
              });
          return outErr->isEmpty() ? QString() : *outErr;
        });
    connect(task, &PaleoTask::finished, this,
            [this, done, outRows, outErr] {
              emit previewActiveChanged(false);
              if (done)
                done(*outRows, *outErr);
            });
    return;
  }
  QString err;
  const QVector<FolderPreviewRow> rows = m_svc->previewFolder(dir, &err);
  if (done)
    done(rows, err);
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
  importFolder(dir, overrides, QStringList{}, done);
}

void FolderImportWorkflow::importFolder(const QString &dir,
                                        const QMap<QString, QString> &overrides,
                                        const QStringList &forceImportPaths,
                                        const ImportDone &done)
{
  if (!m_svc)
  {
    if (done)
      done({}, QStringLiteral("导入服务未就绪"));
    return;
  }
  // D1b+T2：任务池在场 → 整个文件夹导入（plan 扫描/哈希 + LAS 解析/层位
  // 装箱/SEG-Y 索引）跑 worker 线程——plan 期经 COW 快照不再 marshal 回
  // GUI；catalog 写操作仍经服务内 marshal 回 GUI。进度口径：扫描段
  // total==0（跑马灯 + 文件名 detail），执行段 total=行数。worker 的
  // imported 信号排队顺序先于 finished——槽里抑制标签的标志在终态回调
  // 复位，不会漏开逐文件标签。
  if (m_taskSvc)
  {
    emit importActiveChanged(true);
    auto outRows = std::make_shared<QVector<FolderRowResult>>();
    auto outErr = std::make_shared<QString>();
    PaleoTask *task = m_taskSvc->start(
        tr("导入工区文件夹"),
        [svc = m_svc, dir, overrides, forceImportPaths, outRows,
         outErr](PaleoTask *t) -> QString {
          *outRows = svc->importFolder(
              dir, outErr.get(), overrides, forceImportPaths,
              [t](int d, int total, const QString &p) {
                if (total > 0)
                {
                  t->reportBytes(d, total); // 执行段：行进度
                  t->reportDetail(p);
                }
                else
                {
                  t->reportDetail(tr("扫描 %1").arg(QFileInfo(p).fileName()));
                }
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
  const auto res = m_svc->importFolder(dir, &importErr, overrides, forceImportPaths);
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
