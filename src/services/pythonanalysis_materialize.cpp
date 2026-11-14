// 层：数据
#include "pythonanalysis_internal.h"
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>

namespace paleo::python::analysis_detail {
QString checkHead(const data::DataService &service,const data::ModelChangeHead &expected) {
  data::ModelChangeHead live;std::string error;
  if(!service.readModelChangeHead(live,error) || !live.available)return "python_analysis_snapshot_unavailable";
  if(live.storeId!=expected.storeId || live.generation!=expected.generation || live.sequence!=expected.sequence)
    return "python_analysis_model_changed";
  return {};
}
QString checkSource(const data::DataService &service,const AnalysisCurveSelection &selection,const AnalysisReadPlan &plan,
                    AnalysisReadMetrics &metrics) {
  const auto &source=selection.source;data::SourceIndexEntry registered;std::string error;
  const auto remaining=plan.readByteBudget-metrics.payloadBytes-metrics.metadataTextBytes;
  if(remaining<1)return "python_analysis_read_budget";
  data::SourceIndexReadOptions options;options.maxFieldBytes=qMin<qint64>(8192,remaining);options.maxTextBytes=qMin<qint64>(8192,remaining);
  data::SourceIndexReadStats usage;
  if(!service.readSourceIndexEntry(source.ref.sourceId,registered,error,options,&usage)) {
    if(error.starts_with("SourceIndexValueBudgetExceeded"))return "python_analysis_read_budget";
    if(error.starts_with("SourceIndexValueBudgetUnavailable"))return "python_analysis_source_metadata_unavailable";
    return "python_analysis_source_changed";
  }
  if(usage.textBytesRead<0 || usage.textBytesRead>remaining)return "python_analysis_read_budget";
  metrics.metadataTextBytes+=usage.textBytesRead;++metrics.sourceRows;
  if(source.ref.documentId!=plan.documentId || source.versionId!=selection.sourceVersionId ||
      !registered.active ||
      QString::fromStdString(registered.sha256)!=source.ref.fileRevisionId)return "python_analysis_source_changed";
  return {};
}
namespace {
void charge(AnalysisReadMetrics &total,const data::CurveReadStats &part) {
  total.payloadBytes+=part.payloadBytesRead;total.legacyBytes+=part.legacyBytesMaterialized;
  total.metadataTextBytes+=part.metadataBytesRead;
  total.axisBlocks+=part.axisBlocksRead;total.valueBlocks+=part.valueBlocksRead;++total.windows;
}
QString axis(const data::CurveWindowResult &window,const AnalysisCurveSelection &selection,
             const QVector<data::CurveWindowResult> &previous) {
  const auto &curve=window.curve;
  for(const auto origin:{window.depthOrigins.depthType,window.depthOrigins.depthUnit,window.depthOrigins.mdReference}) {
    if(origin==data::CurveDepthOrigin::Unknown)return "python_analysis_semantics_required";
    if(origin!=data::CurveDepthOrigin::Declared)return "python_analysis_semantics_inferred";
  }
  if(window.firstSample!=selection.firstSample || window.totalSamples<selection.firstSample ||
      selection.sampleCount>window.totalSamples-selection.firstSample || curve.curveId!=selection.curveId ||
      curve.depths.size()!=size_t(selection.sampleCount) || curve.values.size()!=curve.depths.size() ||
      curve.quality.size()!=curve.depths.size())return "python_analysis_window_out_of_range";
  if(QString::fromStdString(curve.unit)!=selection.unit || QString::fromStdString(window.depthInfo.depthType)!=selection.depthType ||
      QString::fromStdString(window.depthInfo.depthUnit)!=selection.depthUnit ||
      QString::fromStdString(window.depthInfo.mdReference)!=selection.depthReference)return "python_analysis_semantics_changed";
  if(curve.name.empty() || curve.name.size()>256)return "python_analysis_semantics_required";
  for(size_t i=0;i<curve.depths.size();++i) {
    if(!std::isfinite(curve.depths[i]) || (i && curve.depths[i]<=curve.depths[i-1]))return "python_analysis_axis_invalid";
    if(curve.quality[i]&~7U)return "python_analysis_quality_unknown";
  }
  if(!previous.isEmpty() && previous.first().curve.depths!=curve.depths)return "python_analysis_axis_mismatch";
  return {};
}
}
QString readColumns(data::DataService &service,const AnalysisReadPlan &plan,QVector<data::CurveWindowResult> &columns,
                    AnalysisReadMetrics &metrics,const std::function<bool()> &cancelled) {
  columns.clear();std::string error;QHash<qlonglong,QString> sourceRevisions;
  for(const auto &selection:plan.curves) {
    if(cancelled && cancelled())return "cancelled";
    // An exact keyset seek proves ownership without enumerating the well's other curves.
    // Reserve the SDK's maximum returned name/detail bytes before its metadata query.
    if(plan.readByteBudget-metrics.payloadBytes-metrics.metadataTextBytes<8192)return "python_analysis_read_budget";
    std::vector<data::DataEntry> entries;bool more=false;
    data::MetadataReadOptions metadataBudget;metadataBudget.maxFieldBytes=4096;
    metadataBudget.maxTextBytes=qMin<qint64>(8192,plan.readByteBudget-metrics.payloadBytes-metrics.metadataTextBytes);
    data::MetadataReadStats metadataUsage;
    if(!service.listDataPage("curves",selection.wellId,selection.curveId-1,1,entries,more,error,metadataBudget,&metadataUsage)) {
      if(error.starts_with("DataPageValueBudgetExceeded"))return "python_analysis_read_budget";
      return "python_analysis_metadata_unavailable";
    }
    metrics.metadataRows+=entries.size();metrics.metadataTextBytes+=metadataUsage.textBytesRead;
    const auto sourceId=selection.source.ref.sourceId;
    if(sourceRevisions.contains(sourceId)) {
      if(sourceRevisions.value(sourceId)!=selection.source.ref.fileRevisionId)return "python_analysis_source_changed";
    } else {
      const auto sourceCode=checkSource(service,selection,plan,metrics);if(!sourceCode.isEmpty())return sourceCode;
      sourceRevisions.insert(sourceId,selection.source.ref.fileRevisionId);
    }
    if(entries.size()!=1 || entries.front().id!=selection.curveId || entries.front().wellId!=selection.wellId ||
        entries.front().parentId!=selection.logSetId || entries.front().sourceId!=selection.source.ref.sourceId)
      return "python_analysis_curve_mismatch";
    data::CurveReadOptions budget;budget.maxSamples=selection.sampleCount;budget.maxLegacyBytes=0;
    budget.maxReadBytes=plan.readByteBudget-metrics.payloadBytes-metrics.metadataTextBytes;budget.cancelled=cancelled;
    if(budget.maxReadBytes<1)return "python_analysis_read_budget";
    budget.maxMetadataBytes=qMin<qint64>(65536,budget.maxReadBytes);budget.maxMetadataFieldBytes=8192;
    data::CurveWindowQuery query;query.curveId=selection.curveId;query.firstSample=selection.firstSample;
    query.sampleCount=selection.sampleCount;query.processed=selection.processed;query.includeAuxiliaryAxes=false;
    data::CurveWindowResult window;data::CurveReadStats usage;
    const bool ok=service.readCurveWindow(query,window,error,budget,&usage);charge(metrics,usage);
    if(!ok) {
      if(cancelled && cancelled())return "cancelled";
      if(error.starts_with("CurveWindowMigrationRequired:"))return "python_analysis_chunk_storage_required";
      if(error.starts_with("CurveWindowBudgetExceeded:"))return "python_analysis_read_budget";
      return "python_analysis_read_failed";
    }
    if(metrics.payloadBytes>plan.readByteBudget-metrics.metadataTextBytes || metrics.legacyBytes!=0)return "python_analysis_read_budget";
    const auto invalid=axis(window,selection,columns);if(!invalid.isEmpty())return invalid;
    columns.append(std::move(window));
  }
  return checkHead(service,plan.expectedHead);
}
} // namespace paleo::python::analysis_detail

namespace paleo::python {
AnalysisMaterialization materializeAnalysis(const AnalysisReadPlan &plan,const QByteArray &trustedTemplate,
    const ExecutionPolicy &policy,const std::shared_ptr<ExecutionPermit> &permit,const std::function<bool()> &cancelled) {
  AnalysisMaterialization out;const auto fail=[&](const QString &code){out.code=code;out.bundle={};out.metadata={};return out;};
  if(!permit)return fail("python_execution_permit_required");
  if(cancelled && cancelled())return fail("cancelled");
  auto code=analysis_detail::validatePlan(plan,policy);if(!code.isEmpty())return fail(code);
  if(trustedTemplate.isEmpty() || trustedTemplate.size()>policy.codeBytes ||
      analysis_detail::hash(trustedTemplate)!=analysisTemplateSha256(plan.kind))return fail("python_template_changed");
  // The accepted shape bounds both SDK allocations and the complete framed transport.
  const qint64 maximumPayload=qint64(plan.curves.first().sampleCount)*(8+9*plan.curves.size());
  if(maximumPayload>policy.inputBytes-65540 || maximumPayload>policy.diskBytes-trustedTemplate.size()-2*65536)
    return fail("python_input_budget_exceeded");
  data::DataService service;std::string detail;
  if(!service.openProjectReadOnly(plan.modelDirectory.toUtf8().toStdString(),detail))return fail("python_analysis_model_unavailable");
  code=analysis_detail::checkHead(service,plan.expectedHead);if(!code.isEmpty())return fail(code);
  QVector<data::CurveWindowResult> columns;
  code=analysis_detail::readColumns(service,plan,columns,out.io,cancelled);if(!code.isEmpty())return fail(code);
  if(cancelled && cancelled())return fail("cancelled");
  const auto bytes=analysis_detail::payload(columns);out.metadata=analysis_detail::metadata(plan,columns,out.io,bytes);
  const auto control=QJsonDocument(out.metadata).toJson(QJsonDocument::Compact);
  if(control.size()>64*1024 || qint64(control.size())+bytes.size()+4>policy.inputBytes)return fail("python_input_budget_exceeded");
  const QJsonObject scope{{"documentId",plan.documentId},{"callerScope",plan.callerScope},
    {"inputProtocol","paleo.python-analysis-frame.v1"},{"inputContract","paleo.python-analysis-input.v1"},
    {"analysis",analysisKindId(plan.kind)},{"provenance",out.metadata.value("provenance")}};
  out.bundle=freezeInputBundle(plan.projectDir,{{"code.py",trustedTemplate},{"request.json",control},{"data.bin",bytes}},scope,policy,cancelled);
  if(!out.bundle.ok())return fail(out.bundle.code);
  out.code="ok";return out;
}
} // namespace paleo::python
