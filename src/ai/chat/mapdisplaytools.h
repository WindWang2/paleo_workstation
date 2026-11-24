// 层：功能
#pragma once
#include <QString>
#include <QVector>

struct AiToolSpec;

// ai/chat/mapdisplaytools.h — I3：地图显示原子（受管图层目录/显示状态、
// 可见性、透明度、受限样式）的工具名单一权威。
//
// 五个工具按精确名称分派；与 I4 的 paleo.map_locate（视野/选择/预览，本
// 任务不实现）严格隔离——isMapDisplayTool 用精确集合判定，绝不以
// "paleo.map_" 前缀吞掉他人路由。工具契约/校验/分派/readiness 共用本表：
// 新增注册不得让未实现能力进入模型目录。

namespace paleo::agent::mapdisplay {

inline constexpr auto kLayersTool = "paleo.map_layers";
inline constexpr auto kGetTool = "paleo.map_layer_get";
inline constexpr auto kSetVisibilityTool = "paleo.map_layer_set_visibility";
inline constexpr auto kSetOpacityTool = "paleo.map_layer_set_opacity";
inline constexpr auto kApplyStyleTool = "paleo.map_layer_apply_style";

// 精确名称集合：true 仅对上述五名成立（"paleo.map_locate" 等 I4 名称与
// 任何前缀变体都为 false）。
bool isMapDisplayTool(const QString &name);

// 查询族（Read）与显示修改族（Read|Presentation）的静态划分——效果随
// 工具固定，不随实参变化。
bool isMapDisplayMutationTool(const QString &name);

} // namespace paleo::agent::mapdisplay

// 追加五个显示原子到内置工具表（builtinAiToolSpecs 收尾处调用一次）。
void appendAgentMapDisplayTools(QVector<AiToolSpec> &specs);
