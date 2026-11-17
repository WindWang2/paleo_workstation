// 层：数据
#include "piconversation.h"
#include "piprotocol.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include "piprocessplatform_p.h"

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
  const auto environment = detail::isolatedProcessEnvironment(directory.path());
  detail::ProcessTree tree;
  if (!tree.available()) { result.code = "pi_process_tree_unavailable"; return result; }
  QProcess process; process.setWorkingDirectory(directory.path()); process.setProcessEnvironment(environment);
  process.setProcessChannelMode(QProcess::SeparateChannels);
  tree.configure(process);
  if (stopped()) { result.code = "interrupted"; return result; }
  process.start(runtime.nodeProgram(), {"--no-addons", "--disable-proto=throw", "--no-warnings", runtime.entryPoint()});
  const auto cleanup = qScopeGuard([&] { if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(2000); } });
  if (!process.waitForStarted(1000)) { result.code = "pi_process_start_failed"; return result; }
  if (!tree.attach(process)) { result.code = "pi_process_tree_unavailable"; return result; }
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
