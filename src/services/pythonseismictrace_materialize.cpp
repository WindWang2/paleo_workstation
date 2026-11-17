// 层：数据
#include "pythonseismictrace_internal.h"
#include "pythongenerated.h"
#include <paleo_data/service.h>
#include <QFileInfo>
#include <QJsonDocument>
namespace paleo::python::seismic_trace_detail {
QString sdkFailure(const std::string &detail) {
  if(detail=="SeismicWindowCancelled")return "cancelled";
  if(detail=="SeismicWindowStaleReadCut")return "python_seismic_revision_changed";
  if(detail=="SeismicWindowSourceMismatch"||detail=="SeismicWindowSourceUnavailable")return "python_seismic_source_changed";
  if(detail=="SeismicWindowInvalidTraceIndex")return "python_seismic_trace_out_of_range";
  if(detail=="SeismicWindowOutsideTrace")return "python_seismic_window_out_of_range";
  if(detail.find("Budget")!=std::string::npos)return "python_seismic_read_budget";
  if(detail=="SeismicWindowAxisUndeclared"||detail=="SeismicWindowTraceAxisMismatch"||detail=="SeismicWindowAxisOverflow")return "python_seismic_axis_mismatch";
  if(detail=="SeismicWindowUnsupportedOrChangedLayout")return "python_seismic_layout_unavailable";
  if(detail=="SeismicWindowReadCutUnavailable")return "python_seismic_read_cut_unavailable";
  return "python_seismic_read_failed";
}
}
namespace paleo::python {
namespace {
using Reader=artifacts::AuthenticatedArtifactReader;
bool sameHead(const data::ModelChangeHead &a,const data::ModelChangeHead &b) {
  return a.available&&b.available&&a.storeId==b.storeId&&a.generation==b.generation&&a.sequence==b.sequence;
}
bool semantics(const SeismicTraceSelection &s,const data::SeismicWindowDescriptor &d) {
  return d.cut.volumeId==s.volumeId&&d.cut.sourceId==s.source.sourceId&&QString::fromStdString(d.cut.volumeRevision)==s.volumeRevision&&
    QString::fromStdString(d.amplitudeUnit)==s.amplitudeUnit&&QString::fromStdString(d.timeUnit)==s.timeUnit&&
    QString::fromStdString(d.timeType)==s.timeType&&QString::fromStdString(d.timeReference)==s.timeReference;
}
QString validatePlan(const SeismicTraceReadPlan &plan,const QByteArray &code,const ExecutionPolicy &policy) {
  auto error=validateGeneratedCode(code,policy);if(!error.isEmpty())return error;
  SeismicTraceSelection checked;if(!seismicTraceSelectionFromJson(seismicTraceSelectionJson(plan.selection),checked,&error))return error;
  const auto &source=plan.authenticatedSource;
  if(!plan.reader||plan.projectDir.isEmpty()||plan.documentId!=checked.source.documentId||!seismic_trace_detail::text(plan.callerScope,256)||
      plan.source.ref!=checked.source||plan.source.versionId.isEmpty()||plan.source.assetId.isEmpty()||
      !seismic_trace_detail::text(plan.source.versionId,256)||!seismic_trace_detail::text(plan.source.assetId,256)||
      plan.source.fingerprint.kind!=document::FingerprintKind::Sha256||plan.source.fingerprint.stored()!=checked.source.fileRevisionId||
      source.sha256!=checked.source.fileRevisionId||source.path!=plan.source.path||source.scopeKey.isEmpty()||
      source.byteSize<3844||source.byteSize>Reader::MaxSeismicSourceBytes||
      QFileInfo(plan.source.modelDirectory).canonicalFilePath()!=QFileInfo(plan.modelDirectory).canonicalFilePath()||
      QFileInfo(plan.modelDirectory).canonicalFilePath().isEmpty())return "python_seismic_source_mismatch";
  if(plan.readByteBudget<1||plan.readByteBudget>8*1024*1024||plan.readByteBudget>policy.inputBytes)return "python_seismic_read_budget";
  return {};
}
Reader::Statistics difference(const Reader::Statistics &after,const Reader::Statistics &before) {
  return {after.fullScans-before.fullScans,after.fullHashBytes-before.fullHashBytes,after.chunkHashBytes-before.chunkHashBytes,
    after.readBytes-before.readBytes,after.cachedEntries,after.cachedDigestBytes};
}
}
SeismicTraceMaterialization materializeSeismicTrace(const SeismicTraceReadPlan &plan,const QByteArray &code,
    const ExecutionPolicy &policy,const std::shared_ptr<ExecutionPermit> &permit,const std::function<bool()> &cancelled) {
  SeismicTraceMaterialization out;const auto fail=[&](const QString &error){out.code=error;out.bundle={};out.metadata={};return out;};
  if(!permit)return fail("python_execution_permit_required");if(cancelled&&cancelled())return fail("cancelled");
  auto error=validatePlan(plan,code,policy);if(!error.isEmpty())return fail(error);
  const qint64 packed=qint64(plan.selection.sampleCount)*17;
  if(packed>policy.inputBytes-65540||packed>policy.diskBytes-code.size()-2*65536)return fail("python_input_budget_exceeded");
  // Bounded typed vectors/copies, SDK scratch, control/code, cached digest index
  // and two one-MiB authentication chunks. This is reserved host data, not RSS.
  const qint64 reserved=4*packed+4*plan.readByteBudget+4*65536+2*code.size()+6*1024*1024;
  if(reserved>policy.memoryBytes)return fail("python_input_memory_budget");
  data::DataService sdk;std::string detail;
  if(!sdk.openProjectReadOnly(plan.modelDirectory.toUtf8().toStdString(),detail))return fail("python_seismic_model_unavailable");
  data::SeismicWindowReadOptions options;options.cancelled=cancelled;options.maxReadBytes=plan.readByteBudget;
  options.metadata.maxFieldBytes=4096;options.metadata.maxTextBytes=qMin<qint64>(8192,plan.readByteBudget);
  data::SeismicWindowDescriptor descriptor;data::SeismicWindowReadStats described;
  if(!sdk.describeSeismicWindow(plan.selection.volumeId,descriptor,detail,options,&described)) {
    out.io=described;return fail(seismic_trace_detail::sdkFailure(detail));
  }
  out.io=described;
  if(!semantics(plan.selection,descriptor))return fail("python_seismic_revision_or_units_changed");
  if(described.metadataBytesRead<0||described.metadataBytesRead>=options.maxReadBytes||described.requestedSourceBytes||described.outputBytes)
    return fail("python_seismic_read_budget");
  if(plan.selection.firstSample>=descriptor.totalSamples||plan.selection.sampleCount>descriptor.totalSamples-plan.selection.firstSample)
    return fail("python_seismic_window_out_of_range");
  const auto maximumTraceCount=(plan.authenticatedSource.byteSize-3600)/(240+qint64(descriptor.totalSamples)*4);
  if(plan.selection.traceIndex>=maximumTraceCount)return fail("python_seismic_trace_out_of_range");
  options.maxReadBytes-=described.metadataBytesRead;options.metadata.maxTextBytes=qMin<qint64>(8192,options.maxReadBytes);
  const auto initial=plan.reader->statistics();QString authenticationError;
  data::SeismicAuthenticatedSource source;source.sha256=plan.authenticatedSource.sha256.toStdString();source.byteSize=plan.authenticatedSource.byteSize;
  source.read=[&](std::int64_t offset,int count,std::vector<std::uint8_t> &bytes,std::string &why,const auto &stopped) {
    bytes.clear();if(count<1||count>65536){authenticationError="python_seismic_read_budget";why=authenticationError.toStdString();return false;}
    const auto page=plan.reader->read(plan.authenticatedSource,offset,count,stopped);
    if(page.code!="ok"||page.bytes.size()!=count) {
      authenticationError=page.code=="cancelled"?"cancelled":page.code=="artifact_changed"?"python_seismic_source_changed":"python_seismic_authenticated_read_failed";
      why=authenticationError.toStdString();return false;
    }
    bytes.assign(page.bytes.begin(),page.bytes.end());return true;
  };
  data::SeismicWindowQuery query;query.expected=descriptor.cut;query.sourceSha256=source.sha256;query.traceIndex=plan.selection.traceIndex;
  query.firstSample=plan.selection.firstSample;query.sampleCount=plan.selection.sampleCount;
  data::SeismicWindow window;data::SeismicWindowReadStats read;
  const bool ok=sdk.readAuthenticatedSeismicWindow(query,source,window,detail,options,&read);
  out.io=read;out.io.metadataBytesRead+=described.metadataBytesRead;out.authentication=difference(plan.reader->statistics(),initial);
  if(!ok)return fail(authenticationError.isEmpty()?seismic_trace_detail::sdkFailure(detail):authenticationError);
  if(read.metadataBytesRead<0||read.requestedSourceBytes<0||read.metadataBytesRead>options.maxReadBytes-read.requestedSourceBytes||
      read.outputBytes!=packed||read.reservedBytes<0||read.reservedBytes>options.maxWorkingBytes||!semantics(plan.selection,window.descriptor)||
      seismicTraceReadCutJson(window.descriptor.cut)!=seismicTraceReadCutJson(descriptor.cut)||window.sourceSha256!=source.sha256||
      window.traceIndex!=query.traceIndex||window.firstSample!=query.firstSample)return fail("python_seismic_read_budget");
  data::ModelChangeHead last;if(!sdk.readModelChangeHead(last,detail)||!sameHead(last,window.descriptor.cut.model))return fail("python_seismic_revision_changed");
  if(cancelled&&cancelled())return fail("cancelled");
  const auto bytes=seismic_trace_detail::payload(window,error);if(!error.isEmpty()||bytes.size()!=packed)return fail("python_seismic_value_invalid");
  const auto &stats=out.authentication;
  const QJsonObject metrics{{"metadataTextBytes",QString::number(out.io.metadataBytesRead)},
    {"sdkRequestedSourceBytes",QString::number(read.requestedSourceBytes)},{"sdkSourceReadCalls",QString::number(read.sourceReadCalls)},
    {"typedOutputBytes",QString::number(packed)},{"sdkReservedBytes",QString::number(read.reservedBytes)},
    {"hostNumericPreparationReservedBytes",QString::number(reserved)},{"authenticatedFullScans",QString::number(stats.fullScans)},
    {"authenticatedFullHashBytes",QString::number(stats.fullHashBytes)},{"authenticatedChunkHashBytes",QString::number(stats.chunkHashBytes)},
    {"authenticatedReadBytes",QString::number(stats.readBytes)},{"cachedDigestBytes",QString::number(stats.cachedDigestBytes)},
    {"measurement","sdk_requested_ranges_and_actual_authentication_io_not_process_rss"}};
  out.metadata=seismic_trace_detail::metadata(plan,window,metrics,bytes);
  if(!validateSeismicTraceInputMetadata(out.metadata))return fail("python_seismic_input_metadata_invalid");
  const auto control=QJsonDocument(out.metadata).toJson(QJsonDocument::Compact);
  if(control.size()>65536||control.size()+qint64(bytes.size())+4>policy.inputBytes)return fail("python_input_budget_exceeded");
  const QJsonObject scope{{"documentId",plan.documentId},{"callerScope",plan.callerScope},{"inputProtocol","paleo.python-analysis-frame.v1"},
    {"inputContract",QLatin1String(SeismicTraceInputContract)},{"analysis","generated_seismic_trace"},{"provenance",out.metadata.value("provenance")}};
  out.bundle=freezeInputBundle(plan.projectDir,{{"code.py",code},{"request.json",control},{"data.bin",bytes}},scope,policy,cancelled);
  if(!out.bundle.ok())return fail(out.bundle.code);out.code="ok";return out;
}
}
