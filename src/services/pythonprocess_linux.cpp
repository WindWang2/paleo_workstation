// 层：数据
#include "pythonprocess_linux_internal.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <unistd.h>

namespace paleo::python::process_detail::linux_native {
namespace {
QString cgroupDirectory(const Prepared &prepared) {
  QProcess query;query.setProcessEnvironment(prepared.environment);
  query.start("/usr/bin/systemctl",{"--user","show",prepared.unit+".scope","--property=ControlGroup","--value"});
  if(!query.waitForFinished(300)) {query.kill();query.waitForFinished(1000);return {};}
  const auto path=QString::fromUtf8(query.readAllStandardOutput()).trimmed();
  const auto prefix=QStringLiteral("/user.slice/user-%1.slice/user@%1.service/").arg(::getuid());
  if(!path.startsWith(prefix)||!path.endsWith("/"+prepared.unit+".scope")||path.contains(".."))return {};
  return "/sys/fs/cgroup"+path;
}
QString drain(QProcess &process,ProcessResult &result,const ExecutionPolicy &policy) {
  struct Channel {QProcess::ProcessChannel id;QByteArray *data;qint64 limit;const char *error;};
  const Channel channels[]={{QProcess::StandardOutput,&result.output,policy.outputBytes,"python_process_output_limit"},
    {QProcess::StandardError,&result.error,policy.logBytes,"python_process_log_limit"}};
  for(const auto &channel:channels) {
    process.setReadChannel(channel.id);
    while(process.bytesAvailable()>0) {
      const auto part=process.read(64*1024);
      if(channel.data->size()>channel.limit-part.size())return QLatin1String(channel.error);
      channel.data->append(part);
    }
  }
  process.setReadChannel(QProcess::StandardOutput);return {};
}
void captureStats(QFile &cpu,QFile &memory,QFile &events,ProcessResult &result,bool &oom) {
  // Retain real observed maxima before an empty transient cgroup is collected.
  // A read-only descriptor does not keep that cgroup online indefinitely.
  if(cpu.isOpen()&&cpu.seek(0))for(const auto &line:cpu.read(16*1024).split('\n'))
    if(line.startsWith("usage_usec "))result.cpuMicros=qMax(result.cpuMicros,line.mid(11).toLongLong());
  if(memory.isOpen()&&memory.seek(0))result.peakMemoryBytes=qMax(result.peakMemoryBytes,memory.read(128).trimmed().toLongLong());
  if(events.isOpen()&&events.seek(0))for(const auto &line:events.read(16*1024).split('\n'))
    if(line.startsWith("oom_kill ")&&line.mid(9).toLongLong()>0)oom=true;
}
QString prepareTrusted(const Launch &request,Prepared &prepared) {
  if(!request.policy.allowTrusted)return "python_execution_denied";
  const auto program=QFileInfo(request.program).canonicalFilePath(),working=QFileInfo(request.workingDirectory).canonicalFilePath();
  if(program.isEmpty()||working.isEmpty()||!QFileInfo(program).isExecutable()||!QFileInfo(working).isDir()||
      request.arguments.size()>128)return "python_process_request_invalid";
  const auto code=prepareEnvelope(request,prepared);if(!code.isEmpty())return code;
  prepared.fileBytes=request.policy.outputBytes;
  // Trusted templates retain user filesystem/network privileges; this only contains
  // the process tree and resources. Generated code must use strong preparation.
  prepared.arguments += {"/usr/bin/bwrap","--unshare-user","--unshare-pid","--disable-userns",
    "--die-with-parent","--new-session","--clearenv","--bind","/","/","--dev","/dev","--proc","/proc","--chdir",working};
  const QSet<QString> keys{"PATH","LANG","OPENBLAS_NUM_THREADS","OMP_NUM_THREADS","MKL_NUM_THREADS","NUMEXPR_NUM_THREADS"};
  for(const auto &entry:request.environment) {
    const auto index=entry.indexOf('=');if(index<1||entry.contains(QChar(0))||!keys.contains(entry.left(index)))return "python_process_environment_invalid";
    prepared.arguments += {"--setenv",entry.left(index),entry.mid(index+1)};
  }
  prepared.arguments += {"--block-fd","4","--",program};prepared.arguments += request.arguments;return {};
}
}
ProcessResult runPreparedChild(const Launch &request,Prepared &prepared) {
  ProcessResult result;result.code.clear();
  if(const auto code=request.policy.validate();!code.isEmpty()) {result.code=code;return result;}
  const auto permit=request.permit?request.permit:acquireProcessPermit(request.cancelled);
  if(!permit) {result.code="cancelled";return result;}
  if(request.cancelled&&request.cancelled()) {result.code="cancelled";return result;}
  qint64 bytes=0;
  for(const auto &chunk:request.inputChunks) {
    if(chunk.size()>request.policy.inputBytes+request.policy.codeBytes+4096-bytes) {result.code="python_input_budget_exceeded";return result;}
    bytes+=chunk.size();
  }
  QProcess process;configureProcess(process,prepared,request.policy);
  QElapsedTimer elapsed;elapsed.start();process.start("/usr/bin/systemd-run",prepared.arguments);
  if(!process.waitForStarted(5000)) {result.code="python_process_start_failed";return result;}
  QFile cpu,memory,memoryEvents;bool released=false,writeClosed=false,oom=false;qsizetype chunkIndex=0;qint64 offset=0;
  while(process.state()!=QProcess::NotRunning) {
    if(request.cancelled&&request.cancelled()) {result.code="cancelled";break;}
    if(elapsed.elapsed()>qint64(request.policy.wallSeconds)*1000+1000) {result.code="python_process_timeout";break;}
    if(const auto code=drain(process,result,request.policy);!code.isEmpty()) {result.code=code;break;}
    if(!released) {
      const auto path=cgroupDirectory(prepared);
      if(!path.isEmpty()) {
        if(const auto code=verifyResourceLimits(path,request.policy);!code.isEmpty()) {result.code=code;break;}
        cpu.setFileName(path+"/cpu.stat");memory.setFileName(path+"/memory.peak");memoryEvents.setFileName(path+"/memory.events");
        if(!cpu.open(QIODevice::ReadOnly)||!memory.open(QIODevice::ReadOnly)||!memoryEvents.open(QIODevice::ReadOnly)) {
          result.code="python_linux_resource_accounting_unavailable";break;
        }
        captureStats(cpu,memory,memoryEvents,result,oom);
        prepared.blockWrite.reset();released=true;
      } else if(elapsed.elapsed()>5000) {result.code="python_linux_cgroup_unavailable";break;}
    }
    if(released&&!writeClosed&&process.bytesToWrite()<128*1024) {
      while(chunkIndex<request.inputChunks.size()&&offset==request.inputChunks[chunkIndex].size()) {++chunkIndex;offset=0;}
      if(chunkIndex==request.inputChunks.size()) {process.closeWriteChannel();writeClosed=true;}
      else {
        const auto &chunk=request.inputChunks[chunkIndex];const auto count=qMin<qint64>(64*1024,chunk.size()-offset);
        const auto wrote=process.write(chunk.constData()+offset,count);if(wrote<0) {result.code="python_stdin_failed";break;}offset+=wrote;
      }
    }
    process.waitForReadyRead(20);
    captureStats(cpu,memory,memoryEvents,result,oom);
    if(result.cpuMicros>=qint64(request.policy.cpuSeconds)*1000000) {result.code="python_process_cpu_limit";break;}
  }
  if(process.state()!=QProcess::NotRunning) {process.kill();process.waitForFinished(3000);}
  prepared.blockWrite.reset();
  if(result.code.isEmpty())result.code=drain(process,result,request.policy);
  result.exitCode=process.exitStatus()==QProcess::NormalExit?process.exitCode():-1;
  captureStats(cpu,memory,memoryEvents,result,oom);
  if(result.code.isEmpty()) {
    if(!released)result.code="python_linux_cgroup_unavailable";
    else if(oom)result.code="python_process_memory_limit";
    else if(result.exitCode==0)result.code="ok";
    else if(result.cpuMicros>=qint64(request.policy.cpuSeconds)*1000000)result.code="python_process_cpu_limit";
    else if(elapsed.elapsed()>=qint64(request.policy.wallSeconds)*1000)result.code="python_process_timeout";
    else result.code="python_process_failed";
  }
  if(!result.ok()){result.outputLog=result.output.first(qMin<qint64>(result.output.size(),request.policy.logBytes));result.output.clear();}
  return result;
}
}
namespace paleo::python::process_detail {
ProcessResult runLinuxTrustedChild(const Launch &request) {
  linux_native::Prepared prepared;
  const auto invalid=linux_native::prepareTrusted(request,prepared);
  if(!invalid.isEmpty()) {ProcessResult result;result.code=invalid;return result;}
  return linux_native::runPreparedChild(request,prepared);
}
}
