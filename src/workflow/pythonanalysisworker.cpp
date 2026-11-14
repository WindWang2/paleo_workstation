// 层：功能
#include "pythonanalysisoperation_internal.h"
#include "pythonoperationdiagnostics.h"
#include "services/pythonanalysisreport.h"
#include <QDir>
#include <QJsonDocument>

namespace paleo::python::analysis_operation {
capability::OperationWorker worker(const QJsonObject &plan,AnalysisKind kind,std::shared_ptr<RuntimeProvider> provider) {
  return [plan,kind,provider](const capability::OperationWorkContext &context) {
    capability::OperationWorkResult out;
    const auto fail=[&](const QString &code){out.code=code;return out;};
    auto code=checkPlan(plan,kind,provider);if(!code.isEmpty())return fail(code);
    if(context.frozenPlan!=plan)return fail("invalid_python_analysis_plan");
    const auto permit=claimExecutionPermit(context.cancelled);if(!permit)return fail("cancelled");
    const auto runtime=provider->inspect(context.cancelled);if(!runtime.ready())return fail(runtime.code);
    if(runtime.runtime->manifestSha256!=plan.value("runtimeManifestSha256").toString())return fail("python_runtime_changed");
    const auto input=loadAnalysisProcessInput(context.projectDir,plan.value("storedInput").toObject(),
      plan.value("documentId").toString(),plan.value("callerScope").toString(),provider->policy(),permit,context.cancelled);
    if(!input.ok())return fail(input.code);
    if(input.metadata!=plan.value("inputMetadata").toObject())return fail("python_analysis_input_metadata_changed");
    const auto bundle=verifyInputBundle(context.projectDir,plan.value("storedInput").toObject(),provider->policy(),context.cancelled);
    if(!bundle.ok())return fail(bundle.code);
    TrustedProcessRequest request;request.runtime=runtime.runtime;request.scriptPath=bundle.root+"/code.py";
    request.scriptSha256=analysisTemplateSha256(kind);request.input=input.bytes;request.policy=provider->policy();
    request.cancelled=context.cancelled;request.permit=permit;
    const auto process=runTrustedProcess(request);appendPythonProcessDiagnostics(out,context,process,request.policy);
    if(!process.ok())return fail(process.code);
    const auto report=validateAnalysisReport(process.output,kind,input.metadata,request.policy);if(!report.ok())return fail(report.code);
    const auto path=QDir(context.privateDir).filePath("REPORT.json");
    if(!storePythonOperationBytes(path,report.bytes))return fail("python_output_write_failed");
    out.outputs.ok=true;out.outputs.artifacts.append({"REPORT",path,{},{},{{"encoding","utf8"}}, {}});
    out.summary=report.summary;out.summary.insert("runtimeManifestSha256",runtime.runtime->manifestSha256);
    out.summary.insert("scriptSha256",request.scriptSha256);out.summary.insert("cpuMicros",QString::number(process.cpuMicros));
    out.summary.insert("observedPeakMemoryBytes",QString::number(process.peakMemoryBytes));out.summary.insert("memoryPeakComplete",process.memoryPeakComplete);
    if(QJsonDocument(out.summary).toJson(QJsonDocument::Compact).size()>4096)return fail("python_output_budget_exceeded");
    out.ok=true;out.code="ok";return out;
  };
}
} // namespace paleo::python::analysis_operation
