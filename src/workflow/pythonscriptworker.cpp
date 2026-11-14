// 层：功能
#include "pythonscriptoperation.h"
#include "pythonoperationdiagnostics.h"
#include "services/pythoninputbundle.h"
#include "services/pythonprocess.h"
#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStringDecoder>
#include <cmath>

namespace paleo::python {
namespace {
QString report(const QByteArray &bytes,QByteArray &output) {
  QStringDecoder decoder(QStringDecoder::Utf8);const QString text=decoder(bytes);Q_UNUSED(text);
  if(decoder.hasError())return "python_output_encoding_invalid";
  const auto document=QJsonDocument::fromJson(bytes);if(!document.isObject())return "python_output_invalid";
  const auto envelope=document.object();const auto ports=envelope.value("ports").toObject();
  if(envelope.value("contract")!="paleo.python-output.v1" || ports.keys()!=QStringList{"REPORT"} || !ports.value("REPORT").isObject())return "python_output_contract_mismatch";
  const auto value=ports.value("REPORT").toObject();
  if(value.value("contract")!="paleo.python-summary.v1" || !value.value("count").isDouble() ||
      value.value("count").toDouble()<1 || value.value("count").toDouble()>8192 ||
      value.value("count").toDouble()!=std::floor(value.value("count").toDouble()))return "python_output_contract_mismatch";
  for(const auto &key:{"sum","minimum","maximum"})if(!value.value(QLatin1String(key)).isDouble() || !std::isfinite(value.value(QLatin1String(key)).toDouble()))return "python_output_contract_mismatch";
  output=QJsonDocument(value).toJson(QJsonDocument::Compact);return {};
}
}
capability::OperationWorker trustedScriptWorker(const QJsonObject &plan,std::shared_ptr<RuntimeProvider> provider) {
  return [plan,provider](const capability::OperationWorkContext &context) {
    capability::OperationWorkResult out;
    const auto permit=claimExecutionPermit(context.cancelled);if(!permit){out.code="cancelled";return out;}
    const auto runtime=provider->inspect(context.cancelled);
    if(!runtime.ready()){out.code=runtime.code;return out;}
    if(runtime.runtime->manifestSha256!=plan.value("runtimeManifestSha256").toString()){out.code="python_runtime_changed";return out;}
    const auto bundle=verifyInputBundle(context.projectDir,plan.value("storedInput").toObject(),provider->policy(),context.cancelled);
    if(!bundle.ok()){out.code=bundle.code;return out;}
    const auto metadata=bundle.manifest.value("metadata").toObject();
    if(metadata.value("documentId")!=plan.value("documentId") || metadata.value("callerScope")!=plan.value("callerScope") ||
        metadata.value("capability")!=plan.value("capability")){out.code="python_input_scope_mismatch";return out;}
    QFile input(bundle.root+"/request.json");
    if(!input.open(QIODevice::ReadOnly) || input.size()>provider->policy().inputBytes){out.code="python_input_changed";return out;}
    TrustedProcessRequest request;request.runtime=runtime.runtime;request.scriptPath=bundle.root+"/code.py";
    request.scriptSha256=plan.value("scriptSha256").toString();request.input=input.read(provider->policy().inputBytes+1);
    QString expectedInputHash;qint64 expectedInputSize=-1;
    for(const auto &value:bundle.manifest.value("files").toArray())if(value.toObject().value("path")=="request.json") {
      expectedInputHash=value.toObject().value("sha256").toString();expectedInputSize=qint64(value.toObject().value("size").toDouble(-1));
    }
    if(request.input.size()!=expectedInputSize || QString::fromLatin1(QCryptographicHash::hash(request.input,QCryptographicHash::Sha256).toHex())!=expectedInputHash) {
      out.code="python_input_changed";return out;
    }
    request.policy=provider->policy();request.cancelled=context.cancelled;request.permit=permit;
    const auto process=runTrustedProcess(request);appendPythonProcessDiagnostics(out,context,process,request.policy);
    if(!process.ok()){out.code=process.code;return out;}
    QByteArray content;out.code=report(process.output,content);if(!out.code.isEmpty())return out;
    const auto path=QDir(context.privateDir).filePath("REPORT.json");
    if(!storePythonOperationBytes(path,content)){out.code="python_output_write_failed";return out;}
    out.outputs.ok=true;out.outputs.artifacts.append({"REPORT",path,{},{},{{"encoding","utf8"}}, {}});
    out.summary={{"runtimeManifestSha256",runtime.runtime->manifestSha256},{"scriptSha256",request.scriptSha256},
      {"report",QJsonDocument::fromJson(content).object()},{"cpuMicros",QString::number(process.cpuMicros)},
      {"observedPeakMemoryBytes",QString::number(process.peakMemoryBytes)},{"memoryPeakComplete",process.memoryPeakComplete}};
    out.ok=true;out.code="ok";return out;
  };
}
} // namespace paleo::python
