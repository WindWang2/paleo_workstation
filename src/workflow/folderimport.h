// 层：功能
#pragma once

#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QMap>
#include <QString>
#include <QVector>
#include <functional>
#include <memory>

#include "../domain/importrows.h" // FolderPreviewRow / FolderRowResult（domain 纯数据）

class DataImportService;
class PaleoTaskService;
class PaleoTask;
struct ImportSession;

// workflow/folderimport — 「工区文件夹导入」+「单文件导入」编排（W2：从主窗下沉）。
//
// 视图只发意图：壳收集目录/覆盖类型后调本类；本类负责
//   · previewFolder —— 列出行不导入（确认表数据源）；
//   · importFolder —— 整批导入，有任务服务时跑 worker（行进度回报），
//     无则同步旧路径；完成回调总在 GUI 线程发；
//   · importFolderRow —— 单行重试（forceType 由视图按当前下拉值算好）；
//   · importFile —— 单文件导入（同上任务/同步双路径）；
//   · importedWellHeadAsset —— 入库后按行源路径反查井口资产（先词法绝对路径、
//     再规范路径，最后唯一同名兜底；catalog 读）。
// 编排期间发 importActiveChanged(true/false)，壳据此抑制逐文件预览标签。
// 本类不碰控件、不弹窗——全部视图出口经调用方给的回调回壳。
//
// 审计 02 M-8（produce-then-commit）：异步导入 = GUI 上 beginImport →
// 任务池 worker 只跑 DataImportService::produce*（写 session 的 staging 副本，
// 活 catalog 零接触）→ PaleoTask::finished（GUI）里 commitImport 一处入库。
// 导入作业 FIFO 串行（一次一个，保持旧的逐批语义）；提交时基线 mutationSeq 已
// 被别处写入改变 → 用新 session 重做（最多 kMaxImportAttempts 次，取消过的
// 作业不重做、按失败落行）。
class FolderImportWorkflow : public QObject
{
  Q_OBJECT

  public:
    // done(rows, importErr)：res 为空且 importErr 非空 = 整体失败（视图可重试）。
    using ImportDone =
        std::function<void(const QVector<FolderRowResult> &, const QString &)>;
    // 精确定向本次作业；可忽略返回值。完成后调用无效，不影响后续或同源作业。
    using CancelImport = std::function<void()>;

    FolderImportWorkflow(DataImportService *svc, PaleoTaskService *taskSvc,
                         QObject *parent = nullptr);

    DataImportService *importService() const; // 服务已析构 → nullptr
    void setTaskService(PaleoTaskService *taskSvc) { m_taskSvc = taskSvc; }

    // 确认表预览：与 importFolder 同一枚举/分类口径，只列行不导入。
    QVector<FolderPreviewRow> previewFolder(const QString &dir, QString *error = nullptr);
    QString classifyFile(const QString &path) const;
    // 预览异步（T2）：任务池在场时扫描/分类/哈希跑 worker（GUI 只收任务
    // 页进度行），done(rows, err) 总在 GUI 线程回调；无任务服务同步旧路径。
    void previewFolderAsync(
        const QString &dir,
        const std::function<void(const QVector<FolderPreviewRow> &,
                                 const QString &)> &done);
    // 单行导入/重试：forceType 空 = 按分类器原类型。
    FolderRowResult importFolderRow(const QString &path, const QString &forceType,
                                    QString *error = nullptr,
                                    const QString &wellTarget = {},
                                    const QString &expectedHash = {});
    // 整批导入：异步（任务池在场）或同步；done 总在 GUI 线程调用。
    CancelImport importFolder(const QString &dir, const QMap<QString, QString> &overrides,
                      const ImportDone &done,
                      const QMap<QString, QString> &wellTargets = {},
                      const QMap<QString, QString> &expectedHashes = {},
                      const QStringList &skippedPaths = {},
                      const QStringList &previewedPaths = {});
    // 同上 + 「仍导入」改判（T2 确认表跳过策略）：forceImportPaths 是用户
    // 对「重复→跳过」行点了「仍导入」的源路径集合——plan 里这些行改判
    // as_new_version（同字节结局交内部 dedup：AlreadyStored + 补挂）。
    // previewedPaths：预览展示过的源路径全集；非空时执行期目录重扫多出的
    // 文件一律 Skipped（F1：确认表所见即所执行）。
    CancelImport importFolder(const QString &dir, const QMap<QString, QString> &overrides,
                      const QStringList &forceImportPaths, const ImportDone &done,
                      const QMap<QString, QString> &wellTargets = {},
                      const QMap<QString, QString> &expectedHashes = {},
                      const QStringList &skippedPaths = {},
                      const QStringList &previewedPaths = {});
    // 单文件导入（数据页「导入」按钮）：同 importFolder 的双路径语义。
    void importFile(const QString &kind, const QString &path,
                    std::function<void(const QString &assetId, const QString &error)> done);
    // 多选文件导入（文件列表口径）：与 importFolder 同一确认表/任务池/台账
    // 语义；预览 importActiveChanged 抑制逐文件预览标签口径相同。
    CancelImport importFiles(const QStringList &paths, const QMap<QString, QString> &overrides,
                     const QStringList &forceImportPaths, const ImportDone &done,
                     const QMap<QString, QString> &wellTargets = {},
                     const QMap<QString, QString> &expectedHashes = {},
                     const QStringList &skippedPaths = {},
                     const QStringList &previewedPaths = {});
    void previewFilesAsync(
        const QStringList &paths,
        const std::function<void(const QVector<FolderPreviewRow> &,
                                 const QString &)> &done);
    // 按行源文件名反查刚入库的井口资产 id（找不到 → 空）。
    QString importedWellHeadAsset(const QString &rowPath) const;
    // 测试/诊断：排队中 + 在途的异步导入作业数。
    int pendingImportJobs() const;

    static constexpr int kMaxImportAttempts = 3;


  signals:
    // 导入编排进行中（worker 或同步段）——壳把逐文件预览标签抑制挂上。
    void importActiveChanged(bool active);
    // 预览扫描进行中（T2 异步预览）——壳可挂忙碌光标/状态栏一行。
    void previewActiveChanged(bool active);

  private:
    struct ImportJob;
    // （#560）整批导入（importFolder/importFiles）的 produce 面：只写
    // session；进度统一经任务页回调（扫描段 total==0 跑马灯 + 文件名
    // detail，执行段 total=行数走 reportBytes + 路径 detail）。
    using BatchProduce = std::function<
        void(ImportSession &,
             const std::function<bool(int done, int total, const QString &path)> &)>;
    // （#560）importFolder/importFiles 的作业脚手架收口：统一 job 构造
    // （produce/finish/fail）+ importActiveChanged 双发（入队前 true、
    // finish/fail 收尾 false）+ enqueue。finish 先判「同一工程」
    // （词法 → canonical 双段比较）再写台账（ledgerDir 只是台账展示位），
    // 后回调 done。
    CancelImport makeImportJob(const QString &title, const BatchProduce &produce,
                       const QString &ledgerDir, const ImportDone &done);
    // （#560）previewFolderAsync/previewFilesAsync 的骨架收口：任务池在场
    // → 扫描/分类/哈希跑 worker（produce 只读 session 的 staging 副本，
    // 进度回报任务页），无任务服务同步走 syncPreview；done 总在 GUI 线程
    // 回调。
    void previewAsync(
        const QString &title,
        const std::function<QVector<FolderPreviewRow>(
            ImportSession &, QString *,
            const std::function<bool(int filesSeen, const QString &path)> &)> &produce,
        const std::function<QVector<FolderPreviewRow>(QString *)> &syncPreview,
        const std::function<void(const QVector<FolderPreviewRow> &,
                                 const QString &)> &done);
    void enqueueImport(const std::shared_ptr<ImportJob> &job);
    void pumpImports();
    void runImportJob(std::shared_ptr<ImportJob> job);
    QVector<FolderRowResult> runInlineImport(
        const std::function<void(ImportSession &)> &produce,
        const QMap<QString, QString> &wellTargets,
        const QMap<QString, QString> &expectedHashes, QString *error,
        const QStringList &skippedPaths = {},
        const QStringList &previewedPaths = {});
    // 方向 30：整批导入完成后写台账（.paleo/import_ledger.json，窗口 50 批）。
    // 写失败 qWarning 如实留痕，不挡导入主路径。
    void recordLedger(const QString &dir, const QDateTime &started,
                      const QVector<FolderRowResult> &rows, const QString &importErr);

    // QPointer：导入作业在飞时服务可能先析构（壳收尾/换工程）——finished
    // 处理器里的 recordLedger/finish 回调必须看到「服务已不在」而不是悬垂指针。
    QPointer<DataImportService> m_svc;
    PaleoTaskService *m_taskSvc = nullptr;
    QList<std::shared_ptr<ImportJob>> m_importQueue;
    std::shared_ptr<ImportJob> m_activeImport;
};
