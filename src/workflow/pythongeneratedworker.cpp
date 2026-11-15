// 层：功能
#include "pythongeneratedoperation_internal.h"
#include "pythongeneratedworker.h"
#include "pythonoperationdiagnostics.h"
#include "services/pythongenerated.h"
#include <QDir>
#include <QJsonDocument>

namespace paleo::python {
capability::OperationWorker generatedReportWorker(const QJsonObject &plan,std::shared_ptr<RuntimeProvider> provider,
    std::shared_ptr<SandboxProvider> sandbox,GeneratedWorkerChecks checks) {
  return [plan,provider,sandbox,checks](const capability::OperationWorkContext &context) {
    capability::OperationWorkResult out;const auto fail=[&](const QString &code){out.code=code;return out;};
    if(!checks.plan||!checks.bundle||(!checks.report&&!checks.outputs))return fail("python_contract_unavailable");
    auto code=checks.plan(plan,provider,sandbox,true);if(!code.isEmpty())return fail(code);
    if(context.frozenPlan!=plan)return fail(checks.invalidPlanCode);
    const auto permit=claimExecutionPermit(context.cancelled);if(!permit)return fail("cancelled");
    const auto runtime=provider->inspectIntegrity(context.cancelled);if(!runtime.ready())return fail(runtime.code);
    if(runtime.runtime->manifestSha256!=plan.value("runtimeManifestSha256").toString())return fail("python_runtime_changed");
    const auto bundle=verifyInputBundle(context.projectDir,plan.value("storedInput").toObject(),provider->policy(),context.cancelled);
    if(!bundle.ok())return fail(bundle.code);code=checks.bundle(bundle,plan);if(!code.isEmpty())return fail(code);
    SandboxRequest request;request.runtime=runtime.runtime;request.input=std::make_shared<InputBundle>(bundle);
    request.policy=provider->policy();request.cancelled=context.cancelled;request.permit=permit;
    request.stdoutLimitBytes=checks.stdoutLimitBytes>0?qMin(checks.stdoutLimitBytes,request.policy.outputBytes):checks.stdoutLimitBytes;
    const auto process=sandbox->run(request);
    ProcessResult diagnostics;diagnostics.code=process.code;diagnostics.output=process.output;diagnostics.error=process.error;
    diagnostics.cpuMicros=process.cpuMicros;diagnostics.peakMemoryBytes=process.peakMemoryBytes;
    diagnostics.memoryPeakComplete=process.memoryPeakComplete;diagnostics.exitCode=process.exitCode;diagnostics.outputLog=process.outputLog;
    appendPythonProcessDiagnostics(out,context,diagnostics,request.policy);
    if(!process.ok())return fail(process.code);
    GeneratedWorkerOutputs outputs;
    if(checks.outputs) outputs=checks.outputs(process.output,plan,request.policy,context.cancelled);
    else {
      const auto report=checks.report(process.output,plan.value("inputMetadata").toObject(),
        plan.value("codeSha256").toString(),runtime.runtime->manifestSha256,request.policy);
      outputs.code=report.code;outputs.summary=report.summary;
      if(report.ok())outputs.artifacts.append({"REPORT","REPORT.json",report.bytes,{{"encoding","utf8"}}});
    }
    if(outputs.code!="ok")return fail(outputs.code);
    if(outputs.artifacts.isEmpty())return fail("python_output_contract_mismatch");
    for(const auto &artifact:outputs.artifacts) {
      if(context.cancelled&&context.cancelled())return fail("cancelled");
      // File names are fixed by the C++ codec, never supplied by child output.
      if(artifact.fileName.isEmpty()||artifact.fileName.contains('/')||artifact.fileName.contains('\\')||
          artifact.fileName=="."||artifact.fileName=="..")return fail("python_output_contract_mismatch");
      const auto path=QDir(context.privateDir).filePath(artifact.fileName);
      if(!storePythonOperationBytes(path,artifact.bytes))return fail("python_output_write_failed");
      out.outputs.artifacts.append({artifact.port,path,{},{},artifact.metadata,{}});
    }
    out.outputs.ok=true;out.summary=outputs.summary;
    out.summary["runtimeManifestSha256"]=runtime.runtime->manifestSha256;out.summary["backend"]=plan.value("backend");
    out.summary["cpuMicros"]=QString::number(process.cpuMicros);out.summary["observedPeakMemoryBytes"]=QString::number(process.peakMemoryBytes);
    out.summary["memoryPeakComplete"]=process.memoryPeakComplete;out.summary["hostSnapshotReservedBytes"]=QString::number(process.hostSnapshotReservedBytes);
    out.summary["childMemoryLimitBytes"]=QString::number(process.childMemoryLimitBytes);
    if(QJsonDocument(out.summary).toJson(QJsonDocument::Compact).size()>4096)return fail("python_output_budget_exceeded");
    out.ok=true;out.code="ok";return out;
  };
}
}
namespace paleo::python::generated_operation {
capability::OperationWorker worker(const QJsonObject &plan,std::shared_ptr<RuntimeProvider> provider,std::shared_ptr<SandboxProvider> sandbox) {
  return generatedReportWorker(plan,std::move(provider),std::move(sandbox),
    {checkPlan,checkBundle,validateGeneratedReport,"invalid_python_generated_plan"});
}
}
