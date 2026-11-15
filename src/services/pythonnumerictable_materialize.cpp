// 层：数据
#include "pythonnumerictable_internal.h"
#include "pythongenerated.h"
#include <paleo_data/service.h>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

namespace paleo::python::numeric_table_detail {
QString digest(QByteArrayView bytes){return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
QString sdkFailure(const std::string &detail) {
  if(detail=="NumericTableCancelled")return "cancelled";
  if(detail=="NumericTableProjectionUnavailable")return "python_table_projection_unavailable";
  if(detail=="NumericTableSchemaUnavailable"||detail=="NumericTableUnsupportedSchema")return "python_table_schema_unavailable";
  if(detail=="NumericTableStaleReadCut"||detail=="NumericTableSourceChanged")return "python_table_revision_changed";
  if(detail=="NumericTableWindowOutsideProjection")return "python_table_window_out_of_range";
  if(detail.find("Budget")!=std::string::npos)return "python_table_read_budget";
  if(detail=="NumericTableCellTypeInvalid"||detail=="NumericTableMissingCell")return "python_table_value_invalid";
  return "python_table_read_failed";
}
QString validatePlan(const NumericTableReadPlan &plan,const QByteArray &code,const ExecutionPolicy &policy) {
  auto error=validateGeneratedCode(code,policy);if(!error.isEmpty())return error;
  const auto &selection=plan.selection;
  const auto publicInputs=numericTableSelectionJson(selection);
  NumericTableSelection checked;if(!numericTableSelectionFromJson(publicInputs,checked,&error))return error;
  if(plan.projectDir.isEmpty()||plan.modelDirectory.isEmpty()||plan.documentId.isEmpty()||plan.callerScope.isEmpty()||
      plan.documentId!=selection.source.documentId||plan.source.ref!=selection.source||
      plan.source.versionId.isEmpty()||plan.source.versionId.toUtf8().size()>256||plan.source.assetId.isEmpty()||
      plan.source.fingerprint.stored()!=selection.source.fileRevisionId||
      QFileInfo(plan.source.modelDirectory).canonicalFilePath()!=QFileInfo(plan.modelDirectory).canonicalFilePath()||
      QFileInfo(plan.modelDirectory).canonicalFilePath().isEmpty())return "python_table_source_mismatch";
  if(plan.readByteBudget<1||plan.readByteBudget>8*1024*1024||plan.readByteBudget>policy.inputBytes)return "python_table_read_budget";
  return {};
}
}
namespace paleo::python {
namespace {
bool sameHead(const data::ModelChangeHead &left,const data::ModelChangeHead &right) {
  return left.available&&right.available&&left.storeId==right.storeId&&left.generation==right.generation&&left.sequence==right.sequence;
}
}
NumericTableMaterialization materializeNumericTable(const NumericTableReadPlan &plan,const QByteArray &sourceCode,
    const ExecutionPolicy &policy,const std::shared_ptr<ExecutionPermit> &permit,const std::function<bool()> &cancelled) {
  NumericTableMaterialization out;const auto fail=[&](const QString &code){out.code=code;out.bundle={};out.metadata={};return out;};
  if(!permit)return fail("python_execution_permit_required");
  if(cancelled&&cancelled())return fail("cancelled");
  auto code=numeric_table_detail::validatePlan(plan,sourceCode,policy);if(!code.isEmpty())return fail(code);
  const auto &selected=plan.selection;const qint64 maximum=qint64(selected.rowCount)*(8+9*selected.columnIds.size());
  if(maximum>policy.inputBytes-65540||maximum>policy.diskBytes-sourceCode.size()-2*65536)return fail("python_input_budget_exceeded");
  // Two SDK vector capacities, two packed buffers, bounded query/decode scratch and JSON/code copies.
  const qint64 reserved=4*maximum+4*plan.readByteBudget+4*65536+2*sourceCode.size();
  if(reserved>policy.memoryBytes)return fail("python_input_memory_budget");
  data::DataService sdk;std::string detail;
  if(!sdk.openProjectReadOnly(plan.modelDirectory.toUtf8().toStdString(),detail))return fail("python_table_model_unavailable");
  data::SourceIndexEntry source;data::SourceIndexReadOptions sourceBudget;data::SourceIndexReadStats sourceStats;
  sourceBudget.maxFieldBytes=qMin<qint64>(8192,plan.readByteBudget);sourceBudget.maxTextBytes=sourceBudget.maxFieldBytes;
  const bool sourceOk=sdk.readSourceIndexEntry(selected.source.sourceId,source,detail,sourceBudget,&sourceStats);
  out.sourceMetadataBytes=sourceStats.textBytesRead;if(!sourceOk)return fail(numeric_table_detail::sdkFailure(detail));
  if(sourceStats.textBytesRead<0||sourceStats.textBytesRead>sourceBudget.maxTextBytes)return fail("python_table_read_budget");
  if(!source.active||source.id!=selected.source.sourceId||QString::fromStdString(source.sha256)!=selected.source.fileRevisionId)
    return fail("python_table_source_changed");
  data::NumericTableReadOptions budget;budget.cancelled=cancelled;budget.metadata.maxFieldBytes=8192;
  budget.metadata.maxTextBytes=qMin<qint64>(32768,plan.readByteBudget-sourceStats.textBytesRead);
  budget.maxReadBytes=plan.readByteBudget-sourceStats.textBytesRead;
  if(budget.maxReadBytes<1||budget.metadata.maxTextBytes<1)return fail("python_table_read_budget");
  data::NumericTableDescriptor descriptor;data::NumericTableReadStats described;
  const bool describedOk=sdk.describeNumericTableProjection(selected.tableId,descriptor,detail,budget,&described);
  out.io=described;if(!describedOk)return fail(numeric_table_detail::sdkFailure(detail));
  const auto &projection=descriptor.projection;
  if(described.metadataBytesRead<0||described.metadataBytesRead>budget.maxReadBytes||described.cellsRead||described.cellTextBytesRead)
    return fail("python_table_read_budget");
  if(projection.tableId!=selected.tableId||projection.sourceId!=selected.source.sourceId||
      QString::fromStdString(projection.sourceSha256)!=selected.source.fileRevisionId||
      QString::fromStdString(descriptor.revision)!=selected.projectionRevision)return fail("python_table_revision_changed");
  if(selected.firstRow>projection.rowCount||selected.rowCount>projection.rowCount-selected.firstRow)return fail("python_table_window_out_of_range");
  data::NumericTableWindowQuery query;query.expected=descriptor.cut;query.firstRow=selected.firstRow;query.rowCount=selected.rowCount;
  for(const auto &id:selected.columnIds)query.columnIds.push_back(id.toStdString());
  budget.maxReadBytes-=described.metadataBytesRead;
  budget.metadata.maxTextBytes=qMin<qint64>(32768,budget.maxReadBytes);
  if(budget.maxReadBytes<1)return fail("python_table_read_budget");
  data::NumericTableWindow window;data::NumericTableReadStats read;
  const bool readOk=sdk.readNumericTableWindow(query,window,detail,budget,&read);
  out.io=read;out.io.metadataBytesRead+=described.metadataBytesRead;if(!readOk)return fail(numeric_table_detail::sdkFailure(detail));
  if(read.metadataBytesRead<0||read.cellTextBytesRead<0||read.metadataBytesRead>budget.maxReadBytes-read.cellTextBytesRead||
      read.outputBytes<0||read.outputBytes>maximum||read.rowsReturned!=selected.rowCount||
      window.sourceRows.size()!=size_t(selected.rowCount)||window.columns.size()!=size_t(selected.columnIds.size())||window.firstRow!=selected.firstRow||
      numericTableReadCutJson(window.descriptor.cut)!=numericTableReadCutJson(descriptor.cut))return fail("python_table_read_budget");
  data::ModelChangeHead after;if(!sdk.readModelChangeHead(after,detail)||!sameHead(after,window.descriptor.cut.model))return fail("python_table_revision_changed");
  if(cancelled&&cancelled())return fail("cancelled");
  const auto bytes=numeric_table_detail::payload(window,code);if(!code.isEmpty())return fail(code);
  if(bytes.size()!=read.outputBytes)return fail("python_table_value_invalid");
  const QJsonObject metrics{{"metadataTextBytes",QString::number(sourceStats.textBytesRead+described.metadataBytesRead+read.metadataBytesRead)},
    {"cellTextBytes",QString::number(read.cellTextBytesRead)},{"cellsRead",QString::number(read.cellsRead)},
    {"rowsReturned",selected.rowCount},{"typedOutputBytes",QString::number(read.outputBytes)},
    {"sdkReservedBytes",QString::number(read.reservedBytes)},{"hostNumericPreparationReservedBytes",QString::number(reserved)},
    {"measurement","sdk_delivered_bytes_and_reservation_not_disk_io_or_process_rss"}};
  out.metadata=numeric_table_detail::metadata(plan,window,metrics,bytes);
  if(!validateNumericTableInputMetadata(out.metadata))return fail("python_table_input_metadata_invalid");
  const auto control=QJsonDocument(out.metadata).toJson(QJsonDocument::Compact);
  if(control.size()>65536||control.size()+qint64(bytes.size())+4>policy.inputBytes)return fail("python_input_budget_exceeded");
  const QJsonObject scope{{"documentId",plan.documentId},{"callerScope",plan.callerScope},{"inputProtocol","paleo.python-analysis-frame.v1"},
    {"inputContract",QLatin1String(NumericTableInputContract)},{"analysis","generated_numeric_table"},{"provenance",out.metadata.value("provenance")}};
  out.bundle=freezeInputBundle(plan.projectDir,{{"code.py",sourceCode},{"request.json",control},{"data.bin",bytes}},scope,policy,cancelled);
  if(!out.bundle.ok())return fail(out.bundle.code);out.code="ok";return out;
}
}
