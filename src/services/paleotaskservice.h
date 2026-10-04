// 层：数据
#pragma once
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QVector>
#include <atomic>
#include <deque>
#include <functional>

class QThreadPool;
class PaleoProjectStore;

// services/ — PaleoTaskService: 异步任务注册中心 + 池化执行（autoplan pass-2
// 批准的 D1/D2）。SEG-Y 道索引/SHA 校验、LAS 解析、层位装箱、单线解码、文件
// 夹导入等长 IO 跑在 QThreadPool::global()，不再阻塞 GUI；任务经排队回调把
// 字节进度发回主线程，任务页以 1s 粒度刷进度条、以最近 ≤10s 字节速率线性估
// 剩余时间；取消是协作式的——worker 在循环里自查 cancelRequested()。
//
// worker 签名：std::function<QString(PaleoTask*)>，返回错误串（空 = 成功）。
// 结束态由服务统一判：cancelRequested() 为真 → Cancelled（忽略返回值）；
// 返回非空 → Failed；否则 Succeeded。worker 内报进度用 reportBytes/
// reportDetail（内部经 QueuedConnection 回主线程，任意线程可直接调），节流
// 要求 ≤20Hz——每 ~50ms 或每若干 MB 报一次，别逐字节发。
class PaleoTask : public QObject
{
  Q_OBJECT
public:
  enum class State { Running, Succeeded, Failed, Cancelled };

  // D4.6 优先级（QThreadPool 语义：高优先级先出队）。用户交互路径 High/
  // Normal，预取与后台扫描 Low。
  enum class Priority { Low = 0, Normal = 1, High = 2 };

  qint64 id() const { return m_id; }
  QString title() const { return m_title; }
  QString layerId() const { return m_layerId; }
  State state() const { return m_state; }
  QString errorText() const { return m_error; }
  QString detailText() const { return m_detail; }
  qint64 bytesDone() const { return m_bytesDone; }
  qint64 bytesTotal() const { return m_bytesTotal; }
  // bytesTotal>0 → 字节百分比；无总量运行中 → -1（面板跑马灯）；
  // Succeeded → 100；Failed/Cancelled → 停在最后进度。
  int percent() const;
  QString etaText() const; // 「约 Ns」/「约 Nm」/「--」（速率未知或已结束）
  bool running() const { return m_state == State::Running; }
  // quiet：交互内嵌任务（切片/剖面解码/预取等）——照常进任务页列表、可取消，
  // 但不触发主窗口任务中心自动露出（交互控件自带进度语义，弹出会打断操作）。
  bool quiet() const { return m_quiet; }
  // 发起时的任务会话号（PaleoTaskService::session()）。工程切换开新会话，
  // 提交端比对 task->session() != service->session() 即知结果已过期。
  quint64 session() const { return m_session; }

  Q_INVOKABLE void requestCancel() { m_cancel.store(true); }
  bool cancelRequested() const { return m_cancel.load(); }

  // worker 侧上报：非 GUI 线程可直调，内部排队回主线程。
  void reportBytes(qint64 done, qint64 total);
  void reportDetail(const QString &detail);
  // D4.3 阶段化进度：stage 用标准词表（scan/parse/index/decode/hash/build/
  // publish）；percent 无意义时 -1（字节进度仍走 reportBytes）。ETA 面不替
  // 换——阶段是给人看的分组，速率仍按字节估。
  void reportStage(const QString &stage, int percent = -1);
  QString stage() const { return m_stage; }
  int stagePercent() const { return m_stagePercent; }
  bool isFinished() const { return m_state != State::Running; }

signals:
  void changed();
  void finished(); // 终态后读 state()/errorText()

private:
  friend class PaleoTaskService;
  PaleoTask(qint64 id, const QString &title, const QString &layerId,
            QObject *parent);
  Q_INVOKABLE void applyProgress(qint64 done, qint64 total);
  Q_INVOKABLE void applyDetail(const QString &detail);
  Q_INVOKABLE void applyStage(const QString &stage, int percent);
  Q_INVOKABLE void applyFinish(const QString &error);

  qint64 m_id;
  QString m_title, m_layerId, m_error, m_detail;
  QString m_stage;
  int m_stagePercent = -1;
  State m_state = State::Running;
  bool m_quiet = false;
  quint64 m_session = 0;
  std::atomic_bool m_cancel{false};
  qint64 m_bytesDone = 0, m_bytesTotal = -1;
  double m_rateBytesPerSec = -1.0; // 最近 ≤10s 窗口字节速率；-1 不可估
  QElapsedTimer m_clock;
  std::deque<QPair<qint64, qint64>> m_etaSamples; // (elapsedMs, bytesDone)
};

class PaleoTaskService : public QObject
{
  Q_OBJECT
public:
  explicit PaleoTaskService(PaleoProjectStore *store = nullptr,
                            QObject *parent = nullptr);
  ~PaleoTaskService() override;

  // layerId 非空时在 store busy 注册表挂账（§35 工具门在任务期挡该层），
  // 任务终态自动 markLayerFree。返回的 task 属本服务，finished() 信号可接。
  // quiet=true：任务仍注册/可取消，但不拉起任务中心——交互内嵌取数专用。
  PaleoTask *start(const QString &title,
                   std::function<QString(PaleoTask *)> work,
                   const QString &layerId = QString(), bool quiet = false);

  // D4.6 带优先级启动（预取 Low / 用户点击 High）。同签名 3 参版默认 Normal。
  PaleoTask *start(const QString &title,
                   std::function<QString(PaleoTask *)> work,
                   const QString &layerId, PaleoTask::Priority priority,
                   bool quiet = false);

  QVector<PaleoTask *> tasks() const { return m_tasks; }
  void clearFinished(); // 移除非运行态行（运行中任务永不删）

  // 关停排空（H-2）：对所有运行中任务 requestCancel、清空排队，再等 worker
  // 全部退出（≤timeoutMs；<0 不限时）。pumpEvents=true 且在主线程时等待期间
  // 泵事件，让 worker 排队回主线程的进度/终态回包与其它排队调用落地（导入
  // 链已不再 BlockingQueued 回主线程——旧 catInvoke 已移除；泵事件保留给仍
  // 可能这样做的调用方，tst_taskservice 覆盖该互锁回归）。返回
  // false = 超时仍有 worker 在跑。宿主（AppContext）应在析构依赖对象之前调用；
  // 析构函数自身也会（不泵事件地）排空。幂等。
  bool shutdown(int timeoutMs = kShutdownWaitMs, bool pumpEvents = true);
  static constexpr int kShutdownWaitMs = 10000;

  // D4.5 线程池纪律：本服务任务跑专用池，工作线程上限 4（j4 资源纪律精神；
  // 不再与 UI 侧共用 globalInstance 的无上限默认）。返回池配置面（测试/诊断）。
  int maxWorkerThreads() const;
  void setMaxWorkerThreads(int n); // 夹取 [1,8]；只影响之后的排队
  int runningCount() const;

  // 任务会话（#153/#124）：工程即将关闭/切换时宿主调 beginNewSession()——
  // 会话号自增，所有运行中/排队任务 requestCancel（终态必为 Cancelled，
  // 下游 JobRunner/finished 槽据此丢弃旧工程结果）；尚未出队的任务出队时
  // 直接跳过 work。返回新会话号。
  quint64 beginNewSession();
  quint64 session() const { return m_session; }

  // #164 任务注册表保留策略：quiet 任务终态后 quietLingerMs 自动移除；
  // 非 quiet 终态任务最多保留 maxFinished 条（超出删最旧）。运行中任务
  // 永不删。<0 关闭对应策略。删除走 deleteLater——持有方须用 QPointer。
  void setRetention(int maxFinished, int quietLingerMs);
  int maxFinishedRetained() const { return m_maxFinished; }
  int quietLingerMs() const { return m_quietLingerMs; }
  static constexpr int kDefaultMaxFinished = 50;
  static constexpr int kDefaultQuietLingerMs = 5000;

signals:
  void taskAdded(PaleoTask *task);
  void tasksChanged(); // 行集合变化（新增/清理），面板据此重建

private:
  friend class PaleoTask;
  void onTaskFinished(PaleoTask *task);
  PaleoTask *createTask(const QString &title, const QString &layerId, bool quiet);
  void removeTask(PaleoTask *task);
  void pruneFinished();

  quint64 m_session = 1;
  int m_maxFinished = kDefaultMaxFinished;
  int m_quietLingerMs = kDefaultQuietLingerMs;

  PaleoProjectStore *m_store;
  qint64 m_nextId = 1;
  QVector<PaleoTask *> m_tasks;
  QThreadPool *m_pool = nullptr; // D4.5 专用 ≤4 工作线程池
};
