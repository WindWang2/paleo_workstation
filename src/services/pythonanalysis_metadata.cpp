// 层：数据
#include "pythonanalysis_internal.h"
#include <QJsonArray>
#include <QtEndian>
#include <bit>

namespace paleo::python::analysis_detail {
namespace {
void number(QByteArray &bytes,double value) {
  char raw[8];qToLittleEndian<quint64>(std::bit_cast<quint64>(value),raw);bytes.append(raw,8);
}
QJsonObject head(const data::ModelChangeHead &value) {
  return {{"storeId",QString::fromStdString(value.storeId)},{"generation",QString::fromStdString(value.generation)},
    {"sequence",QString::number(value.sequence)},{"boundary","unchanged_before_and_after_all_windows"}};
}
}
QByteArray payload(const QVector<data::CurveWindowResult> &columns) {
  QByteArray bytes;const auto rows=columns.first().curve.values.size();bytes.reserve(qsizetype(rows)*(8+9*columns.size()));
  for(double value:columns.first().curve.depths)number(bytes,value);
  for(size_t row=0;row<rows;++row)for(const auto &column:columns)number(bytes,column.curve.values[row]);
  for(size_t row=0;row<rows;++row)for(const auto &column:columns)bytes.append(char(column.curve.quality[row]));
  return bytes;
}
QJsonObject metadata(const AnalysisReadPlan &plan,const QVector<data::CurveWindowResult> &columns,
                     const AnalysisReadMetrics &metrics,const QByteArray &bytes) {
  const auto rows=plan.curves.first().sampleCount;const auto count=columns.size();QJsonArray fields,sources;
  for(qsizetype i=0;i<columns.size();++i) {
    const auto &selected=plan.curves[i];const auto &window=columns[i];
    fields.append(QJsonObject{{"curveId",QString::number(selected.curveId)},{"name",QString::fromStdString(window.curve.name)},
      {"unit",selected.unit},{"processed",selected.processed},{"nullPolicy","sdk_quality_bits_1_missing_2_abnormal_4_edited_and_nonfinite"}});
    sources.append(QJsonObject{{"source",document::sourceRefJson(selected.source.ref)},{"sourceVersionId",selected.sourceVersionId},
      {"wellId",QString::number(selected.wellId)},{"logSetId",QString::number(selected.logSetId)},{"curveId",QString::number(selected.curveId)},
      {"firstSample",QString::number(selected.firstSample)},{"sampleCount",selected.sampleCount},
      {"sourceTotalSamples",QString::number(window.totalSamples)},{"processed",selected.processed}});
  }
  const auto &first=plan.curves.first();
  const QJsonObject axis{{"kind",first.depthType},{"unit",first.depthUnit},{"reference",first.depthReference},
    {"alignment",plan.alignment},{"conversion","none"}};
  const QJsonObject sampling{{"method",plan.sampling},{"seed",int(plan.seed)},{"count",rows},
    {"scope","selected_window_only"},{"random",false},{"extrapolation",false}};
  const QJsonObject provenance{{"documentId",plan.documentId},{"callerScope",plan.callerScope},{"model",head(plan.expectedHead)},
    {"sources",sources},{"axis",axis},{"sampling",sampling},{"readMetrics",metrics.json()}};
  return {{"contract","paleo.python-analysis-input.v1"},{"analysis",analysisKindId(plan.kind)},
    {"inputProtocol","paleo.python-analysis-frame.v1"},{"sourceKind","well_curve_numeric_attributes"},
    {"dtype","<f8"},{"shape",QJsonArray{rows,int(count)}},{"columns",fields},{"axis",axis},
    {"layout",QJsonObject{{"axisBytes",rows*8},{"valueBytes",rows*int(count)*8},{"qualityBytes",rows*int(count)},
      {"order","axis_then_row_major_values_then_row_major_quality"}}},
    {"payload",QJsonObject{{"bytes",bytes.size()},{"sha256",hash(bytes)}}},{"provenance",provenance}};
}
} // namespace paleo::python::analysis_detail
