// 层：数据
#pragma once
#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;
class QTimer;

// services/scriptrunner.h — 用户 Python 脚本运行服务（方向68：脚本面底座）。
// 只管「异步跑一个 .py」：解释器沿用 PythonEnvService 发现序（请求可覆盖），
// stdout/stderr 按整行流式回传，退出码/超时/取消（terminate→kill 递进，
// Unix 走进程组连坐脚本拉起的子进程）齐全；并发闸缺省同时 ≤2、超出排队
// （PaleoTaskService 排队先例；拒收变体见 jobrunner，本方向定案排队）。
// 不解释输出内容——JSON 行协议在 scriptprotocol（展示侧解析），本服务对
// 脚本零强制：不遵守协议的脚本照常运行、输出按纯文本回传。
//
// 安全口径：脚本执行恒为显式用户动作；本服务不做沙箱、不内置任何自动
// 下载执行，脚本以其自身权限运行，安全性由用户自担（面板与
// tools/reference/scripts/README.md 同步声明）。
class ScriptRunnerService : public QObject
{
  Q_OBJECT
  public:
    explicit ScriptRunnerService(QObject *parent = nullptr);
    ~ScriptRunnerService() override;

    struct RunRequest
    {
      QString scriptPath;   // .py 文件（须存在；相对路径按 workingDir 解析）
      QStringList args;     // argv 透传（不经 shell，逐元素传递）
      QString workingDir;   // 空 = 脚本所在目录
      QString interpreter;  // 空 = PythonEnvService::findBasePython() 发现序
      int timeoutMs = 0;    // 0 = 不限时；超时按与取消相同的递进终止
    };

    // 同步前置校验（脚本存在/解释器可执行）失败返回 0 且 error 带出中文原因；
    // 成功返回运行 id（>0）——闸内空闲即启动（runStarted），否则排队
    // （runQueued）。解释器发现序见 PythonEnvService::findBasePython。
    qint64 start(const RunRequest &request, QString *error = nullptr);
    // 运行中：terminate→kill 递进；排队中：直接出队了结。幂等（未知 id 忽略）。
    void cancel(qint64 runId);

    bool isRunning(qint64 runId) const;
    bool isQueued(qint64 runId) const;
    int runningCount() const;
    int queuedCount() const;
    // 运行中进程的 OS pid（排队/已结束/未启动完成返回 0）——诊断与孤儿断言用。
    qint64 processId(qint64 runId) const;

    int maxConcurrent() const { return m_maxConcurrent; }
    // 钳制 1..4（方向定案缺省 2）；调大后立即补泵队列，调小不抢占已运行项。
    void setMaxConcurrent(int n);

  signals:
    void runQueued(qint64 runId, const QString &scriptPath);
    void runStarted(qint64 runId, const QString &scriptPath);
    // 整行回传（不带换行符）；stderrChannel 区分通道。进程退出时缓冲里的
    // 残缺行作为最后一行在 runFinished 之前冲刷发出。
    void runOutput(qint64 runId, const QString &line, bool stderrChannel);
    // exitCode 仅正常退出（!crashed）时有意义；crashed=异常退出/启动失败；
    // cancelled=经 cancel() 终止；timedOut=超时被终止（与 cancelled 互斥）。
    void runFinished(qint64 runId, const QString &scriptPath, int exitCode,
                     bool crashed, bool cancelled, bool timedOut);
    // busy = 运行中 + 排队中 > 0（闸口径，避免泵队列时的忙态闪烁）。
    void busyChanged(bool busy);

  private:
    struct Run;
    void pumpQueue();
    void launch(Run *run);
    void terminateGracefully(Run *run);
    void finishRun(Run *run, int exitCode, bool crashed);
    void drainChannel(Run *run, bool stderrChannel, bool flush);
    Run *findRun(qint64 runId) const;
    void updateBusy();

    QList<Run *> m_running;
    QList<Run *> m_queue;
    qint64 m_nextId = 1;
    int m_maxConcurrent = 2;
    bool m_busy = false;
    bool m_shuttingDown = false;
};
