// 层：测试壳
#pragma once
#include "services/pythonanalysis.h"
#include <QJsonArray>
namespace paleo::python::report_fixture {
inline QJsonObject metadata(AnalysisKind kind=AnalysisKind::WellQuality,int rows=4,int channels=2) {
  QJsonArray columns,sources;
  for(int index=0;index<channels;++index){const auto id=QString::number(index+1);
    columns.append(QJsonObject{{"curveId",id},{"name",index?"B":"A"},{"unit",index?"m/s":"API"},{"processed",true},
      {"nullPolicy","sdk_quality_bits_1_missing_2_abnormal_4_edited_and_nonfinite"}});
    sources.append(QJsonObject{{"source",QJsonObject{{"documentId","doc"},{"modelStoreId","area-model"},{"sourceId","7"},{"fileRevisionId",QString(64,'a')}}},
      {"sourceVersionId","source-v1"},{"wellId","1"},{"logSetId","2"},{"curveId",id},{"firstSample","0"},
      {"sampleCount",rows},{"sourceTotalSamples","8192"},{"processed",true}});}
  const QJsonObject axis{{"kind","MD"},{"unit","m"},{"reference","KB"},{"alignment","exact_shared_axis_v1"},{"conversion","none"},
    {"origins",QJsonObject{{"kind","declared"},{"unit","declared"},{"reference","declared"}}}};
  const QJsonObject sampling{{"method","all_selected_window_v1"},{"seed",0},{"count",rows},{"scope","selected_window_only"},{"random",false},{"extrapolation",false}};
  const QJsonObject provenance{{"documentId","doc"},{"callerScope","agent:test"},
    {"model",QJsonObject{{"storeId","model"},{"generation","generation"},{"sequence","1"},{"boundary","unchanged_before_and_after_all_windows"}}},
    {"sources",sources},{"axis",axis},{"sampling",sampling},{"readMetrics",QJsonObject{{"payloadBytes","104"}}}};
  return {{"contract","paleo.python-analysis-input.v1"},{"analysis",analysisKindId(kind)},
    {"inputProtocol","paleo.python-analysis-frame.v1"},{"sourceKind","well_curve_numeric_attributes"},{"dtype","<f8"},
    {"shape",QJsonArray{rows,channels}},{"columns",columns},{"axis",axis},{"provenance",provenance}};
}
}
