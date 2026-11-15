// 层：数据
#include "pythonnumerictablequery.h"
#include <QJsonArray>
#include <cmath>
namespace paleo::python {
QJsonObject numericTableProjectionSourceSchema() {
  const QJsonObject text{{"type","string"},{"minLength",1},{"maxLength",256}};
  return {{"type","object"},{"required",QJsonArray{"documentId","modelStoreId","sourceId","fileRevisionId"}},{"additionalProperties",false},
    {"properties",QJsonObject{{"documentId",text},{"modelStoreId",QJsonObject{{"const","area-model"}}},
      {"sourceId",QJsonObject{{"type","string"},{"pattern","^[1-9][0-9]{0,18}$"}}},
      {"fileRevisionId",QJsonObject{{"type","string"},{"pattern","^[0-9a-f]{64}$"}}}}}};
}
QString numericTableProjectionQueryArguments(const QJsonObject &arguments,document::SourceRef &reference,qlonglong &tableId) {
  reference={};tableId=0;
  for(auto it=arguments.begin();it!=arguments.end();++it)if(!QStringList{"topic","source_ref","table_id","max_bytes"}.contains(it.key()))return "unsupported_topic_argument";
  if(arguments.value("topic")!="numeric_table_projection"||!arguments.value("source_ref").isObject())return "invalid_python_table_query";
  const auto source=arguments.value("source_ref").toObject();document::SourceRef pending;
  if(!document::sourceRefFromJson(source,pending)||document::sourceRefJson(pending)!=source||pending.modelStoreId!="area-model"||pending.sourceId<1||
      pending.documentId.toUtf8().size()>256||pending.documentId.contains(QChar(0))||QString::fromUtf8(pending.documentId.toUtf8())!=pending.documentId||
      pending.fileRevisionId.size()!=64)return "python_table_source_mismatch";
  for(const auto c:pending.fileRevisionId)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return "python_table_source_mismatch";
  bool valid=false;const auto id=arguments.value("table_id");qlonglong parsed=0;
  if(!id.isString()||(parsed=id.toString().toLongLong(&valid))<1||!valid||QString::number(parsed)!=id.toString())return "python_table_id_required";
  if(arguments.contains("max_bytes")) {
    const auto budget=arguments.value("max_bytes");if(!budget.isDouble()||!std::isfinite(budget.toDouble())||
        budget.toDouble()!=std::floor(budget.toDouble())||budget.toDouble()<2048||budget.toDouble()>65536)return "query_budget_invalid";
  }
  reference=pending;tableId=parsed;return {};
}
}
