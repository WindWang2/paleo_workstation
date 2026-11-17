// 层：功能
#include "pythonanalysistools.h"
#include "domaintools.h"
#include "services/pythonanalysisselection.h"
#include "services/pythonanalysiscapabilityspec.h"
#include "services/pythongenerated.h"
#include "services/pythonnumerictable.h"
#include "services/pythonseismictrace.h"
#include "services/pythontableoutputspec.h"
#include <QObject>

namespace {
paleo::python::AnalysisKind kind(const QString &name) {
  return name==QLatin1String(paleo::python::WellQualityTool)?paleo::python::AnalysisKind::WellQuality:
    paleo::python::AnalysisKind::AttributeCorrelation;
}
}
bool isPythonAnalysisTool(const QString &name) {
  return name==QLatin1String(paleo::python::WellQualityTool) || name==QLatin1String(paleo::python::CorrelationTool) ||
    name==QLatin1String(paleo::python::GeneratedTool) || name==QLatin1String(paleo::python::NumericTableTool) ||
    name==QLatin1String(paleo::python::SeismicTraceTool) || name==QLatin1String(paleo::python::TableOutputsTool);
}
QJsonObject pythonAnalysisToolSchema(const QString &name) {
  if(name==QLatin1String(paleo::python::SeismicTraceTool))return paleo::python::seismicTraceToolRequestSchema();
  if(name==QLatin1String(paleo::python::TableOutputsTool))return paleo::python::tableOutputsToolRequestSchema();
  if(name==QLatin1String(paleo::python::NumericTableTool))return paleo::python::numericTableToolRequestSchema();
  return name==QLatin1String(paleo::python::GeneratedTool)?paleo::python::generatedToolRequestSchema():paleo::python::analysisToolRequestSchema(kind(name));
}
QString validatePythonAnalysisArguments(const QString &name,const QJsonObject &args) {
  if(name==QLatin1String(paleo::python::SeismicTraceTool))return paleo::python::validateSeismicTraceToolArguments(args);
  if(name==QLatin1String(paleo::python::TableOutputsTool))return paleo::python::validateTableOutputsToolArguments(args);
  if(name==QLatin1String(paleo::python::NumericTableTool))return paleo::python::validateNumericTableToolArguments(args);
  if(name==QLatin1String(paleo::python::GeneratedTool))return paleo::python::validateGeneratedToolArguments(args);
  return paleo::python::validateAnalysisToolArguments(args,kind(name));
}
void appendPythonAnalysisTools(QVector<AiToolSpec> &specs) {
  AiToolSpec quality;quality.name=QLatin1String(paleo::python::WellQualityTool);
  quality.title=QObject::tr("测井曲线质量报告");
  quality.description=QObject::tr("先用 query_project 的 well_curves 取得精确 input_ref，再显式选择同井曲线、样点窗、处理状态和 MD 基准。可信 Python 模板只分析所选窗口，报告缺失、异常、覆盖率与统计量；最多16列、8192点。prepare 冻结输入，compute-only 保存私有报告；publish 只能引用原操作并经宿主授权登记成果。不修改原始数据。");
  AiToolSpec correlation;correlation.name=QLatin1String(paleo::python::CorrelationTool);
  correlation.title=QObject::tr("所选数值属性相关性");
  correlation.description=QObject::tr("分析 well_curves 精确引用的同井数值属性窗口：要求至少2列、完全一致的显式 MD 轴，保留逐列单位，不做插值或隐式换算。可信 Python 模板报告 Pearson 相关、共同有效点数及冗余对；结论仅适用于所选窗口，不代表全体积地震属性优选或融合。prepare、compute-only 和引用原操作的 publish 复用现有成果链路。");
  AiToolSpec generated;generated.name=QLatin1String(paleo::python::GeneratedTool);
  generated.title=QObject::tr("受控 Python 计算");
  generated.description=QObject::tr("对 well_curves 精确引用的同井窗口执行有界生成代码，限16列、8192点及16KiB UTF-8源码。必须启用独立生成代码权限并通过当前平台隔离探测；缺项拒绝执行。源码、输入、运行时及策略按内容身份冻结，每次使用新进程。只接收有界指标、表格和说明，宿主补来源证据；脚本的数值与单位声明未经独立重算。compute-only 不登记正式成果；publish 仅引用原操作并单独授权。");
  AiToolSpec table;table.name=QLatin1String(paleo::python::NumericTableTool);
  table.title=QObject::tr("受控数值表计算");
  table.description=QObject::tr("对宿主已确认的 source_tables 数值投影执行有界生成 Python：显式 SourceRef、tableId、投影修订、列和行窗；最多15个数值或布尔列及独立 int64 物理行轴，共16列、8192行。不猜测展示字符串的类型、单位或 null。缺投影或正式来源版本映射时拒绝，不自动升级模型。保留实际 ReadCut 和冻结字节，输出仍为有界 REPORT；不新增 CSV 或图片成果。生成权限、真实隔离、compute-only 与引用原操作 publish 沿用宿主链路；数值结果未经独立重算。");
  AiToolSpec trace;trace.name=QLatin1String(paleo::python::SeismicTraceTool);
  trace.title=QObject::tr("受控地震道窗口计算");
  trace.description=QObject::tr("对精确 SourceRef、体修订及零基文件道序号的一段样点执行有界生成 Python，最多8192点。只支持固定道长、big-endian IEEE32 的 SEG-Y rev1，显式 ms TWT/OWT、时间参考与振幅单位；不猜近邻道、不混用 MD、不整卷加载。宿主按正式来源完整 SHA 建立有界分块认证，冷扫描字节与后续范围认证 I/O 分开记录。输入与实际 ReadCut 冻结，仍只输出 REPORT；生成权限、真实隔离及单独 publish 沿现有链路。数值结果未经独立重算。");
  AiToolSpec outputs;outputs.name=QLatin1String(paleo::python::TableOutputsTool);
  outputs.title=QObject::tr("受控 Python 表格成果");
  outputs.description=QObject::tr("使用显式数值表窗口和冻结的输出列声明，生成报告与单张数值表；最多16列、8192行，宿主编码CSV至多4MiB。两项成果整组验收、一次发布。源码、输入和运行时保留来源证据；生成数值和单位未经宿主独立重算。旧REPORT操作保持原有恢复方式。");
  for(auto spec:{quality,correlation,generated,table,trace,outputs}) {
    spec.effects=paleo::host::Read|paleo::host::PrivateStage|paleo::host::LocalExecute;
    spec.resultSchemaJson=QStringLiteral(R"({"type":"object","properties":{"code":{"type":"string"},"operationId":{"type":"string"},"execution":{"type":"string"},"commitState":{"type":"string"},"summary":{"type":"object"}}})");
    specs.append(std::move(spec));
  }
}
