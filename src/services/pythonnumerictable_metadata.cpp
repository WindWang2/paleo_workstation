// 层：数据
#include "pythonnumerictable_internal.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QtEndian>
#include <bit>
#include <cmath>
#include <limits>

namespace paleo::python {
QJsonObject numericTableReadCutJson(const data::NumericTableReadCut &cut) {
  return {{"contract",QLatin1String(data::NumericTableContract)},
    {"model",QJsonObject{{"storeId",QString::fromStdString(cut.model.storeId)},{"generation",QString::fromStdString(cut.model.generation)},
      {"sequence",QString::number(cut.model.sequence)},{"available",cut.model.available}}},
    {"tableId",QString::number(cut.tableId)},{"sourceId",QString::number(cut.sourceId)},
    {"sourceSha256",QString::fromStdString(cut.sourceSha256)},{"projectionRevision",QString::fromStdString(cut.projectionRevision)},
    {"boundary","observed_sqlite_read_snapshot_not_history_lease"}};
}
}
namespace paleo::python::numeric_table_detail {
QByteArray payload(const data::NumericTableWindow &window,QString &error) {
  error.clear();const auto rows=window.sourceRows.size();QByteArray bytes;bytes.reserve(qsizetype(rows)*(8+9*window.columns.size()));
  const auto fail=[&]{error="python_table_value_invalid";return QByteArray{};};
  for(size_t row=0;row<rows;++row) {
    if(window.sourceRows[row]!=window.descriptor.projection.firstSourceRow+window.firstRow+std::int64_t(row))return fail();
    char encoded[8];qToLittleEndian<quint64>(quint64(window.sourceRows[row]),encoded);bytes.append(encoded,8);
  }
  for(const auto &column:window.columns) {
    if(column.nullMask.size()!=rows)return fail();const auto type=column.column.dtype;
    if(type==data::NumericTableDtype::Float64) {
      if(column.numbers.size()!=rows||!column.booleans.empty())return fail();
      for(size_t row=0;row<rows;++row) {
        if(column.nullMask[row]>1||!std::isfinite(column.numbers[row])||(column.nullMask[row]&&column.numbers[row]!=0))return fail();
        char encoded[8];qToLittleEndian<quint64>(std::bit_cast<quint64>(column.numbers[row]),encoded);bytes.append(encoded,8);
      }
    } else if(type==data::NumericTableDtype::Boolean) {
      if(column.booleans.size()!=rows||!column.numbers.empty())return fail();
      for(size_t row=0;row<rows;++row) {
        if(column.nullMask[row]>1||column.booleans[row]>1||(column.nullMask[row]&&column.booleans[row]))return fail();
        bytes.append(char(column.booleans[row]));
      }
    } else return fail();
    for(auto mask:column.nullMask)bytes.append(char(mask));
  }
  return bytes;
}
QJsonObject metadata(const NumericTableReadPlan &plan,const data::NumericTableWindow &window,
                    const QJsonObject &metrics,const QByteArray &bytes) {
  const auto rows=plan.selection.rowCount;QJsonArray columns;qint64 offset=qint64(rows)*8;
  for(const auto &value:window.columns) {
    const auto &column=value.column;const auto width=column.dtype==data::NumericTableDtype::Float64?8:1;
    QJsonArray nulls;for(const auto &text:column.nullTexts)nulls.append(QString::fromStdString(text));
    columns.append(QJsonObject{{"id",QString::fromStdString(column.id)},{"label",QString::fromStdString(column.label)},
      {"unit",QString::fromStdString(column.unit)},{"sourceColumn",column.sourceColumn},
      {"dtype",width==8?"<f8":"u1"},{"nullTexts",nulls},{"missingIsNull",column.missingIsNull},
      {"valueOffset",offset},{"valueBytes",rows*width},{"nullOffset",offset+rows*width},{"nullBytes",rows}});
    offset+=qint64(rows)*(width+1);
  }
  const QJsonObject axis{{"id","source_row"},{"kind","physical_source_row"},{"unit","1"},{"dtype","<i8"},
    {"reference","one_based_physical_rows"},{"valueOffset",0},{"valueBytes",rows*8}};
  const QJsonObject selected{{"tableId",QString::number(plan.selection.tableId)},{"projectionRevision",plan.selection.projectionRevision},
    {"columnIds",QJsonArray::fromStringList(plan.selection.columnIds)},{"firstRow",QString::number(plan.selection.firstRow)},{"rowCount",rows}};
  const QJsonArray sources{QJsonObject{{"source",document::sourceRefJson(plan.source.ref)},
    {"sourceVersionId",plan.source.versionId},{"assetId",plan.source.assetId}}};
  const QJsonObject provenance{{"documentId",plan.documentId},{"callerScope",plan.callerScope},{"sources",sources},
    {"readCut",numericTableReadCutJson(window.descriptor.cut)},{"selection",selected},{"axis",axis},{"readMetrics",metrics},
    {"declaredRange",QJsonObject{{"firstSourceRow",QString::number(window.descriptor.projection.firstSourceRow)},
      {"rowCount",QString::number(window.descriptor.projection.rowCount)}}}};
  return {{"contract",QLatin1String(NumericTableInputContract)},{"inputProtocol","paleo.python-analysis-frame.v1"},
    {"analysis","generated_numeric_table"},{"sourceKind","source_table_numeric_projection"},
    {"shape",QJsonArray{rows,int(columns.size())}},{"columns",columns},{"axis",axis},
    {"layout",QJsonObject{{"order","axis_then_column_major_values_and_null_masks"},{"littleEndian",true},{"conversion","none"}}},
    {"payload",QJsonObject{{"bytes",bytes.size()},{"sha256",digest(bytes)}}},{"provenance",provenance}};
}
}
namespace paleo::python {
namespace {
bool keys(const QJsonObject &value,std::initializer_list<const char*> names) {
  if(value.size()!=qsizetype(names.size()))return false;for(const auto *name:names)if(!value.contains(QLatin1String(name)))return false;return true;
}
bool boundedText(const QJsonValue &value,int maximum,bool empty=false) {
  if(!value.isString())return false;const auto text=value.toString();const auto bytes=text.toUtf8();
  return (empty||!text.trimmed().isEmpty())&&bytes.size()<=maximum&&!text.contains(QChar(0))&&QString::fromUtf8(bytes)==text;
}
bool decimal(const QJsonValue &value,qlonglong &number,bool positive=false) {
  bool valid=false;if(!value.isString())return false;number=value.toString().toLongLong(&valid);
  return valid&&number>=(positive?1:0)&&QString::number(number)==value.toString();
}
bool hash(const QJsonValue &value) {
  if(!value.isString()||value.toString().size()!=64)return false;
  for(const auto c:value.toString())if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;return true;
}
bool dimension(const QJsonValue &value,int limit,int &out) {
  if(!value.isDouble()||!std::isfinite(value.toDouble())||value.toDouble()<0||value.toDouble()>limit||
      std::floor(value.toDouble())!=value.toDouble())return false;out=value.toInt();return true;
}
bool columns(const QJsonArray &fields,const NumericTableSelection &selection,int rows,qint64 &bytes) {
  if(fields.size()!=selection.columnIds.size())return false;bytes=qint64(rows)*8;QSet<int> physical;
  for(qsizetype index=0;index<fields.size();++index) {
    const auto field=fields[index].toObject();int sourceColumn=0;
    if(!fields[index].isObject()||!keys(field,{"id","label","unit","sourceColumn","dtype","nullTexts","missingIsNull","valueOffset","valueBytes","nullOffset","nullBytes"})||
        field.value("id")!=selection.columnIds[index]||!boundedText(field.value("label"),256,true)||!boundedText(field.value("unit"),128)||
        !dimension(field.value("sourceColumn"),std::numeric_limits<int>::max(),sourceColumn)||sourceColumn<1||physical.contains(sourceColumn)||
        !field.value("missingIsNull").isBool()||!field.value("nullTexts").isArray()||field.value("nullTexts").toArray().size()>8)return false;
    physical.insert(sourceColumn);const auto type=field.value("dtype").toString(),unit=field.value("unit").toString().trimmed().toCaseFolded();
    if((type!="<f8"&&type!="u1")||(type=="u1"&&field.value("unit")!="1")||
        QStringList{"unknown","unspecified","undefined","?","n/a"}.contains(unit))return false;
    QSet<QString> nulls;for(const auto &text:field.value("nullTexts").toArray()) {
      if(!boundedText(text,64,true)||nulls.contains(text.toString()))return false;nulls.insert(text.toString());
    }
    const auto width=type=="<f8"?8:1;
    if(field.value("valueOffset")!=bytes||field.value("valueBytes")!=rows*width||field.value("nullOffset")!=bytes+rows*width||field.value("nullBytes")!=rows)return false;
    bytes+=qint64(rows)*(width+1);
  }
  return true;
}
}
bool validateNumericTableInputMetadata(const QJsonObject &metadata) {
  if(QJsonDocument(metadata).toJson(QJsonDocument::Compact).size()>65536||
      !keys(metadata,{"contract","inputProtocol","analysis","sourceKind","shape","columns","axis","layout","payload","provenance"})||
      metadata.value("contract")!=QLatin1String(NumericTableInputContract)||metadata.value("inputProtocol")!="paleo.python-analysis-frame.v1"||
      metadata.value("analysis")!="generated_numeric_table"||metadata.value("sourceKind")!="source_table_numeric_projection")return false;
  const auto provenance=metadata.value("provenance").toObject();
  if(!keys(provenance,{"documentId","callerScope","sources","readCut","selection","axis","readMetrics","declaredRange"})||
      !boundedText(provenance.value("documentId"),256)||!boundedText(provenance.value("callerScope"),256)||
      !provenance.value("sources").isArray()||provenance.value("sources").toArray().size()!=1)return false;
  const auto source=provenance.value("sources").toArray()[0].toObject(),selection=provenance.value("selection").toObject();
  if(!keys(source,{"source","sourceVersionId","assetId"})||!boundedText(source.value("sourceVersionId"),256)||!boundedText(source.value("assetId"),256)||
      !keys(selection,{"tableId","projectionRevision","columnIds","firstRow","rowCount"}))return false;
  NumericTableSelection checked;const QJsonObject inputs{{"source_ref",source.value("source")},{"table_id",selection.value("tableId")},
    {"projection_revision",selection.value("projectionRevision")},{"column_ids",selection.value("columnIds")},
    {"first_row",selection.value("firstRow")},{"row_count",selection.value("rowCount")}};
  if(!numericTableSelectionFromJson(inputs,checked)||checked.source.documentId!=provenance.value("documentId"))return false;
  const auto cut=provenance.value("readCut").toObject(),model=cut.value("model").toObject();qlonglong sequence=0;
  if(!keys(cut,{"contract","model","tableId","sourceId","sourceSha256","projectionRevision","boundary"})||cut.value("contract")!=QLatin1String(data::NumericTableContract)||
      cut.value("boundary")!="observed_sqlite_read_snapshot_not_history_lease"||!keys(model,{"storeId","generation","sequence","available"})||
      !boundedText(model.value("storeId"),128)||!boundedText(model.value("generation"),128)||model.value("available")!=true||
      !decimal(model.value("sequence"),sequence)||cut.value("tableId")!=selection.value("tableId")||
      cut.value("sourceId")!=QString::number(checked.source.sourceId)||cut.value("sourceSha256")!=checked.source.fileRevisionId||
      cut.value("projectionRevision")!=checked.projectionRevision)return false;
  const auto range=provenance.value("declaredRange").toObject();qlonglong first=0,total=0;
  if(!keys(range,{"firstSourceRow","rowCount"})||!decimal(range.value("firstSourceRow"),first,true)||!decimal(range.value("rowCount"),total)||
      total>std::numeric_limits<qlonglong>::max()-first||checked.firstRow>total||checked.rowCount>total-checked.firstRow)return false;
  const auto shape=metadata.value("shape").toArray();int rows=0,count=0;
  if(!metadata.value("shape").isArray()||shape.size()!=2||!dimension(shape[0],data::NumericTableMaxRows,rows)||rows!=checked.rowCount||
      !dimension(shape[1],data::NumericTableMaxColumns,count)||count!=checked.columnIds.size()||!metadata.value("columns").isArray())return false;
  const QJsonObject axis{{"id","source_row"},{"kind","physical_source_row"},{"unit","1"},{"dtype","<i8"},
    {"reference","one_based_physical_rows"},{"valueOffset",0},{"valueBytes",rows*8}};
  const QJsonObject layout{{"order","axis_then_column_major_values_and_null_masks"},{"littleEndian",true},{"conversion","none"}};
  qint64 bytes=0;if(metadata.value("axis")!=QJsonValue(axis)||provenance.value("axis")!=QJsonValue(axis)||metadata.value("layout")!=QJsonValue(layout)||
      !columns(metadata.value("columns").toArray(),checked,rows,bytes))return false;
  const auto payload=metadata.value("payload").toObject();if(!keys(payload,{"bytes","sha256"})||payload.value("bytes")!=bytes||!hash(payload.value("sha256")))return false;
  const auto metrics=provenance.value("readMetrics").toObject();
  if(!keys(metrics,{"metadataTextBytes","cellTextBytes","cellsRead","rowsReturned","typedOutputBytes","sdkReservedBytes","hostNumericPreparationReservedBytes","measurement"})||
      metrics.value("rowsReturned")!=rows||metrics.value("typedOutputBytes")!=QString::number(bytes)||
      metrics.value("measurement")!="sdk_delivered_bytes_and_reservation_not_disk_io_or_process_rss")return false;
  qlonglong meta=0,cells=0,cellBytes=0,sdkReserve=0,hostReserve=0;
  return decimal(metrics.value("metadataTextBytes"),meta)&&decimal(metrics.value("cellTextBytes"),cellBytes)&&meta<=8*1024*1024-cellBytes&&
    decimal(metrics.value("cellsRead"),cells)&&cells<=qint64(rows)*count&&decimal(metrics.value("sdkReservedBytes"),sdkReserve)&&sdkReserve<=8*1024*1024&&
    decimal(metrics.value("hostNumericPreparationReservedBytes"),hostReserve,true)&&hostReserve<=512LL*1024*1024;
}
}
