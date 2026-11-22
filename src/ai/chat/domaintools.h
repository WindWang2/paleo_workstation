// 层：功能
#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <limits>
#include "services/hostcallcontext.h"
#include "services/capabilityspec.h"

// ai/chat — 领域工具描述表（方向51 建立，方向61 接入执行回路）。
//
// 把既有的三条推理能力（tile 分类 / 层位建议 / 测井相）登记为「可枚举、有
// schema 的工具描述」，供聊天面板展示、并由模型点名挑用（tools[] 上送）。
//
// 边界（本文件只管描述与可路由性）：表本身不执行工具——执行编排在
// workflow/aichattoolrunner（方向61），它把这里的表映射到 AiAssistWorkflow /
// WellFaciesService 的既有执行面。分发函数回答「这个调用现在能否路由到
// 入口、缺什么」，不承诺已代用户落任何库。
//
// 方向77 增两只读工程问答工具（query_project / asset_lineage）：同样只登记
// 描述与可路由性——执行面在 workflow/aichattoolrunner 的 runQuery/runLineage
//（catalog 只读查询，零登记副作用）。

struct AiToolParamSpec {
  QString name;
  QString type; // JSON Schema 类型名：string / number / integer / boolean / object / array
  QString description;
  bool required = false;
  // string 入参的封闭词表（方向77）：非空时 schema 出 "enum"、实参校验拒绝
  // 词表外取值——模型少走一轮「取值错→报错→重试」的往返。
  QStringList enumValues;
  // 数值域（G3/W4）：有限时进 schema（minimum/maximum）并在校验时拒绝越界。
  // 缺省 NaN 表示无声明域（执行面仍可能有运行期检查）。
  double minimum = std::numeric_limits<double>::quiet_NaN();
  double maximum = std::numeric_limits<double>::quiet_NaN();
  QJsonObject toJsonSchema() const;
};

struct AiToolSpec {
  QString name;            // 稳定 key（工具名；模型只能按此挑）
  QString title;           // 面板显示名（已翻译）
  QString description;     // 干什么（已翻译）
  QVector<AiToolParamSpec> parameters;
  QString resultSchemaJson; // 原生 runner 结果 schema；保持既有 strict v1 契约
  // 是否有本地执行前提（ORT 在否）——没有就是不可用，UI 明说而不是灰eif都不说。
  bool requiresLocalOrt = false;
  unsigned effects = paleo::host::Read;
  bool legacyAdapter = false;
  CapabilityExposure exposure = CapabilityExposure::Internal;
  unsigned effectsForIntent(const QString &intent) const;
  unsigned effectsForArguments(const QJsonObject &arguments) const;

  QJsonObject toJsonSchema() const;   // 入参 object schema
  QJsonObject toChatFunction() const; // OpenAI tools[].function 形状
  QJsonObject modelResultSchema() const; // 模型边界 envelope，data 引用原生 schema
  // 空串表示通过；非空为验证原因，测井相沿用领域请求的机器错误码。
  // 本函数是共享宽松档（PW-060 口径）：整数参数接受整串可解析的数字文本
  // （"16"），供宿主内部既有调用与已持久化会话兼容；tst_aichat 钉住该行为。
  QString validateParameters(const QJsonObject &arguments) const;
  // 严格外部档（G3/W3）：新外部模型请求经 runner 入口先过本档再分派——
  // integer/number 参数必须是 JSON 原生数值（拒绝 "16" 文本、分数、越界、
  // 非有限值），query_project 主题字段/预算域、seeds 元素整数同档收紧。
  // 返回机器错误码；field 给出首个违规字段（可空）。宽松档兼容路径只保留
  // 在 validateParameters/dispatchAiTool 的直接内部调用，不经 runner 执行。
  QString validateExternalArguments(const QJsonObject &arguments, QString *field = nullptr) const;
};

// 查询、持久操作与受控执行的内置目录；顺序稳定，宿主另行筛选运行时可用性。
QVector<AiToolSpec> builtinAiToolSpecs();
// 按名查表；未登记返回空 spec（name 为空）。
AiToolSpec aiToolSpec(const QString &name);
bool isAiCapabilityTool(const QString &name);

// ---------------------------------------------------------------------------
// 分发（可路由性判定——不执行；执行在 workflow/aichattoolrunner）
// ---------------------------------------------------------------------------
enum class AiToolDispatchStatus {
  Routed,    // Static route exists; runtime provider/model/context readiness is separate.
  NotFound,  // 工具未登记
  Disabled,  // 登记了，但本构建缺依赖（如无 ORT）或实参不合格，入口不可达
  NotImplemented, // 保留值：方向61 后内置三工具均不再返回此态（新工具未接线时用）
};
QString aiToolDispatchLabel(AiToolDispatchStatus status);

struct AiToolDispatch {
  AiToolDispatchStatus status = AiToolDispatchStatus::NotFound;
  AiToolSpec spec;   // 命中的描述（NotFound 时为空）
  QString target;    // 目标入口（"ai/onnxpredictionservice + tileinference" 等）
  QString note;      // 为什么（多为「未接线」/「无 ORT」——诚实写清楚）
};
AiToolDispatch dispatchAiTool(const QString &name,
                              const QJsonObject &arguments);
