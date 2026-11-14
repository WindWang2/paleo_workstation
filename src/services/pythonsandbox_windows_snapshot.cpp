// 层：数据
#include "pythonsandbox_windows_internal.h"
#ifdef Q_OS_WIN
#include "services/pythoninputbundle.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QtEndian>

namespace paleo::python::windows_detail {
namespace {
constexpr qint64 JsonBytes=64*1024;
QString projectRoot(const InputBundle &input) {
  const auto directory=input.reference.value("directory").toString();
  if(directory.isEmpty()||!input.root.endsWith('/'+directory))return {};
  const auto project=input.root.left(input.root.size()-directory.size()-1);
  return canonicalRoot(project)==project?project:QString();
}
QJsonObject row(const InputBundle &input,const QString &path) {
  for(const auto &value:input.manifest.value("files").toArray())if(value.toObject().value("path")==path)return value.toObject();
  return {};
}
bool readAuthenticated(const QString &root,const QJsonObject &entry,qint64 maximum,const SandboxRequest &request,QByteArray &out,QString *error) {
  const auto size=entry.value("size").toInteger(-1);const auto name=entry.value("path").toString();
  if(size<0||size>maximum||name.isEmpty()){*error="python_input_protocol_invalid";return false;}
  QFile file(QDir(root).filePath(name));if(!file.open(QIODevice::ReadOnly)||file.size()!=size){*error="python_input_changed";return false;}
  out.reserve(qsizetype(size));QCryptographicHash hash(QCryptographicHash::Sha256);
  while(out.size()<size){if(cancelled(request)){*error="cancelled";return false;}
    const auto bytes=file.read(qMin<qint64>(64*1024,size-out.size()));if(bytes.isEmpty()){*error="python_input_changed";return false;}
    hash.addData(bytes);out+=bytes;}
  if(file.size()!=size||!file.atEnd()||QString::fromLatin1(hash.result().toHex())!=entry.value("sha256").toString()){
    out.clear();*error="python_input_changed";return false;}
  return true;
}
bool transport(const SandboxRequest &request,const InputBundle &input,AuthenticatedSnapshot &snapshot,QString *error) {
  const auto protocol=input.manifest.value("metadata").toObject().value("inputProtocol").toString();
  const bool framed=protocol==QLatin1String(SandboxAnalysisInputProtocol);
  if(!framed&&protocol!=QLatin1String(SandboxJsonInputProtocol)){*error="python_input_protocol_invalid";return false;}
  QSet<QString> names;for(const auto &value:input.manifest.value("files").toArray())names.insert(value.toObject().value("path").toString());
  const QSet<QString> expected=framed?QSet<QString>{"code.py","request.json","data.bin"}:QSet<QString>{"code.py","request.json"};
  if(names!=expected){*error="python_input_protocol_invalid";return false;}
  const auto jsonRow=row(input,"request.json"),binaryRow=framed?row(input,"data.bin"):QJsonObject();
  const auto jsonSize=jsonRow.value("size").toInteger(-1),binarySize=framed?binaryRow.value("size").toInteger(-1):0;
  if(jsonSize<1||jsonSize>JsonBytes||binarySize<0||jsonSize>request.policy.inputBytes-binarySize){*error="python_input_budget_exceeded";return false;}
  snapshot.reservedBytes=hostBaseReservation(request.policy)+jsonSize+binarySize+8192;
  if(snapshot.reservedBytes>request.policy.memoryBytes){*error="python_snapshot_memory_budget";return false;}
  QByteArray json,binary;if(!readAuthenticated(input.root,jsonRow,JsonBytes,request,json,error))return false;
  QJsonParseError parse;const auto document=QJsonDocument::fromJson(json,&parse);
  if(parse.error!=QJsonParseError::NoError||!document.isObject()){*error="python_input_protocol_invalid";return false;}
  if(framed){if(!readAuthenticated(input.root,binaryRow,request.policy.inputBytes-jsonSize,request,binary,error))return false;
    QByteArray header(4,Qt::Uninitialized);qToLittleEndian<quint32>(quint32(json.size()),header.data());snapshot.inputChunks={header,json,binary};}
  else snapshot.inputChunks={json};
  snapshot.program=request.runtime->interpreter;snapshot.codePath=QDir(input.root).filePath("code.py");return true;
}
} // namespace
qint64 hostBaseReservation(const ExecutionPolicy &policy) {
  // A separate host budget: bounded manifest/path/handle metadata, plus the
  // worst reallocating stdout/stderr buffers. Runtime/input payloads stay locked
  // files and are not copied here; only authenticated stdin payloads are copied.
  return 64LL*1024*1024+3*(policy.outputBytes+policy.logBytes);
}
bool authenticateSnapshots(const SandboxRequest &request,const LockedTree &runtime,const LockedTree &input,
                           AuthenticatedSnapshot &snapshot,QString *error) {
  snapshot.reservedBytes=hostBaseReservation(request.policy);
  const RuntimeConfig configuration{runtime.root,request.runtime->manifestSha256,true};
  const auto verified=verifyRuntime(configuration,request.cancelled,request.runtime->retention);
  if(!verified.ready()){*error=verified.code;return false;}
  if(verified.runtime->runtimeVersion!=request.runtime->runtimeVersion||verified.runtime->platform!=request.runtime->platform){*error="python_runtime_changed";return false;}
  const auto project=projectRoot(*request.input);if(project.isEmpty()){*error="python_input_path_unsafe";return false;}
  const auto authenticated=verifyInputBundle(project,request.input->reference,request.policy,request.cancelled);
  if(!authenticated.ok()){*error=authenticated.code;return false;}
  if(authenticated.root!=input.root||authenticated.manifest!=request.input->manifest){*error="python_input_changed";return false;}
  return transport(request,authenticated,snapshot,error);
}
} // namespace paleo::python::windows_detail
#endif
