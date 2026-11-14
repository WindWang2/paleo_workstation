// 层：功能
#include "pythonanalysistools.h"
#include "domaintools.h"
#include "services/pythonanalysisselection.h"
#include "services/pythonanalysiscapabilityspec.h"
#include <QObject>

namespace {
paleo::python::AnalysisKind kind(const QString &name) {
  return name==QLatin1String(paleo::python::WellQualityTool)?paleo::python::AnalysisKind::WellQuality:
    paleo::python::AnalysisKind::AttributeCorrelation;
}
}
bool isPythonAnalysisTool(const QString &name) {
  return name==QLatin1String(paleo::python::WellQualityTool) || name==QLatin1String(paleo::python::CorrelationTool);
}
QJsonObject pythonAnalysisToolSchema(const QString &name) {return paleo::python::analysisToolRequestSchema(kind(name));}
QString validatePythonAnalysisArguments(const QString &name,const QJsonObject &args) {
  return paleo::python::validateAnalysisToolArguments(args,kind(name));
}
void appendPythonAnalysisTools(QVector<AiToolSpec> &specs) {
  AiToolSpec quality;quality.name=QLatin1String(paleo::python::WellQualityTool);
  quality.title=QObject::tr("测井曲线质量报告");
  quality.description=QObject::tr("先用 query_project 的 well_curves 取得精确 input_ref，再显式选择同井曲线、样点窗、处理状态和 MD 基准。可信 Python 模板只分析所选窗口，报告缺失、异常、覆盖率与统计量；最多16列、8192点。prepare 冻结输入，compute-only 保存私有报告；publish 只能引用原操作并经宿主授权登记成果。不修改原始数据。");
  AiToolSpec correlation;correlation.name=QLatin1String(paleo::python::CorrelationTool);
  correlation.title=QObject::tr("所选数值属性相关性");
  correlation.description=QObject::tr("分析 well_curves 精确引用的同井数值属性窗口：要求至少2列、完全一致的显式 MD 轴，保留逐列单位，不做插值或隐式换算。可信 Python 模板报告 Pearson 相关、共同有效点数及冗余对；结论仅适用于所选窗口，不代表全体积地震属性优选或融合。prepare、compute-only 和引用原操作的 publish 复用现有成果链路。");
  for(auto spec:{quality,correlation}) {
    spec.effects=paleo::host::Read|paleo::host::PrivateStage|paleo::host::LocalExecute;
    spec.resultSchemaJson=QStringLiteral(R"({"type":"object","properties":{"code":{"type":"string"},"operationId":{"type":"string"},"execution":{"type":"string"},"commitState":{"type":"string"},"summary":{"type":"object"}}})");
    specs.append(std::move(spec));
  }
}
