// 层：功能
#pragma once
#include <QString>
#include <QVector>

struct AiToolSpec;

// ai/chat/navigationtools.h — I4：语义导航原子（井/地震/地图的业务身份选择、
// 主画布定位与原生井资料/地震测线打开）的工具名单一权威。
//
// 六个工具按精确名称分派；与 I3 的 paleo.map_layer*（图层显示族）严格隔离
// ——isNavigationTool 用精确集合判定，不以 "paleo.map_" 前缀吞掉他人路由；
// I3 同样只列它的精确五名。工具契约/校验/分派/readiness 共用本表：新增
// 注册不得让未实现能力进入模型目录。
//
// 语义边界（任务书 I4）：临时选择/视图导航，效果恒为宿主 Read——不改
// host::Presentation 全局写门，不改 renderer/可见性/样式（I3 域），不写
// Catalog 业务状态、不自动保存工程。固定版本井资料/地震测线的完整性
// 语义见 services/previewdoc.h 的 per-request SHA 策略。

namespace paleo::agent::navigation {

inline constexpr auto kContract = "paleo.navigation.v1";
inline constexpr auto kStateTool = "paleo.navigation_state";
inline constexpr auto kSelectWellsTool = "paleo.select_wells";
inline constexpr auto kClearWellSelectionTool = "paleo.clear_well_selection";
inline constexpr auto kMapLocateTool = "paleo.map_locate";
inline constexpr auto kOpenWellViewTool = "paleo.open_well_view";
inline constexpr auto kOpenSeismicLineTool = "paleo.open_seismic_line";

// 入参集合/长度/条目上限（严格外部档共用；超出在任何打开/选择动作前拒绝）。
inline constexpr int kMaxWellIds = 64;       // select_wells / map_locate(well_ids)
inline constexpr int kMaxIdLength = 128;     // 单个业务 ID 字符上限
inline constexpr int kMaxTokenLength = 256;  // 宿主生成选择修订 token 上限
// 地震测线号域（有界 JSON integer；真实存在性由 survey 几何/解码结果判定，
// 不假设范围内每个整数都存在）。
inline constexpr int kMinLineNumber = -10000000;
inline constexpr int kMaxLineNumber = 10000000;

// 精确名称集合：true 仅对上述六名成立（"paleo.map_layers" 等 I3 名称与任何
// 前缀变体都为 false）。
bool isNavigationTool(const QString &name);
// open_well_view 当前支持的井资料类型（显式表；其他类型按表拒绝，不默认
// 打开 Office/外部网址/解释编辑页/任意资产）。
bool isSupportedWellViewAssetType(const QString &assetType);

} // namespace paleo::agent::navigation

// 追加六个导航原子到内置工具表（builtinAiToolSpecs 收尾处调用一次）。
void appendAgentNavigationTools(QVector<AiToolSpec> &specs);
