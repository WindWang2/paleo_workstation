// 层：功能
#include "pythonconsolecontroller.h"

#include "../services/pythonenv.h"
#include "../services/pythonrepl.h"
#include "../services/scriptrunner.h"
#include "../services/scriptprotocol.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

PythonConsoleController::PythonConsoleController(ScriptRunnerService *runner,
                                                 PythonReplSession *repl,
                                                 QObject *parent)
    : QObject(parent), m_runner(runner), m_repl(repl)
{
  m_interpreter = PythonEnvService::findBasePython();

  connect(m_runner, &ScriptRunnerService::runQueued, this,
          [this](qint64 runId, const QString &scriptPath) {
            if (m_active.contains(runId))
              emit runQueued(runId, scriptPath);
          });
  connect(m_runner, &ScriptRunnerService::runStarted, this,
          [this](qint64 runId, const QString &scriptPath) {
            if (m_active.contains(runId))
              emit runStarted(runId, scriptPath);
          });
  connect(m_runner, &ScriptRunnerService::runOutput, this,
          &PythonConsoleController::onRunnerOutput);
  connect(m_runner, &ScriptRunnerService::runFinished, this,
          &PythonConsoleController::onRunnerFinished);

  connect(m_repl, &PythonReplSession::started, this,
          &PythonConsoleController::replStarted);
  connect(m_repl, &PythonReplSession::output, this,
          &PythonConsoleController::replOutput);
  connect(m_repl, &PythonReplSession::finished, this,
          &PythonConsoleController::replFinished);
  connect(m_repl, &PythonReplSession::startFailed, this,
          &PythonConsoleController::replFailed);
}

QString PythonConsoleController::recordStatusText(const RunRecord &record)
{
  const auto tr = [](const char *text) {
    return QCoreApplication::translate("PythonConsoleController", text);
  };
  if (record.cancelled)
    return tr("已取消");
  if (record.timedOut)
    return tr("超时终止");
  if (record.crashed)
    return tr("异常退出");
  if (record.exitCode == 0)
    return tr("成功");
  return tr("退出码 %1").arg(record.exitCode);
}

QStringList PythonConsoleController::splitArgs(const QString &argsText)
{
  return argsText.split(QRegularExpression(QStringLiteral("\\s+")),
                        Qt::SkipEmptyParts);
}

bool PythonConsoleController::replRunning() const
{
  return m_repl && m_repl->isRunning();
}

void PythonConsoleController::refreshInterpreter()
{
  const QString found = PythonEnvService::findBasePython();
  if (found == m_interpreter)
    return;
  m_interpreter = found;
  emit interpreterChanged(m_interpreter);
}

void PythonConsoleController::runScript(const QString &scriptPath,
                                        const QString &argsText,
                                        const QString &workingDir)
{
  if (m_interpreter.isEmpty())
  {
    emit consoleLine(tr("未找到 Python 解释器——请安装 Python 3，或设置 "
                        "PALEO_PYTHON 环境变量后点「重新检测」"),
                     Error);
    return;
  }
  ScriptRunnerService::RunRequest request;
  request.scriptPath = scriptPath;
  request.args = splitArgs(argsText);
  request.workingDir = workingDir;
  request.interpreter = m_interpreter;
  QString error;
  const qint64 id = m_runner->start(request, &error);
  if (id <= 0)
  {
    emit consoleLine(tr("启动失败：%1").arg(error), Error);
    return;
  }
  // 与 runner 的解析口径一致（相对路径按 workingDir → 绝对），补发信号与
  // 历史记录统一用解析后的拼写（runner 自带信号里也是这个口径）。
  QString resolvedScript = scriptPath;
  if (QDir(resolvedScript).isRelative() && !workingDir.isEmpty())
    resolvedScript = QDir(workingDir).absoluteFilePath(resolvedScript);
  resolvedScript = QFileInfo(resolvedScript).absoluteFilePath();
  ActiveRun active;
  active.scriptPath = resolvedScript;
  active.args = request.args;
  // 与 runner 的生效口径一致：空工作目录 = 脚本所在目录。
  active.workingDir =
      workingDir.isEmpty() ? QFileInfo(resolvedScript).absolutePath() : workingDir;
  m_active.insert(id, active);
  updateBusy();
  // runner 的 runQueued/runStarted 在 start() 内同步发出——早于 m_active
  // 登记，转发过滤器收不到——这里按实际落位补发一次；排队项日后被泵起
  // 时的 runStarted 则走正常转发路径。
  if (m_runner->isQueued(id))
    emit runQueued(id, resolvedScript);
  else
    emit runStarted(id, resolvedScript);
}

void PythonConsoleController::cancelAll()
{
  // m_active 在取消回调（runFinished）里逐项移除——先取 id 快照再迭代。
  const QList<qint64> ids = m_active.keys();
  for (const qint64 id : ids)
    m_runner->cancel(id);
}

void PythonConsoleController::startRepl()
{
  if (m_repl)
    m_repl->start(m_interpreter);
}

void PythonConsoleController::stopRepl()
{
  if (m_repl)
    m_repl->stop();
}

void PythonConsoleController::sendReplLine(const QString &line)
{
  if (m_repl)
    m_repl->sendLine(line);
}

void PythonConsoleController::onRunnerOutput(qint64 runId, const QString &line,
                                             bool stderrChannel)
{
  if (!m_active.contains(runId))
    return; // 非本控制器发起的运行：不呈（共享 runner 的其他编排各自展示）
  if (stderrChannel)
  {
    emit consoleLine(line, Stderr);
    return;
  }
  const ScriptMessage msg = parseScriptLine(line);
  switch (msg.type)
  {
    case ScriptMessage::Type::Progress:
      emit progressChanged(runId, qBound(0, msg.percent, 100), msg.message);
      break;
    case ScriptMessage::Type::Result:
    {
      QString path = msg.path;
      if (path.isEmpty())
      {
        // 诚实面：协议行了产出但没带路径——如实记系统行，不造空结果项。
        emit consoleLine(tr("脚本报告了产出但未给出路径"), System);
        break;
      }
      if (QDir(path).isRelative())
        path = QDir(m_active.value(runId).workingDir).absoluteFilePath(path);
      emit resultProduced(path, msg.kind, msg.message);
      emit consoleLine(tr("产出：%1").arg(path), System);
      break;
    }
    case ScriptMessage::Type::Error:
      emit consoleLine(tr("脚本错误：%1（码 %2）").arg(msg.message).arg(msg.code),
                       Error);
      break;
    case ScriptMessage::Type::Text:
      emit consoleLine(line, Stdout);
      break;
  }
}

void PythonConsoleController::onRunnerFinished(qint64 runId,
                                               const QString &scriptPath,
                                               int exitCode, bool crashed,
                                               bool cancelled, bool timedOut)
{
  const auto it = m_active.find(runId);
  if (it == m_active.end())
    return;
  RunRecord record;
  record.runId = runId;
  record.scriptPath = it->scriptPath;
  record.args = it->args;
  record.exitCode = exitCode;
  record.crashed = crashed;
  record.cancelled = cancelled;
  record.timedOut = timedOut;
  record.finishedAt = QDateTime::currentDateTime();
  m_active.erase(it);
  m_history.prepend(record);
  while (m_history.size() > historyLimit)
    m_history.removeLast();
  emit consoleLine(
      tr("结束：%1 —— %2").arg(scriptPath, recordStatusText(record)), System);
  emit runFinished(record);
  updateBusy();
}

void PythonConsoleController::updateBusy()
{
  const bool busy = !m_active.isEmpty();
  if (busy == m_busy)
    return;
  m_busy = busy;
  emit busyChanged(busy);
}
