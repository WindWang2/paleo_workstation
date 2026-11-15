// 层：数据
#include "piconversation.h"
#include "piprotocol.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(Q_OS_LINUX)
#include <signal.h>
#include <sys/prctl.h>
#include <unistd.h>
#endif

namespace paleo::pi {
namespace {
constexpr qint64 MaximumTurnMilliseconds = 30 * 60 * 1000;
bool opened(const QJsonObject &frame, const VerifiedRuntime &runtime) {
  return frame.value("type") == "opened" && frame.value("payload").toObject() == QJsonObject{
    {"upstreamCommit", runtime.upstreamCommit()}, {"coreVersion", runtime.coreVersion()}, {"nodeVersion", runtime.nodeVersion()},
    {"toolExecution", "sequential"}, {"modelTransport", "host-only"}, {"sandboxed", false}};
}
QString readPeer(QProcess &process, QByteArray &buffer, ConversationResult &result,
    const ConversationRequest &request, bool &started, bool &closed) {
  process.setReadChannel(QProcess::StandardError);
  while (process.bytesAvailable()) {
    const auto bytes = process.read(16 * 1024); const auto left = MaxDiagnosticBytes - result.diagnostics.size();
    if (bytes.isEmpty()) return "pi_process_read_failed";
    result.diagnostics.append(bytes.constData(), qMin<qsizetype>(bytes.size(), left));
    if (bytes.size() > left) return "pi_diagnostic_budget";
  }
  process.setReadChannel(QProcess::StandardOutput);
  for (;;) {
    QJsonObject frame; bool complete = false; QString code;
    if (!takeFrame(buffer, frame, complete, code)) return code;
    if (complete) {
      if (closed || (!started && !opened(frame, *request.runtime))) return "pi_conversation_state";
      bool notify = false; if (!request.channel->acceptPeer(frame, notify)) return request.channel->error();
      started = true;
      if (frame.value("type") == "closed") {
        if (!frame.value("payload").toObject().isEmpty()) return "pi_conversation_state";
        closed = true;
      }
      if (notify && request.incoming) request.incoming();
      continue;
    }
    if (!process.bytesAvailable()) return {};
    const auto capacity = MaxFrameBytes + 4 - buffer.size();
    if (capacity <= 0) return "pi_frame_budget";
    const auto bytes = process.read(qMin<qsizetype>(16 * 1024, capacity));
    if (bytes.isEmpty()) return "pi_process_read_failed"; buffer += bytes;
  }
}
} // namespace

ConversationResult runConversation(const ConversationRequest &request) {
  ConversationResult result;
  if (!request.enabled) { result.code = "pi_disabled"; return result; }
  if (QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread()) {
    result.code = "pi_worker_required"; return result;
  }
#if defined(Q_OS_LINUX) && QT_VERSION < QT_VERSION_CHECK(6, 6, 0)
  result.code = "pi_process_qt_unsupported"; return result;
#elif !defined(Q_OS_WIN) && !defined(Q_OS_LINUX)
  result.code = "pi_platform_unsupported"; return result;
#else
  if (!request.runtime || !request.channel || !request.channel->identity().valid()) { result.code = "pi_conversation_request_invalid"; return result; }
  QElapsedTimer clock; clock.start();
  const auto stopped = [&] { return request.channel->stopReason() != 0 || (request.cancelled && request.cancelled()) || clock.elapsed() >= MaximumTurnMilliseconds; };
  static QSemaphore one(1);
  // Waiting here would occupy TaskService workers needed by the host tools.
  if (!one.tryAcquire()) { result.code = "pi_process_busy"; return result; }
  const auto permit = qScopeGuard([&] { one.release(); });
  const auto verified = inspectRuntime({true, request.runtime->root(), request.runtime->manifestSha256()}, stopped);
  if (!verified.ok()) { result.code = verified.code; return result; }
  const auto &runtime = *verified.runtime;
  QTemporaryDir directory; if (!directory.isValid()) { result.code = "pi_work_directory_failed"; return result; }
  QProcessEnvironment environment; environment.insert("LANG", "C.UTF-8");
  for (const auto &key : QStringList{"HOME", "USERPROFILE", "TEMP", "TMP", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME"})
    environment.insert(key, directory.path());
#ifdef Q_OS_WIN
  environment.insert("SystemRoot", qEnvironmentVariable("SystemRoot"));
  const auto job = CreateJobObjectW(nullptr, nullptr);
  if (!job) { result.code = "pi_process_tree_unavailable"; return result; }
  const auto releaseJob = qScopeGuard([&] { CloseHandle(job); });
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
  limits.BasicLimitInformation.ActiveProcessLimit = 1;
  if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) { result.code = "pi_process_tree_unavailable"; return result; }
#endif
  QProcess process; process.setWorkingDirectory(directory.path()); process.setProcessEnvironment(environment);
  process.setProcessChannelMode(QProcess::SeparateChannels);
#ifdef Q_OS_LINUX
  const auto parent = ::getpid();
  process.setChildProcessModifier([parent] {
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || ::prctl(PR_SET_PDEATHSIG, SIGKILL) || ::getppid() != parent) ::_exit(125);
  });
  QProcess::UnixProcessParameters parameters; parameters.flags = QProcess::UnixProcessFlag::CloseFileDescriptors;
  process.setUnixProcessParameters(parameters);
#endif
  if (stopped()) { result.code = "interrupted"; return result; }
  process.start(runtime.nodeProgram(), {"--no-addons", "--disable-proto=throw", "--no-warnings", runtime.entryPoint()});
  const auto cleanup = qScopeGuard([&] { if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(2000); } });
  if (!process.waitForStarted(1000)) { result.code = "pi_process_start_failed"; return result; }
#ifdef Q_OS_WIN
  const auto child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(process.processId()));
  const bool assigned = child && AssignProcessToJobObject(job, child); if (child) CloseHandle(child);
  if (!assigned) { result.code = "pi_process_tree_unavailable"; return result; }
#endif
  QByteArray buffer, pending; bool started = false, closed = false, abortSent = false; QElapsedTimer abortClock;
  while (process.state() != QProcess::NotRunning) {
    if (request.cancelled && request.cancelled()) request.channel->requestStop(false);
    if (clock.elapsed() >= MaximumTurnMilliseconds) { result.code = "pi_turn_timeout"; break; }
    const auto channelCode = request.channel->error();
    if (!channelCode.isEmpty() && channelCode != "pi_conversation_busy") { result.code = channelCode; break; }
    if (request.channel->stopReason() && !abortSent) {
      const auto reason = request.channel->stopReason() == 1 ? "cancelled" : "interrupted";
      if (request.channel->send("abort", {{"reason", reason}})) { abortSent = true; abortClock.start(); }
    }
    if (abortSent && abortClock.elapsed() >= 2000) {
      result.code = request.channel->stopReason() == 1 ? "cancelled" : "interrupted"; break;
    }
    if (pending.isEmpty()) pending = request.channel->takeOutgoing();
    if (!pending.isEmpty() && process.bytesToWrite() <= ConversationQueueBytes - pending.size()) {
      if (process.write(pending) != pending.size()) { result.code = "pi_process_write_failed"; break; } pending.clear();
    }
    result.code = readPeer(process, buffer, result, request, started, closed); if (!result.code.isEmpty()) break;
    process.waitForReadyRead(20);
  }
  if (result.code.isEmpty()) result.code = readPeer(process, buffer, result, request, started, closed);
  if (result.code.isEmpty()) result.code = closed && buffer.isEmpty() && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0 ? "ok" : "pi_process_failed";
  if (request.channel->stopReason()) result.code = request.channel->stopReason() == 1 ? "cancelled" : "interrupted";
  if (process.state() != QProcess::NotRunning) {
    process.kill(); if (!process.waitForFinished(2000)) result.code = "pi_process_reap_failed";
  }
  return result;
#endif
}
} // namespace paleo::pi
