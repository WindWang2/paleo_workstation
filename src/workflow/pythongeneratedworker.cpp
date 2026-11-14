// 层：功能
#include "pythongeneratedoperation_internal.h"
#include "pythonoperationdiagnostics.h"
#include "services/pythongenerated.h"
#include <QDir>
#include <QJsonDocument>

namespace paleo::python::generated_operation {
capability::OperationWorker worker(const QJsonObject &plan,std::shared_ptr<RuntimeProvider> provider,std::shared_ptr<SandboxProvider> sandbox) {
  return [plan,provider,sandbox](const capability::OperationWorkContext &context) {
    capability::OperationWorkResult out;const auto fail=[&](const QString &code){out.code=code;return out;};
    auto code=checkPlan(plan,provider,sandbox,true);if(!code.isEmpty())return fail(code);
    if(context.frozenPlan!=plan)return fail("invalid_python_generated_plan");
    const auto permit=claimExecutionPermit(context.cancelled);if(!permit)return fail("cancelled");
    const auto runtime=provider->inspect(context.cancelled);if(!runtime.ready())return fail(runtime.code);
    if(runtime.runtime->manifestSha256!=plan.value("runtimeManifestSha256").toString())return fail("python_runtime_changed");
    const auto bundle=verifyInputBundle(context.projectDir,plan.value("storedInput").toObject(),provider->policy(),context.cancelled);
    if(!bundle.ok())return fail(bundle.code);code=checkBundle(bundle,plan);if(!code.isEmpty())return fail(code);
    SandboxRequest request;request.runtime=runtime.runtime;request.input=std::make_shared<InputBundle>(bundle);
    request.policy=provider->policy();request.cancelled=context.cancelled;request.permit=permit;
    const auto process=sandbox->run(request);
    ProcessResult diagnostics;diagnostics.code=process.code;diagnostics.output=process.output;diagnostics.error=process.error;
    diagnostics.cpuMicros=process.cpuMicros;diagnostics.peakMemoryBytes=process.peakMemoryBytes;
    diagnostics.memoryPeakComplete=process.memoryPeakComplete;diagnostics.exitCode=process.exitCode;diagnostics.outputLog=process.outputLog;
    appendPythonProcessDiagnostics(out,context,diagnostics,request.policy);
    if(!process.ok())return fail(process.code);
    const auto report=validateGeneratedReport(process.output,plan.value("inputMetadata").toObject(),
      plan.value("codeSha256").toString(),runtime.runtime->manifestSha256,request.policy);if(!report.ok())return fail(report.code);
    const auto path=QDir(context.privateDir).filePath("REPORT.json");
    if(!storePythonOperationBytes(path,report.bytes))return fail("python_output_write_failed");
    out.outputs.ok=true;out.outputs.artifacts.append({"REPORT",path,{},{},{{"encoding","utf8"}}, {}});
    out.summary=report.summary;out.summary["runtimeManifestSha256"]=runtime.runtime->manifestSha256;out.summary["backend"]=plan.value("backend");
    out.summary["cpuMicros"]=QString::number(process.cpuMicros);out.summary["observedPeakMemoryBytes"]=QString::number(process.peakMemoryBytes);
    out.summary["memoryPeakComplete"]=process.memoryPeakComplete;out.summary["hostSnapshotReservedBytes"]=QString::number(process.hostSnapshotReservedBytes);
    out.summary["childMemoryLimitBytes"]=QString::number(process.childMemoryLimitBytes);
    if(QJsonDocument(out.summary).toJson(QJsonDocument::Compact).size()>4096)return fail("python_output_budget_exceeded");
    out.ok=true;out.code="ok";return out;
  };
}
}
