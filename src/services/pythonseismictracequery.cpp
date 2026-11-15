// 层：数据
#include "pythonseismictracequery.h"
#include "pythonseismictrace_internal.h"
#include <cmath>
namespace paleo::python {
QString seismicTraceDescriptorQueryArguments(const QJsonObject &arguments,document::SourceRef &reference,qlonglong &volumeId) {
  using namespace seismic_trace_detail;reference={};volumeId=0;
  for(auto it=arguments.begin();it!=arguments.end();++it)if(!QStringList{"topic","source_ref","volume_id","max_bytes"}.contains(it.key()))return "unsupported_topic_argument";
  if(arguments.value("topic")!="seismic_trace_descriptor"||!arguments.value("source_ref").isObject())return "invalid_python_seismic_query";
  const auto source=arguments.value("source_ref").toObject();document::SourceRef pending;
  if(!document::sourceRefFromJson(source,pending)||document::sourceRefJson(pending)!=source||pending.modelStoreId!="area-model"||pending.sourceId<1||
      !text(pending.documentId,256)||!digest(pending.fileRevisionId))return "python_seismic_source_mismatch";
  qlonglong id=0;if(!decimal(arguments.value("volume_id"),id,true))return "python_seismic_volume_required";
  if(arguments.contains("max_bytes")) {
    const auto budget=arguments.value("max_bytes");if(!budget.isDouble()||!std::isfinite(budget.toDouble())||
        budget.toDouble()!=std::floor(budget.toDouble())||budget.toDouble()<2048||budget.toDouble()>65536)return "query_budget_invalid";
  }
  reference=pending;volumeId=id;return {};
}
}
