// 层：功能
#pragma once

#include <QObject>
#include <QMap>
#include <QString>
#include <QVector>
#include <functional>

#include "../domain/importrows.h" // FolderPreviewRow / FolderRowResult（domain 纯数据）

class DataImportService;
class PaleoTaskService;

// workflow/folderimport — 「工区文件夹导入」+「单文件导入」编排（W2：从主窗下沉）。
//
// 视图只发意图：壳收集目录/覆盖类型后调本类；本类负责
//   · previewFolder —— 列出行不导入（确认表数据源）；
//   · importFolder —— 整批导入，有任务服务时跑 worker（行进度回报），
//     无则同步旧路径；完成回调总在 GUI 线程发；
//   · importFolderRow —— 单行重试（forceType 由视图按当前下拉值算好）；
//   · importFile —— 单文件导入（同上任务/同步双路径）；
//   · importedWellHeadAsset —— 入库后按行文件名反查井口资产（catalog 读）。
// 编排期间发 importActiveChanged(true/false)，壳据此抑制逐文件预览标签。
// 本类不碰控件、不弹窗——全部视图出口经调用方给的回调回壳。
class FolderImportWorkflow : public QObject
{
  Q_OBJECT

  public:
    // done(rows, importErr)：res 为空且 importErr 非空 = 整体失败（视图可重试）。
    using ImportDone =
        std::function<void(const QVector<FolderRowResult> &, const QString &)>;

    FolderImportWorkflow(DataImportService *svc, PaleoTaskService *taskSvc,
                         QObject *parent = nullptr);

    DataImportService *importService() const { return m_svc; }
    void setTaskService(PaleoTaskService *taskSvc) { m_taskSvc = taskSvc; }

    // 确认表预览：与 importFolder 同一枚举/分类口径，只列行不导入。
    QVector<FolderPreviewRow> previewFolder(const QString &dir, QString *error = nullptr);
    // 单行导入/重试：forceType 空 = 按分类器原类型。
    FolderRowResult importFolderRow(const QString &path, const QString &forceType,
                                    QString *error = nullptr);
    // 整批导入：异步（任务池在场）或同步；done 总在 GUI 线程调用。
    void importFolder(const QString &dir, const QMap<QString, QString> &overrides,
                      const ImportDone &done);
    // 单文件导入（数据页「导入」按钮）：同 importFolder 的双路径语义。
    void importFile(const QString &kind, const QString &path,
                    std::function<void(const QString &assetId, const QString &error)> done);
    // 按行源文件名反查刚入库的井口资产 id（找不到 → 空）。
    QString importedWellHeadAsset(const QString &rowPath) const;

  signals:
    // 导入编排进行中（worker 或同步段）——壳把逐文件预览标签抑制挂上。
    void importActiveChanged(bool active);

  private:
    DataImportService *m_svc = nullptr;
    PaleoTaskService *m_taskSvc = nullptr;
};
