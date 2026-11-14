// 层：数据
#pragma once
#include "pythonanalysis.h"
#include <paleo_data/service.h>

namespace paleo::python::analysis_detail {
QString validatePlan(const AnalysisReadPlan &plan,const ExecutionPolicy &policy);
QString checkHead(const data::DataService &service,const data::ModelChangeHead &expected);
QString checkSource(const data::DataService &service,const AnalysisCurveSelection &selection,
                    const AnalysisReadPlan &plan);
QString readColumns(data::DataService &service,const AnalysisReadPlan &plan,
    QVector<data::CurveWindowResult> &columns,AnalysisReadMetrics &metrics,
    const std::function<bool()> &cancelled);
QJsonObject metadata(const AnalysisReadPlan &plan,const QVector<data::CurveWindowResult> &columns,
                     const AnalysisReadMetrics &metrics,const QByteArray &payload);
QByteArray payload(const QVector<data::CurveWindowResult> &columns);
QString hash(QByteArrayView bytes);
} // namespace paleo::python::analysis_detail
