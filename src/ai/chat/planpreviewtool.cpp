// 层：功能
#include "planpreviewtool.h"
#include "domaintools.h"

namespace paleo::chat {

void appendAgentPlanPreviewTool(QVector<AiToolSpec> &specs) {
  AiToolSpec spec;
  spec.name = QLatin1String(kPlanPreviewTool);
  spec.title = QObject::tr("计算计划提案预检");
  spec.description = QObject::tr(
    "提出一个 v1 计算计划并只做预检：当前可组合的形状是 "
    "constraint.idw（OUTPUT）→ singlefactor.contours（RASTER）的 compute-only "
    "两步图，终端输出单一私有成果。输入为有类型 nodes/edges（IDW 点集用 "
    "inputs.points 的 x/y/z 数值表给出，subinterval_id 必填）。工具只严格"
    "解码并调用既有整图预检，返回规范化参数、执行顺序与首个可定位问题；"
    "不会开始任何计算——执行须由用户在原生审阅面板中修改、重新预检并"
    "明确点击「计算私有成果」。");
  spec.requiresLocalOrt = false;
  spec.effects = paleo::host::Read;
  spec.parameters = {
    {QStringLiteral("title"), QStringLiteral("string"),
     QObject::tr("计划的非执行性标题（≤120 字符，仅展示用）"), false},
    {QStringLiteral("note"), QStringLiteral("string"),
     QObject::tr("计划的非执行性说明（≤500 字符，仅展示用）"), false},
    {QStringLiteral("nodes"), QStringLiteral("array"),
     QObject::tr("节点表（≤32 项）。元素字段：id、capability、contract_version、"
                 "output_port、intent（仅 compute-only）、parameters、inputs；"
                 "封闭词表，未知字段拒绝"), true},
    {QStringLiteral("edges"), QStringLiteral("array"),
     QObject::tr("边表（≤64 项）。元素字段：from、from_port、to、to_port；"
                 "当前仅支持 IDW OUTPUT → contours RASTER"), true},
  };
  spec.resultSchemaJson = QStringLiteral(
    R"({"type":"object","required":["contract","ok","status"],"properties":)"
    R"({"contract":{"const":"paleo.plan-preview.v1"},"ok":{"type":"boolean"},)"
    R"("status":{"const":"not_executed"},"code":{"type":"string"},"field":{"type":"string"},)"
    R"("title":{"type":"string"},"note":{"type":"string"},)"
    R"("digest":{"type":"string"},"terminal":{"type":"string"},)"
    R"("order":{"type":"array","items":{"type":"string"}},)"
    R"("issues":{"type":"array","items":{"type":"object","properties":)"
    R"({"code":{"type":"string"},"field":{"type":"string"},"message":{"type":"string"}}}},)"
    R"("graph":{"type":"object","properties":{"nodes":{"type":"array"},"edges":{"type":"array"}}},)"
    R"("effective_parameters":{"type":"object","additionalProperties":{"type":"object"}}}})");
  specs.append(spec);
}

} // namespace paleo::chat
