// 层：功能
#pragma once
#include <QJsonObject>
#include <QString>

namespace paleo::agent::mapdisplay {

// workflow/agentmapdisplay.h — I3：显示原子的功能层共享件。
//
// 访问接口（Access）声明在 qgis/agentmapdisplayadapter.h：生产实现属
// QGIS 封装层，接口面只用纯 Qt 类型，功能层经组装根注入后调用（本层
// 不出现 Qgs 符号——qgis-spread 闸，#472）。
//
// 本头只放严格外部档校验的共享入口：AiToolSpec 通用档（必填/类型/枚举/
// 数值域/未知字段拒绝）之外的互斥分支、semantic_symbol 内部形状与标识
// 字段长度界。runner 入口与 readiness 共用，不复制第二份口径。

// 返回空串=通过；非空=机器错误码；field 给首个违规字段。
QString validateMapDisplayArguments(const QString &name, const QJsonObject &args,
                                    QString *field);

} // namespace paleo::agent::mapdisplay
