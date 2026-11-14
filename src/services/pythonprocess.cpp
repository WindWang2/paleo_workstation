// 层：数据
#include "pythonprocess.h"
#include "pythonprocess_internal.h"
#ifdef Q_OS_WIN
#include "pythonprocess_windows.h"
#endif
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringDecoder>
#include <QSemaphore>
#include <QtEndian>

namespace paleo::python {
namespace process_detail {
namespace {QSemaphore &executionSlot(){static QSemaphore slot(1);return slot;}}
ProcessPermit::~ProcessPermit(){if(m_owns)executionSlot().release();}
std::shared_ptr<ProcessPermit> acquireProcessPermit(const std::function<bool()> &cancelled) {
  if(cancelled&&cancelled())return {};
  auto permit=std::shared_ptr<ProcessPermit>(new ProcessPermit);
  while(!executionSlot().tryAcquire(1,25))if(cancelled&&cancelled())return {};
  permit->m_owns=true;
  if(cancelled&&cancelled())return {};
  return permit;
}
} // namespace process_detail
std::shared_ptr<ExecutionPermit> claimExecutionPermit(const std::function<bool()> &cancelled) {
  return process_detail::acquireProcessPermit(cancelled);
}
namespace {
bool sha256(const QString &value) {
  if(value.size()!=64)return false;
  for(const auto c:value)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
  return true;
}
QString scriptBytes(const TrustedProcessRequest &request,QByteArray &bytes,QString &directory) {
  if(!sha256(request.scriptSha256))return "python_script_identity_required";
  const QFileInfo info(request.scriptPath);
  if(!info.isAbsolute()||!info.isFile()||info.isSymLink()||info.size()<1||info.size()>request.policy.codeBytes)
    return "python_script_invalid";
#ifdef Q_OS_WIN
  if(info.isJunction())return "python_script_invalid";
#endif
  const auto canonical=info.canonicalFilePath();
  if(canonical.isEmpty()||canonical!=QDir::cleanPath(QDir::fromNativeSeparators(info.absoluteFilePath())))
    return "python_script_invalid";
  QFile file(canonical);
  if(!file.open(QIODevice::ReadOnly))return "python_script_unavailable";
  bytes=file.read(info.size()+1);
  if(bytes.size()!=info.size()||!file.atEnd()||file.size()!=info.size())return "python_script_changed";
  if(QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())!=request.scriptSha256)
    return "python_script_changed";
  QStringDecoder decoder(QStringDecoder::Utf8);const QString decoded=decoder(bytes);Q_UNUSED(decoded);
  if(decoder.hasError())return "python_script_encoding_invalid";
  directory=info.canonicalPath();return {};
}
} // namespace

ProcessResult runTrustedProcess(const TrustedProcessRequest &request) {
  const auto fail=[](const QString &code){ProcessResult result;result.code=code;return result;};
  if(request.cancelled&&request.cancelled())return fail("cancelled");
  if(!request.policy.validate().isEmpty())return fail("python_policy_invalid");
  if(!request.policy.allowTrusted)return fail("python_execution_denied");
  if(!request.runtime||request.runtime->root.isEmpty()||!QFileInfo(request.runtime->interpreter).isAbsolute())
    return fail("python_environment_missing");
  if(request.input.size()>request.policy.inputBytes)return fail("python_input_budget_exceeded");
  const auto permit=request.permit?request.permit:claimExecutionPermit(request.cancelled);
  if(!permit)return fail("cancelled");
  QByteArray code;QString directory;
  const auto invalid=scriptBytes(request,code,directory);
  if(!invalid.isEmpty())return fail(invalid);
  if(request.cancelled&&request.cancelled())return fail("cancelled");
  process_detail::Launch launch;launch.program=request.runtime->interpreter;launch.workingDirectory=directory;
  launch.policy=request.policy;launch.cancelled=request.cancelled;launch.permit=permit;
  // Execute the authenticated bytes, not a path reopened after hashing. The fixed bootstrap
  // consumes a length-delimited trusted source prefix; the template reads the remaining stdin.
  const QString bootstrap="import sys,struct\n"
    "n=struct.unpack('<I',sys.stdin.buffer.read(4))[0]\n"
    "source=sys.stdin.buffer.read(n)\n"
    "if len(source)!=n: raise RuntimeError('incomplete trusted source')\n"
    "exec(compile(source,'<paleo-trusted-template>','exec'),{'__name__':'__main__','__file__':'<paleo-trusted-template>'})\n";
  launch.arguments={"-I","-u","-B","-X","utf8","-c",bootstrap};
  QByteArray header(4,Qt::Uninitialized);qToLittleEndian<quint32>(quint32(code.size()),header.data());
  launch.inputChunks={header,code,request.input};
  launch.environment={"PATH="+QFileInfo(launch.program).absolutePath(),"OPENBLAS_NUM_THREADS=1",
    "OMP_NUM_THREADS=1","MKL_NUM_THREADS=1","NUMEXPR_NUM_THREADS=1"};
#ifdef Q_OS_WIN
  const auto systemRoot=qEnvironmentVariable("SystemRoot");
  if(!systemRoot.isEmpty())launch.environment.append("SystemRoot="+systemRoot);
  auto result=windows_process::runChild(launch);
#elif defined(Q_OS_LINUX)
  auto result=process_detail::runLinuxTrustedChild(launch);
#else
  auto result=fail("python_process_unavailable");
#endif
  if(!result.ok() && !result.output.isEmpty()) {
    if(result.outputLog.isEmpty())result.outputLog=result.output.first(qMin<qint64>(result.output.size(),request.policy.logBytes));
    result.output.clear();
  }
  return result;
}
} // namespace paleo::python
