// 层：功能
#pragma once
#include <QJsonObject>
#include <QString>
#include <QVector>

struct AiToolSpec;
class PaleoOnnxService;

// paleo.unit_onnx_predict 的工具表工厂与参数/就绪校验（实现见
// ai/chat/unitonnxtools.cpp；工具经 isAiCapabilityTool → runCapability 的
// 通用生产链执行，不走专属 runner 分支）。
void appendAgentUnitOnnxTools(QVector<AiToolSpec> &specs);
bool isUnitOnnxTool(const QString &name);
// 宽松/严格两档共用的形状校验：返回机器码，空串为通过。
QString validateUnitOnnxToolArguments(const QJsonObject &arguments);
// readiness 委托：prepare/新 compute 需要本机 ORT 与推理服务；publish、终态
// 重放与列表广播不受 ORT/模型安装状态牵连（effects/scope 校验仍在通用段）。
QString unitOnnxReadinessCode(PaleoOnnxService *onnx, const QJsonObject &args, bool listing);
