// 层：数据
#include "piprocess.h"
#include "piprotocol.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include "piprocessplatform_p.h"

namespace paleo::pi {
namespace {
bool peer(const QJsonObject &value,const QJsonObject &request,const VerifiedRuntime &runtime,bool closing) {
  auto expected=request;expected.insert("type",closing?"closed":"hello_result");
  if(!closing) {
    expected.insert("upstreamCommit",runtime.upstreamCommit());expected.insert("coreVersion",runtime.coreVersion());
    expected.insert("nodeVersion",runtime.nodeVersion());expected.insert("toolExecution","sequential");
    expected.insert("modelTransport","host-only-unconnected");expected.insert("sandboxed",false);
  }
  return value==expected;
}
QString receive(QProcess &process,QByteArray &buffer,QByteArray &logs) {
  process.setReadChannel(QProcess::StandardError);
  while(process.bytesAvailable()) {
    const auto bytes=process.read(16*1024);const auto left=MaxDiagnosticBytes-logs.size();
    if(bytes.isEmpty())return "pi_process_read_failed";
    logs.append(bytes.constData(),qMin<qsizetype>(left,bytes.size()));
    if(bytes.size()>left)return "pi_diagnostic_budget";
  }
  process.setReadChannel(QProcess::StandardOutput);
  while(process.bytesAvailable()) {
    const auto bytes=process.read(16*1024);
    if(bytes.isEmpty())return "pi_process_read_failed";
    if(bytes.size()>MaxFrameBytes+4-buffer.size())return "pi_frame_budget";
    buffer+=bytes;
  }
  return {};
}
}
HandshakeResult runHandshake(const HandshakeRequest &request) {
  HandshakeResult result;
  if(!request.enabled){result.code="pi_disabled";return result;}
  if(QCoreApplication::instance()&&QThread::currentThread()==QCoreApplication::instance()->thread()) {
    result.code="pi_worker_required";return result;
  }
#if defined(Q_OS_LINUX) && QT_VERSION < QT_VERSION_CHECK(6,6,0)
  result.code="pi_process_qt_unsupported";return result;
#elif !defined(Q_OS_WIN) && !defined(Q_OS_LINUX)
  result.code="pi_platform_unsupported";return result;
#else
  if(!request.runtime){result.code="pi_runtime_identity_required";return result;}
  if(!validSession(request.sessionId)||!validCounter(request.generation)){result.code="pi_request_invalid";return result;}
  QElapsedTimer clock;clock.start();bool cancelled=false,timedOut=false;
  const auto stopped=[&] {
    if(request.cancelled&&request.cancelled())cancelled=true;
    if(clock.elapsed()>=5000)timedOut=true;return cancelled||timedOut;
  };
  const auto finish=[&] {
    if(stopped())result.code=cancelled?"cancelled":"pi_handshake_timeout";
    if(result.code!="ok")result.peer={};return result;
  };
  static QSemaphore one(1);
  while(!one.tryAcquire(1,25))if(stopped())return finish();
  const auto permit=qScopeGuard([&]{one.release();});
  const auto verified=inspectRuntime({true,request.runtime->root(),request.runtime->manifestSha256()},stopped);
  if(!verified.ok()){result.code=verified.code;return finish();}
  const auto &runtime=*verified.runtime;
  QTemporaryDir directory;if(!directory.isValid()){result.code="pi_work_directory_failed";return finish();}
  const auto environment=detail::isolatedProcessEnvironment(directory.path());
  if(environment.isEmpty()){result.code="pi_loader_unavailable";return finish();}
  detail::ProcessTree tree;
  if(!tree.available()){result.code="pi_process_tree_unavailable";return finish();}
  QProcess process;process.setProcessEnvironment(environment);process.setWorkingDirectory(directory.path());
  process.setProcessChannelMode(QProcess::SeparateChannels);
  tree.configure(process);
  if(stopped())return finish();
  process.start(runtime.nodeProgram(),{"--no-addons","--disable-proto=throw","--no-warnings",runtime.entryPoint()});
  if(!process.waitForStarted(int(qMax<qint64>(1,qMin<qint64>(1000,5000-clock.elapsed()))))) {
    if(process.state()!=QProcess::NotRunning){process.kill();process.waitForFinished(2000);}
    result.code="pi_process_start_failed";return finish();
  }
  const auto cleanup=qScopeGuard([&]{if(process.state()!=QProcess::NotRunning){process.kill();process.waitForFinished(2000);}});
  const auto finishProcess=[&] {
    if(process.state()!=QProcess::NotRunning) {
      process.kill();if(!process.waitForFinished(2000)){result.code="pi_process_reap_failed";result.peer={};return result;}
    }
    return finish();
  };
  if(!tree.attach(process)){result.code="pi_process_tree_unavailable";return finishProcess();}
  QJsonObject sent{{"protocol",QLatin1String(Protocol)},{"type","hello"},{"sessionId",request.sessionId},
    {"generation",request.generation},{"sequence","1"},{"nonce",QUuid::createUuid().toString(QUuid::Id128)}};
  const auto send=[&]{const auto frame=encodeFrame(sent);return !frame.isEmpty()&&process.write(frame)==frame.size();};
  if(!send()){result.code="pi_process_write_failed";return finishProcess();}
  QByteArray buffer;bool closing=false,done=false;
  while(process.state()!=QProcess::NotRunning) {
    if(stopped())break;
    result.code=receive(process,buffer,result.diagnostics);if(!result.code.isEmpty())break;
    QJsonObject response;bool complete=false;
    if(!takeFrame(buffer,response,complete,result.code))break;
    if(complete) {
      if(done||!buffer.isEmpty()||!peer(response,sent,runtime,closing)){result.code="pi_frame_state";break;}
      if(closing){done=true;process.closeWriteChannel();}
      else {
        result.peer=response;closing=true;sent.insert("type","close");sent.insert("sequence","2");
        if(!send()){result.code="pi_process_write_failed";break;}
      }
    }
    process.waitForReadyRead(20);
  }
  if(result.code.isEmpty()&&!stopped()) {
    result.code=receive(process,buffer,result.diagnostics);
    if(result.code.isEmpty()&&!done) {
      QJsonObject response;bool complete=false;
      if(takeFrame(buffer,response,complete,result.code)&&complete&&closing&&peer(response,sent,runtime,true))done=true;
    }
    if(result.code.isEmpty())result.code=done&&buffer.isEmpty()&&process.exitStatus()==QProcess::NormalExit&&process.exitCode()==0?"ok":"pi_process_failed";
  }
  return finishProcess();
#endif
}
}
