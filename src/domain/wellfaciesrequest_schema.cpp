// 层：数据
#include "wellfaciesrequest.h"
#include <QJsonArray>

namespace paleo::wellfacies {
namespace {
QJsonObject object(const QJsonObject &properties,const QJsonArray &required) {
  return {{"type","object"},{"properties",properties},{"required",required},{"additionalProperties",false}};
}
QJsonObject text(bool empty=false) {return {{"type","string"},{"minLength",empty?0:1},{"maxLength",256}};}
}
QJsonObject requestJsonSchema() {
  const QJsonObject decimal{{"type","string"},{"pattern","^[1-9][0-9]*$"},{"maxLength",19}};
  const QJsonObject well=object({{"contract_version",QJsonObject{{"const",1}}},{"document_id",text()},
    {"catalog_entity_id",text()},{"model_store_id",QJsonObject{{"const","area-model"}}},
    {"model_store_identity",text()},{"model_generation",text()},{"model_well_id",decimal},
    {"model_head",QJsonObject{{"type","string"},{"pattern","^(0|[1-9][0-9]*)$"},{"maxLength",19}}},
    {"model_available",QJsonObject{{"const",true}}}},
    {"contract_version","document_id","catalog_entity_id","model_store_id","model_store_identity", "model_generation",
     "model_well_id","model_head","model_available"});
  const auto context=object({{"documentId",text()},{"modelStoreId",QJsonObject{{"const","area-model"}}},
    {"purpose",QJsonObject{{"const","facies-analysis"}}},{"schemeId",text()},{"schemeRevision",text()},
    {"intervalId",text()},{"subIntervalId",text(true)},{"aggregate",QJsonObject{{"type","boolean"}}}},
    {"documentId","modelStoreId","purpose","schemeId","schemeRevision","intervalId","subIntervalId","aggregate"});
  const auto scope=object({{"schemeId",text()},{"revision",text()},{"intervalId",text()},{"subIntervalId",text(true)},
    {"aggregate",QJsonObject{{"type","boolean"}}},{"subIndex",QJsonObject{{"type","integer"},{"minimum",0},{"maximum",4}}},
    {"occurrenceKeys",QJsonObject{{"type","array"},{"maxItems",32},{"uniqueItems",true},{"items",text()}}}},
    {"schemeId","revision","intervalId","subIntervalId","aggregate","subIndex"});
  return object({{"contract_version",QJsonObject{{"const",2}}},{"client_request_id",text()},
    {"endpoint_id",QJsonObject{{"type","string"},{"pattern","^[0-9a-f]{64}$"}}},
    {"model",text()},{"model_version",text()},{"well_ref",well},{"context",context},{"unit_scope",scope},
    {"curve_ids",QJsonObject{{"type","object"},{"maxProperties",64},{"additionalProperties",decimal}}},
    {"lithology_track_id",decimal}},
    {"contract_version","client_request_id","endpoint_id","model","model_version","well_ref","context","unit_scope"});
}
} // namespace paleo::wellfacies
