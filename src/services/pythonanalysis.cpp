// 层：数据
#include "pythonanalysis_internal.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QtEndian>

namespace paleo::python {
QString analysisKindId(AnalysisKind kind) {
  switch(kind) {
    case AnalysisKind::WellQuality:return "well_quality";
    case AnalysisKind::AttributeCorrelation:return "attribute_correlation";
  }
  return {};
}
QString analysisTemplateSha256(AnalysisKind kind) {
  switch(kind) {
    case AnalysisKind::WellQuality:return QString::fromLatin1(PALEO_AGENT_PYTHON_WELL_QUALITY_SHA);
    case AnalysisKind::AttributeCorrelation:return QString::fromLatin1(PALEO_AGENT_PYTHON_ATTRIBUTE_CORRELATION_SHA);
  }
  return {};
}
QJsonObject AnalysisReadMetrics::json() const {
  return {{"payloadBytes",QString::number(payloadBytes)},{"legacyBytes",QString::number(legacyBytes)},
    {"axisBlocks",QString::number(axisBlocks)},{"valueBlocks",QString::number(valueBlocks)},
    {"windows",QString::number(windows)},{"measurement","sdk_sql_driver_payload_not_physical_disk"}};
}
namespace analysis_detail {
QString hash(QByteArrayView bytes) {return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
}
namespace {
bool declared(const QString &value) {
  return !value.trimmed().isEmpty() && value.toUtf8().size()<=256 && !value.contains(QChar(0)) &&
    !QStringList{"unknown","unspecified","undefined","?","n/a"}.contains(value.toCaseFolded());
}
QByteArray exactFile(const InputBundle &bundle,const QString &name,qint64 maximum,QString &error,
                    const std::function<bool()> &cancelled) {
  qint64 size=-1;QString digest;
  for(const auto &value:bundle.manifest.value("files").toArray()) {
    const auto file=value.toObject();if(file.value("path")!=name)continue;
    size=qint64(file.value("size").toDouble(-1));digest=file.value("sha256").toString();
  }
  QFile file(bundle.root+'/'+name);const QFileInfo info(file);
  if(size<0 || size>maximum || !info.isFile() || info.isSymLink() || !file.open(QIODevice::ReadOnly) || file.size()!=size) {
    error="python_input_changed";return {};
  }
  QByteArray bytes;bytes.reserve(qsizetype(size));
  while(bytes.size()<size) {
    if(cancelled && cancelled()){error="cancelled";return {};}
    const auto part=file.read(qMin<qint64>(65536,size-bytes.size()));
    if(part.isEmpty()){error="python_input_changed";return {};}bytes.append(part);
  }
  if(file.size()!=size || analysis_detail::hash(bytes)!=digest){error="python_input_changed";return {};}
  return bytes;
}
}
namespace analysis_detail {
QString validatePlan(const AnalysisReadPlan &plan,const ExecutionPolicy &policy) {
  if(!policy.validate().isEmpty())return "python_policy_invalid";
  if(!policy.allowTrusted)return "python_execution_denied";
  if(analysisKindId(plan.kind).isEmpty() || plan.documentId.isEmpty() || plan.callerScope.isEmpty() ||
      plan.projectDir.isEmpty() || plan.modelDirectory.isEmpty())return "python_analysis_scope_required";
  const auto count=plan.curves.size();
  if(count<1 || count>AnalysisMaxChannels || (plan.kind==AnalysisKind::AttributeCorrelation && count<2))return "python_analysis_channel_budget";
  if(plan.alignment!="exact_shared_axis_v1" || plan.sampling!="all_selected_window_v1" || plan.seed!=0)
    return "python_analysis_sampling_unsupported";
  if(!plan.expectedHead.available || plan.expectedHead.storeId.empty() || plan.expectedHead.generation.empty() || plan.expectedHead.sequence<0)
    return "python_analysis_snapshot_unavailable";
  if(plan.readByteBudget<1 || plan.readByteBudget>policy.inputBytes)return "python_analysis_read_budget";
  QSet<qlonglong> ids;const auto &first=plan.curves.first();
  for(const auto &curve:plan.curves) {
    if(curve.wellId<=0 || curve.logSetId<=0 || curve.curveId<=0 || ids.contains(curve.curveId) ||
        curve.firstSample<0 || curve.sampleCount<0 || curve.sampleCount>AnalysisMaxSamples ||
        curve.wellId!=first.wellId || curve.sampleCount!=first.sampleCount)return "python_analysis_selection_invalid";
    ids.insert(curve.curveId);
    if(!declared(curve.unit) || curve.depthType!="MD" || !QStringList{"m","ft"}.contains(curve.depthUnit) ||
        !declared(curve.depthReference))return "python_analysis_semantics_required";
    if(curve.depthType!=first.depthType || curve.depthUnit!=first.depthUnit || curve.depthReference!=first.depthReference)
      return "python_analysis_axis_mismatch";
    if(curve.sourceVersionId.isEmpty() || curve.source.versionId!=curve.sourceVersionId ||
        curve.source.ref.documentId!=plan.documentId || curve.source.ref.modelStoreId!="area-model" ||
        !curve.source.ref.valid() || curve.source.fingerprint.stored()!=curve.source.ref.fileRevisionId ||
        QFileInfo(curve.source.modelDirectory).canonicalFilePath()!=QFileInfo(plan.modelDirectory).canonicalFilePath())
      return "python_analysis_source_mismatch";
  }
  return {};
}
}
AnalysisProcessInput loadAnalysisProcessInput(const QString &project,const QJsonObject &reference,
    const QString &expectedDocumentId,const QString &expectedCallerScope,
    const ExecutionPolicy &policy,const std::shared_ptr<ExecutionPermit> &permit,const std::function<bool()> &cancelled) {
  AnalysisProcessInput out;const auto fail=[&](const QString &code){out.code=code;out.bytes.clear();return out;};
  if(!permit)return fail("python_execution_permit_required");
  const auto bundle=verifyInputBundle(project,reference,policy,cancelled);if(!bundle.ok())return fail(bundle.code);
  QStringList names;for(const auto &value:bundle.manifest.value("files").toArray())names.append(value.toObject().value("path").toString());
  names.sort();if(names!=QStringList{"code.py","data.bin","request.json"})return fail("python_analysis_contract_invalid");
  const auto scope=bundle.manifest.value("metadata").toObject();
  if(expectedDocumentId.isEmpty() || expectedCallerScope.isEmpty() || scope.value("documentId")!=expectedDocumentId ||
      scope.value("callerScope")!=expectedCallerScope || scope.value("inputProtocol")!="paleo.python-analysis-frame.v1")
    return fail("python_input_scope_mismatch");
  QString error;const auto control=exactFile(bundle,"request.json",64*1024,error,cancelled);if(!error.isEmpty())return fail(error);
  const auto payload=exactFile(bundle,"data.bin",policy.inputBytes,error,cancelled);if(!error.isEmpty())return fail(error);
  const auto document=QJsonDocument::fromJson(control);if(!document.isObject())return fail("python_analysis_contract_invalid");
  out.metadata=document.object();const auto declaredPayload=out.metadata.value("payload").toObject();
  const auto provenance=out.metadata.value("provenance").toObject();
  if(out.metadata.value("contract")!="paleo.python-analysis-input.v1" ||
      out.metadata.value("inputProtocol")!="paleo.python-analysis-frame.v1" ||
      out.metadata.value("analysis")!=scope.value("analysis") || provenance.value("documentId")!=expectedDocumentId ||
      provenance.value("callerScope")!=expectedCallerScope || scope.value("provenance")!=QJsonValue(provenance) ||
      declaredPayload.value("bytes").toDouble(-1)!=payload.size() || declaredPayload.value("sha256").toString()!=analysis_detail::hash(payload))
    return fail("python_analysis_contract_invalid");
  if(control.size()>policy.inputBytes-4 || payload.size()>policy.inputBytes-control.size()-4)return fail("python_input_budget_exceeded");
  out.bytes.resize(4);qToLittleEndian<quint32>(quint32(control.size()),out.bytes.data());out.bytes.append(control);out.bytes.append(payload);
  out.code="ok";return out;
}
} // namespace paleo::python
