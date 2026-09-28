// 层：数据
#pragma once
#include <QObject>
#include <QMutex>
#include <QQueue>
#include <QHash>
#include <QPair>
#include <QString>
#include <QVector>
#include <functional>
#include <atomic>

// §41.2 — sole write choke point for project.gpkg / metadata.sqlite / .qgz.
// All writes enqueue here; ordering contract per save: gpkg commit -> .qgz backup ->
// .qgz atomic write (temp+rename). .qgz failure after gpkg commit is recoverable
// (display state regenerable); gpkg failure aborts the sequence.
// Mechanism (not just discipline): callers route through enqueueWrite(); a write
// mutex serializes actual file mutation. Processing/GDAL outputs must go
// temp-then-merge (service boundary), never direct-to-gpkg inside task code.
class PaleoProjectStore : public QObject
{
  Q_OBJECT
  public:
    struct WriteResult { bool ok; QString error; };

    explicit PaleoProjectStore(QObject *parent = nullptr);

    void setProjectPaths(const QString &qgzPath, const QString &gpkgPath, const QString &metaSqlitePath);

    // 单写实例降级（T4/SCHEMA_MIGRATION.md §6）：工程目录被另一实例持锁时
    // 由组装根置 true——enqueueWrite/saveAll/commitAll（gpkg/qgz/journal 的
    // 全部落盘路径）如实失败，读面与 busy 注册表照常。
    void setReadOnly(bool readOnly) { m_readOnly = readOnly; }
    bool isReadOnly() const { return m_readOnly; }
    QString gpkgPath() const { return m_gpkgPath; }
    QString metaDbPath() const { return m_metaPath; }

    // Serialized write: fn runs while holding the write mutex. Thread-safe enqueue.
    WriteResult enqueueWrite(const std::function<WriteResult()> &fn);

    // Whole-save sequence per contract ordering. Backs up .qgz before rewriting.
    WriteResult saveAll(const std::function<WriteResult()> &gpkgCommit,
                        const std::function<WriteResult()> &writeQgz);

    // ---- data/commit-coord：journal + 幂等有序提交 ----
    // 对齐 CommitCoordinator 语义（记档先于执行、阶段单调推进、同 opId 重入
    // 幂等），规模裁到本工程的两单元写序。

    // 提交操作记档（持久化为 <工程>/artifacts/metadata/commit_journal/<opId>.json）。
    // stage 词表：queued → catalog_done → qgz_done → complete；"corrupt" 表示
    // journal 文件存在但不可解析——同样属于未完成，需人工处置。
    struct CommitOp {
      QString opId;
      QString stage;
      QString digest;   // 记档时留下的输入摘要（续跑校验用）
    };

    // 幂等有序提交：catalog 提交恒先于 .qgz 写（与 saveAll 同向——「catalog
    // 说已存 ⇒ 工程文件已写」）。流程：落 queued 记档 → catalogCommit → 记
    // catalog_done → .qgz 备份（同 saveAll）→ qgzWrite → 记 qgz_done → 记
    // complete。任一步失败即停，journal 停在最后成功的阶段。
    // 重入语义：journal=complete → no-op 成功（回调不执行）；journal 未完成
    // 且 inputDigest 与记档一致 → 跳过已完成单元续跑剩余；摘要不同或 journal
    // 损坏/阶段词表外 → 如实报错，不猜不重放。inputDigest 由调用方对输入内容
    // 定值（内容 hash 或稳定参数摘要），是「同一 op 是否同一输入」的判据。
    WriteResult commitAll(const QString &opId, const QString &inputDigest,
                          const std::function<WriteResult()> &catalogCommit,
                          const std::function<WriteResult()> &qgzWrite);

    // 崩溃恢复扫描：列出 commit journal 目录里全部未 complete 的记档（含
    // corrupt）。只报告，不重放——工程打开路径调用一次做 honest 上报，
    // 处置交给操作者（重新保存即以同 opId 续跑）。
    QVector<CommitOp> recoverCommitJournal() const;

    // journal 目录：<工程目录>/artifacts/metadata/commit_journal（工程目录
    // 由 .qgz 路径推导，meta 路径兜底；都未设 → 空串）。
    QString commitJournalDir() const;

    // journal 封顶清理：complete 记档是幂等重入凭证、只增不减——按文件修改
    // 时间保留最新 keepComplete 条、其余删除；未完成（含 corrupt）记档永不
    // 清理，始终留给 recoverCommitJournal 上报。返回删除条数；目录不存在
    // 或锁不住时如实回 0（清理失败不阻塞打开路径）。
    int pruneCommitJournal(int keepComplete = 32);

    // Per-layer busy registry shared with ToolAvailabilityService (§35).
    void markLayerBusy(const QString &layerId, const QString &taskId, const QString &reason);
    void markLayerFree(const QString &layerId);
    bool layerBusy(const QString &layerId, QString *reason = nullptr) const;

    struct BusyEntry { QString layerId; QString taskId; QString reason; };
    QVector<BusyEntry> busyLayers() const; // snapshot for UI (task panel)

  signals:
    void writeCompleted(const QString &target);
    void writeFailed(const QString &target, const QString &error);

  private:
    QString projectDir() const;
    QString commitJournalPath(const QString &opId) const;
    bool writeCommitJournal(const QString &opId, const QString &stage,
                            const QString &digest, QString *error) const;
    CommitOp readCommitJournal(const QString &path) const;

    QString m_qgzPath, m_gpkgPath, m_metaPath;
    bool m_readOnly = false;
    mutable QMutex m_writeMutex;
    QHash<QString, QPair<QString, QString>> m_busy; // layerId -> (taskId, reason)
    mutable QMutex m_busyMutex;
};
