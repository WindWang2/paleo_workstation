// 层：功能
#pragma once
#include <QJsonObject>
#include <QString>
#include <QVector>

// ai/chat — 领域工具描述表（方向51）。
//
// 把既有的三条推理能力（tile 分类 / 层位建议 / 测井相）登记为「可枚举、有
// schema 的工具描述」，供聊天面板展示、并在未来由模型点名挑用。
//
// 诚实边界（本方向的红线）：**这只是一个描述表 + 调用分发占位**。
// 表里的工具不会被自动执行，也不假装被自动执行——function calling 闭环
// （模型挑工具 → 真的跑 → 结果回填再续写）递延，见 TODOS.md 登记项。
// 分发函数现在只回答「这个调用现在能否路由到既有入口」，不能执行。

struct AiToolParamSpec {
  QString name;
  QString type; // JSON Schema 类型名：string / number / integer / boolean / object / array
  QString description;
  bool required = false;
  QJsonObject toJsonSchema() const;
};

struct AiToolSpec {
  QString name;            // 稳定 key（工具名；模型只能按此挑）
  QString title;           // 面板显示名（已翻译）
  QString description;     // 干什么（已翻译）
  QVector<AiToolParamSpec> parameters;
  QString resultSchemaJson; // 出参 JSON Schema 片段（便于对接/ad hoc 校验）
  // 是否有本地执行前提（ORT 在否）——没有就是不可用，UI 明说而不是灰eif都不说。
  bool requiresLocalOrt = false;

  QJsonObject toJsonSchema() const;   // 入参 object schema
  QJsonObject toChatFunction() const; // OpenAI tools[].function 形状
  // 实参校验：空串 = 通过；非空 = 缺失/类型不符（面向用户，已翻译）。
  QString validateParameters(const QJsonObject &arguments) const;
};

// 三条既有能力的工具描述（顺序稳定）。
QVector<AiToolSpec> builtinAiToolSpecs();
// 按名查表；未登记返回空 spec（name 为空）。
AiToolSpec aiToolSpec(const QString &name);

// ---------------------------------------------------------------------------
// 分发（占位）
// ---------------------------------------------------------------------------
enum class AiToolDispatchStatus {
  Routed,    // 入口可达（本构建里有对应的实现面）
  NotFound,  // 工具未登记
  Disabled,  // 登记了，但本构建缺依赖（如无 ORT），入口不可达
  NotImplemented, // 登记且在构建内，但还没把钥匙拧到底（递延项）
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
