// 层：数据
#pragma once
#include <QObject>
#include <QString>

class QProcess;
class QTimer;

// services/pythonrepl.h — 交互式 Python（python -i -u -q）会话桥（方向68，
// 实验性）。一块 QProcess 长会话：stdin 写入 + stdout/stderr 原文回显；不做
// 语法高亮/补全/对象内省（递延）。停止 = 先写 exit() 走正常退出，1s 未退转
// terminate→kill 递进；析构强制回收（Unix 进程组整杀，
// stratigraphicwebsession 先例），不留孤儿进程。
class PythonReplSession : public QObject
{
  Q_OBJECT
  public:
    explicit PythonReplSession(QObject *parent = nullptr);
    ~PythonReplSession() override;

    bool isRunning() const { return m_proc != nullptr; }
    QString interpreter() const { return m_interpreter; }
    qint64 processId() const { return m_pid; } // 未运行返回 0

  public slots:
    // interpreter 空 = PythonEnvService::findBasePython() 发现序；
    // 已在运行则忽略（幂等）。
    void start(const QString &interpreter = QString());
    // 幂等。先写 exit() 让解释器正常退出（atexit/finally 得以执行），
    // 1s 未退转 terminate→kill 递进。
    void stop();
    // 未运行时静默忽略（面板按 isRunning 禁用输入，此属防御路径）。
    void sendLine(const QString &line);

  signals:
    void started(const QString &interpreter);
    // 原文块回显（未按行切分——REPL 提示符不带换行）；stderrChannel 区分通道
    //（CPython 的提示符与 traceback 都走 stderr，属正常口径不是错误）。
    void output(const QString &text, bool stderrChannel);
    void finished(int exitCode, bool crashed);
    void startFailed(const QString &reason);

  private:
    void teardown(int exitCode, bool crashed);
    void escalateStop();

    QProcess *m_proc = nullptr;
    QTimer *m_stopTimer = nullptr;
    QString m_interpreter;
    qint64 m_pid = 0;
};
