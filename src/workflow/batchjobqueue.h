// 层：功能
#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include <memory>

#include "../services/jobrunner.h"

class PaleoTask;
class PaleoTaskService;

// workflow/batchjobqueue — 多层位批量计算编排（层：功能）。
//
// 目标形态（方向 33）：把「层位集合 × 方法集合 × 参数模板」声明成一个批次，
// 队列按并发度逐项调度；单作业严格复用既有 JobRunner 三段式路径，不新造
// 计算通道。批次状态落工程 JSON，崩在批中可续跑。
//
// 三条硬纪律：
//   1. 幂等——参数指纹（method+horizon+规范化 params 的 SHA-256）判重。连跑
//      两遍，第二遍全部落 Skipped，不产生新资产/版本。
//   2. 失败隔离——单项失败记原因继续下一项；仅「catalog/工程不可用」这类
//      熔断条件才停全批（熔断词表见 kFatalMarkers）。
//   3. 取消诚实——在跑项协作停止、待跑项作废、已完成项成果保留。状态如实
//      回写，不谎报成功。
//
// 线程模型：所有编排动作在 owner 线程（与 dispatcher 同线程）。每项作业由一个
// JobRunner 槽位承载（JobRunner::start 忙则拒绝，故并发度 = 槽位数），compute
// 在 worker 上跑纯计算，commit 回 owner 线程做资产登记——活 catalog 只被所属
// 线程读写（datacatalog.h 的 owner-thread 守卫），worker 绝不碰。
namespace PaleoBatchQueue
{

/// 批次项状态机。不用 PaleoTask::State 四态——它没有 Queued，排队的批次项
/// 在任务面板上完全不可见，而批次面板必须显示「待跑 N 项」。
enum class ItemState
{
  Pending,    ///< 待跑（尚未出队）
  Queued,     ///< 已入 FIFO，等待空槽
  Running,    ///< compute 在 worker 上
  Succeeded,  ///< 已发布（终态）
  Failed,     ///< 失败终态（原因在 error；不拖停全批）
  Skipped,    ///< 幂等命中：同指纹已完成，本批不重跑（终态，成功语义）
  Cancelled,  ///< 被取消（终态）
};

QString itemStateText( ItemState state );

/// 批次项：一次「某层位 × 某方法 × 某组参数」的调用。
struct BatchItem
{
  QString itemId;      ///< 批内稳定标识（持久化与面板行的主键）
  QString methodId;    ///< 方法标识（generateFactor / contours / faciesPredict …）
  QString horizon;     ///< 层位
  QVariantMap params;  ///< 参数（模板已合并 + 逐项覆写后的终值）
  QString fingerprint; ///< 参数指纹（幂等键）

  // ---- 以下为运行时/持久化字段，非声明面 ----
  ItemState state = ItemState::Pending;
  QString error;         ///< 失败/取消原因
  QString stage;         ///< 最近一次 stage 词表值
  int percent = 0;       ///< 0..100
  qint64 elapsedMs = 0;  ///< 该项耗时（墙钟仅作人读展示，不作性能断言）

  /// 参数指纹：methodId + horizon + 规范化 params 的 SHA-256。
  /// 规范化 = QJsonObject 按键排序后 Compact 输出——键序不同不得影响指纹。
  static QString fingerprintOf( const QString &methodId, const QString &horizon,
                                const QVariantMap &params );
  static QStringList fatalMarkers();

  QJsonObject toJson() const;
  static BatchItem fromJson( const QJsonObject &obj, bool *ok = nullptr );
};

/// 批次定义：层位集合 × 方法集合 × 参数模板。JSON 可序列化。
struct BatchSpec
{
  QString batchId;
  QStringList horizons;             ///< 层位集合
  QStringList methodIds;            ///< 方法集合
  QVariantMap paramTemplates;       ///< 参数模板（逐项覆写的基）
  int concurrency = 2;              ///< 并发度（保守默认；夹取 [1,8]）
  bool stopOnFatal = true;          ///< 熔断：catalog 不可用时停全批

  /// 笛卡尔展开为批次项。逐项覆写写进 paramTemplates 的
  /// 「perItem.<itemId>」子表（itemId = "<horizon>|<methodId>"）。
  QVector<BatchItem> expand( QString *error = nullptr ) const;

  QJsonObject toJson() const;
  static BatchSpec fromJson( const QJsonObject &obj, bool *ok = nullptr );
};

/// 汇总报告：成功/失败/跳过 + 失败原因聚合。
struct BatchReport
{
  QString batchId;
  int total = 0;
  int succeeded = 0;
  int failed = 0;
  int skipped = 0;
  int cancelled = 0;
  bool trippedFatal = false;  ///< 熔断是否触发
  QString fatalReason;        ///< 熔断原因
  qint64 totalElapsedMs = 0;
  QVector<BatchItem> items;   ///< 逐项终态明细
  /// 失败原因聚合：原因文本 → 出现次数（降序）。报告与 UI 共用。
  QVector<QPair<QString, int>> failureHistogram() const;

  /// 人读一行（任务页 detail 与测试断言共用）。
  QString summary() const;
  /// 导出报告 JSON；返回写出路径，写不出则空串。
  QString exportReport( const QString &path, QString *error = nullptr ) const;
  QJsonObject toJson() const;
};

/// 单项作业的执行体：由调用方注入（闭包内调既有工作流，如
/// `generateFactor` / `generateContours` / 相预测）。编排层不新造计算通道——
/// 它只负责「何时调、调完怎么记账」。compute 段跑在 worker 线程，因此实现
/// 必须是纯计算（不碰活 catalog / 不碰 UI）。
///
/// 参数：job（可写入 ok/error）、canceled（自查，返回 true 即协作停止）、
/// progress（percent ∈ [0,100] + stage 词表）。返回 false = 失败。
using ItemExecutor =
    std::function<bool( BatchItem &job, const std::function<bool()> &canceled,
                        const std::function<void( double, const QString &)> &progress )>;

/// 批次编排器（owner 线程）。
///
/// 用法：构造（给任务服务与工程目录）→ setExecutor → start(spec) → 观察信号。
/// 一个编排器同一时刻只跑一个批次（与既有单因素面「忙则拒绝」一致）。
class BatchQueue : public QObject
{
  Q_OBJECT

  public:
    /// projectDir 非空时批次状态落 <projectDir>/artifacts/metadata/batch_jobs/。
    explicit BatchQueue( PaleoTaskService *tasks, const QString &projectDir,
                         QObject *parent = nullptr );
    ~BatchQueue() override;

    void setExecutor( ItemExecutor executor ) { m_executor = std::move( executor ); }
    ItemExecutor executor() const { return m_executor; }

    /// 并发度（槽位数）。夹取 [1,8]。运行中调只影响后续出队。
    void setConcurrency( int n );
    int concurrency() const { return m_concurrency; }

    bool busy() const { return m_running; }
    QString batchId() const { return m_batchId; }
    QVector<BatchItem> items() const { return m_items; }
    BatchReport report() const;
    /// 队列快照：待跑/在跑计数（面板汇总行）。
    int pendingCount() const;
    int runningCount() const;
    int finishedCount() const;

    /// 启动批次。返回 false = 未绑任务服务、无执行体、上一批未完、
    /// 或定义非法（如 error 有值）。同一批次（batchId + 指纹集）重跑时，
    /// 已完成项落 Skipped，只补未完项。
    bool start( const BatchSpec &spec, QString *error = nullptr );

    /// 从工程里恢复批次。找到同名批次则续跑（只补未完项），否则按 spec 起新批。
    bool resume( const BatchSpec &spec, QString *error = nullptr );
    /// 批次状态 JSON 落盘路径（无 projectDir 则空串）。
    QString statePath( const QString &batchId ) const;
    static QString stateDirFor( const QString &projectDir );

    /// 取消：单项（collaborative 停在跑、待跑作废）、整批、失败项重试。
    bool cancelItem( const QString &itemId );
    bool cancelBatch();
    /// 失败项单独重试：回 Pending 并重排（已完成项不受影响）。
    /// 批后（batch 已结束）也可调用——那是最自然的重试时机，此时内部重新起批。
    /// 无失败项时返回 false。会清掉熔断态：重试即「前置条件已恢复」的显式动作。
    bool retryFailed();

  signals:
    void itemChanged( const QString &itemId );
    void batchStarted( const QString &batchId, int total );
    void batchProgress( int done, int total );
    void batchFinished( const QString &batchId, bool allOk );

  private:
    struct Slot;   ///< 一个 JobRunner 槽位（.cpp 定义，含 QObject 成员）

    void pump();                               ///< 尽可能出队填充空槽
    /// 排到下一轮事件循环再 pump。**必须在 commit/drop 回调里用它而不是
    /// pump()**：框架的 clearTask() 在 commit 返回之后才执行，commit 体内
    /// runner->busy() 仍为真，此刻直接 pump 会让 startOnSlot 提前返回——
    /// 而项已被 FIFO takeFirst 出队，于是永久卡在 Queued、批次永不收尾。
    void pumpLater();
    void startOnSlot( int slot, int itemIndex );
    void onSlotFinished( int slot, int itemIndex );
    void finalizeItem( int itemIndex, ItemState state, const QString &error );
    void writeState();
    bool readState( const BatchSpec &spec, QVector<BatchItem> *out, QString *error );
    void markAllQueuedPending();               ///< 取消后：待跑项作废
    bool isFatal( const QString &error ) const;
    void tripFatal( const QString &reason );
    void emitProgress();
    void finishIfDone();

    PaleoTaskService *m_tasks = nullptr;
    QString m_projectDir;
    ItemExecutor m_executor;
    int m_concurrency = 2;

    bool m_running = false;
    QString m_batchId;
    QVector<BatchItem> m_items;
    QHash<QString, int> m_indexById;   ///< itemId → m_items 下标
    QVector<int> m_fifo;               ///< 排队项下标（FIFO）
    QVector<int> m_slotItem;           ///< 槽位 → 在跑项下标（-1 空）
    QVector<qint64> m_startedAtMs;     ///< 槽位 → 该项起跑时刻
    bool m_trippedFatal = false;
    QString m_fatalReason;
    bool m_stopOnFatal = true;         ///< 熔断开关（来自 spec）
    qint64 m_batchStartedAtMs = 0;   ///< 整批起跑时刻（报告总耗时）

    std::vector<std::unique_ptr<Slot>> m_slots;
    QObject *m_ctx = nullptr;          ///< 信号中转（析构自动断连）
};

} // namespace PaleoBatchQueue
