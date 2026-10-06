// 层：功能
#pragma once
#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

class ScriptRunnerService;
class PythonReplSession;

// workflow/pythonconsolecontroller.h — Python 脚本面编排（方向68）。
// 面板（ui/python/）只渲染 + 发意图；这里做：解释器解析（沿用
// PythonEnvService::findBasePython 发现序）、argv 拆分、运行/取消编排、
// JSON 行协议解释（services/scriptprotocol）、运行历史（内存封顶 20 条，
// 方向定案）、REPL 会话转发。不画像素、不弹对话框、不碰 io。
class PythonConsoleController : public QObject
{
  Q_OBJECT
  public:
    // consoleLine 通道（信号里以 int 传输，测试按值断言）。
    enum Channel { Stdout = 0, Stderr = 1, System = 2, Error = 3 };

    // 方向定案：运行历史内存封顶 20 条（不持久化——历史是会话内回看，
    // 不是审计台账）。面板历史列表同口径引用本常量。
    static constexpr int historyLimit = 20;

    struct RunRecord
    {
      qint64 runId = 0;
      QString scriptPath;
      QStringList args;
      int exitCode = -1;
      bool crashed = false;
      bool cancelled = false;
      bool timedOut = false;
      QDateTime finishedAt;
    };

    PythonConsoleController(ScriptRunnerService *runner, PythonReplSession *repl,
                            QObject *parent = nullptr);

    // 空串 = 未找到解释器（面板据此呈禁用态 + 引导，诚实面）。
    QString interpreter() const { return m_interpreter; }
    bool busy() const { return m_busy; }
    const QList<RunRecord> &history() const { return m_history; }
    bool replRunning() const;

    static QString recordStatusText(const RunRecord &record);
    // 参数框文本 → argv：按空白拆分（不支持引号——面板占位符如实说明）。
    static QStringList splitArgs(const QString &argsText);

  public slots:
    // 重新解析解释器（用户装好 Python 后点「重新检测」）；值变化才发信号。
    void refreshInterpreter();
    void runScript(const QString &scriptPath, const QString &argsText,
                   const QString &workingDir);
    void cancelAll(); // 本控制器发起的运行/排队全部取消
    void startRepl();
    void stopRepl();
    void sendReplLine(const QString &line);

  signals:
    void interpreterChanged(const QString &interpreter);
    void runQueued(qint64 runId, const QString &scriptPath);
    void runStarted(qint64 runId, const QString &scriptPath);
    void consoleLine(const QString &text, int channel);
    void progressChanged(qint64 runId, int percent, const QString &message);
    // path 已解析为绝对路径；kind 是脚本的类型提示（可为空，仅展示用）。
    void resultProduced(const QString &absolutePath, const QString &kind,
                        const QString &message);
    void runFinished(const PythonConsoleController::RunRecord &record);
    void busyChanged(bool busy); // 本控制器口径（自有活动/排队运行 > 0）
    void replStarted(const QString &interpreter);
    void replOutput(const QString &text, bool stderrChannel);
    void replFinished(int exitCode, bool crashed);
    void replFailed(const QString &reason);

  private:
    struct ActiveRun
    {
      QString scriptPath;
      QStringList args;
      QString workingDir; // 生效值（与 runner 解析口径一致）
    };
    void onRunnerOutput(qint64 runId, const QString &line, bool stderrChannel);
    void onRunnerFinished(qint64 runId, const QString &scriptPath, int exitCode,
                          bool crashed, bool cancelled, bool timedOut);
    void updateBusy();

    ScriptRunnerService *m_runner = nullptr;
    PythonReplSession *m_repl = nullptr;
    QHash<qint64, ActiveRun> m_active;
    QList<RunRecord> m_history; // 最新在前，封顶 20 条
    QString m_interpreter;
    bool m_busy = false;
};
