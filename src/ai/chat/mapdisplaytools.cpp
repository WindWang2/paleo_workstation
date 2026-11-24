// 层：功能
// ai/chat/mapdisplaytools.cpp — I3：五个地图显示原子的 AiToolSpec 声明。
// 只登记描述与可路由性；执行编排在 workflow/aichattoolrunner_display，
// QGIS 访问在 qgis/agentmapdisplayadapter。schema 域（页大小、透明度
// 0…1、封闭枚举）与运行期校验（workflow/agentmapdisplay.cpp）同值；
// 互斥分支（named_preset XOR semantic_symbol）与 semantic_symbol 内部
// 形状在 AiToolSpec 的通用校验之外，由运行期严格档补齐。
#include "mapdisplaytools.h"
#include "domaintools.h"

namespace paleo::agent::mapdisplay {

bool isMapDisplayTool(const QString &name) {
  return name == QLatin1String(kLayersTool) || name == QLatin1String(kGetTool) ||
         name == QLatin1String(kSetVisibilityTool) ||
         name == QLatin1String(kSetOpacityTool) || name == QLatin1String(kApplyStyleTool);
}

bool isMapDisplayMutationTool(const QString &name) {
  return name == QLatin1String(kSetVisibilityTool) ||
         name == QLatin1String(kSetOpacityTool) || name == QLatin1String(kApplyStyleTool);
}

} // namespace paleo::agent::mapdisplay

void appendAgentMapDisplayTools(QVector<AiToolSpec> &specs) {
  using namespace paleo::agent::mapdisplay;
  const auto text = [](const char *key, bool required) {
    return AiToolParamSpec{QLatin1String(key), QStringLiteral("string"), QString(), required, {}};
  };
  const auto integer = [](const char *key, int minimum, int maximum) {
    AiToolParamSpec spec{QLatin1String(key), QStringLiteral("integer"), QString(), false, {}};
    spec.minimum = minimum;
    spec.maximum = maximum;
    return spec;
  };
  const auto layerId = [text] { return text("layer_id", true); };
  const auto stateToken = [text] {
    auto token = text("state_token", true);
    token.description = QObject::tr(
        "上次 map_layer_get/修改回执返回的显示状态标记；与当前实际显示状态或"
        "宿主世代不符时明确拒绝（重新查询获取新标记，标记不是授权凭证）");
    return token;
  };

  { // paleo.map_layers —— Read：受管图层目录与可观察显示状态。
    AiToolSpec spec;
    spec.name = QString::fromLatin1(kLayersTool);
    spec.title = QObject::tr("列出受管地图图层");
    spec.description = QObject::tr(
        "列出当前工程 manifest 受管图层及其可观察显示状态（是否已实例化、树"
        "勾选、最终可见、透明度、当前具名样式、语义符号）。只读快照：查询不"
        "实例化未加载层、不切换层位、不改任何显示状态；手工添加的非受管图层"
        "不在目录内。按 layer_id 字典序分页，游标绑定当前显示修订。");
    auto horizon = text("horizon", false);
    horizon.description = QObject::tr("按声明的层位精确筛选（空串=不限层的共享层）");
    auto type = text("type", false);
    type.enumValues = {QStringLiteral("vector"), QStringLiteral("raster"),
                       QStringLiteral("mbtiles"), QStringLiteral("annotations")};
    type.description = QObject::tr("按声明类型精确筛选");
    auto pageSize = integer("page_size", 1, 32);
    pageSize.description = QObject::tr("每页条数（1–32，缺省 16）");
    auto cursor = text("cursor", false);
    cursor.description = QObject::tr(
        "上一页返回的续页游标（绑定筛选条件与显示修订；显示状态变化后明确报错）");
    spec.parameters = {horizon, type, pageSize, cursor};
    spec.effects = paleo::host::Read;
    spec.resultSchemaJson = QStringLiteral(
        R"({"type":"object","required":["code"],"properties":{"ok":{"type":"boolean"},"code":{"type":"string"},"display_revision":{"type":"string"},"total":{"type":"integer"},"returned":{"type":"integer"},"has_more":{"type":"boolean"},"next_cursor":{"type":"string"},"layers":{"type":"array"},"note":{"type":"string"}}})");
    specs.append(spec);
  }

  { // paleo.map_layer_get —— Read：单层显示状态与适用选项。
    AiToolSpec spec;
    spec.name = QString::fromLatin1(kGetTool);
    spec.title = QObject::tr("查询图层显示状态");
    spec.description = QObject::tr(
        "按精确 layer_id 返回该受管图层的显示状态：树勾选与最终可见（含父组"
        "隐藏原因）、透明度、当前具名样式、语义符号、是否编辑中；并列出该层"
        "当前适用的具名预设与语义符号选项（几何适配）。未实例化层如实标记"
        "（不触发加载）。不返回 QGIS 内部 ID、源路径或完整 QML。");
    spec.parameters = {layerId()};
    spec.effects = paleo::host::Read;
    spec.resultSchemaJson = QStringLiteral(
        R"({"type":"object","required":["code","layer_id"],"properties":{"ok":{"type":"boolean"},"code":{"type":"string"},"layer_id":{"type":"string"},"materialized":{"type":"boolean"},"state_token":{"type":"string"},"canvas_present":{"type":"boolean"},"display_revision":{"type":"string"},"checked":{"type":"boolean"},"effective_visible":{"type":"boolean"},"visibility_note":{"type":"string"},"opacity":{"type":"number"},"current_style":{"type":"string"},"symbol_semantics":{"type":"object"},"editing":{"type":"boolean"},"presets":{"type":"array"},"semantic_families":{"type":"array"},"note":{"type":"string"}}})");
    specs.append(spec);
  }

  { // paleo.map_layer_set_visibility —— Read|Presentation。
    AiToolSpec spec;
    spec.name = QString::fromLatin1(kSetVisibilityTool);
    spec.title = QObject::tr("设置图层可见性");
    spec.description = QObject::tr(
        "设置当前已实例化受管图层的树勾选状态（JSON 布尔）。回读自身勾选与"
        "最终可见：父组隐藏时勾选可为 true 而最终可见为 false（不代为打开父"
        "组或其他图层）。同值设置返回 no_change。修改只进入当前工程的原生"
        "显示状态（未保存工程文件），不落 Catalog 版本、不发布。");
    auto visible = AiToolParamSpec{QStringLiteral("visible"), QStringLiteral("boolean"),
                                   QObject::tr("目标勾选状态（JSON 原生布尔）"), true, {}};
    spec.parameters = {layerId(), visible, stateToken()};
    spec.effects = paleo::host::Read | paleo::host::Presentation;
    spec.resultSchemaJson = QStringLiteral(
        R"({"type":"object","required":["code","layer_id"],"properties":{"ok":{"type":"boolean"},"code":{"type":"string"},"layer_id":{"type":"string"},"requested":{"type":"boolean"},"checked":{"type":"boolean"},"effective_visible":{"type":"boolean"},"visibility_note":{"type":"string"},"mutated":{"type":"boolean"},"state_token":{"type":"string"},"display_revision":{"type":"string"},"persistence":{"type":"string"},"canvas_present":{"type":"boolean"},"note":{"type":"string"}}})");
    specs.append(spec);
  }

  { // paleo.map_layer_set_opacity —— Read|Presentation。
    AiToolSpec spec;
    spec.name = QString::fromLatin1(kSetOpacityTool);
    spec.title = QObject::tr("设置图层透明度");
    spec.description = QObject::tr(
        "对支持的矢量/栅格受管图层设置原生层透明度（0–1，JSON 原生数值，拒"
        "绝越界/NaN/文本），并回读实际值。只改显示呈现：不改像元值、要素属"
        "性、源引用或地图范围。同值返回 no_change；修改未保存为工程文件。");
    auto opacity = AiToolParamSpec{QStringLiteral("opacity"), QStringLiteral("number"),
                                   QObject::tr("目标透明度（0=全透明，1=不透明）"), true, {}};
    opacity.minimum = 0.0;
    opacity.maximum = 1.0;
    spec.parameters = {layerId(), opacity, stateToken()};
    spec.effects = paleo::host::Read | paleo::host::Presentation;
    spec.resultSchemaJson = QStringLiteral(
        R"({"type":"object","required":["code","layer_id"],"properties":{"ok":{"type":"boolean"},"code":{"type":"string"},"layer_id":{"type":"string"},"requested":{"type":"number"},"opacity":{"type":"number"},"mutated":{"type":"boolean"},"state_token":{"type":"string"},"display_revision":{"type":"string"},"persistence":{"type":"string"},"canvas_present":{"type":"boolean"},"note":{"type":"string"}}})");
    specs.append(spec);
  }

  { // paleo.map_layer_apply_style —— Read|Presentation。
    AiToolSpec spec;
    spec.name = QString::fromLatin1(kApplyStyleTool);
    spec.title = QObject::tr("应用图层样式");
    spec.description = QObject::tr(
        "对受管矢量/栅格图层应用受限样式：具名预设（当前图层样式管理器中已"
        "存在的名字，精确选项）或语义符号（GeoPatterns 词表 family/id，按几何"
        "适配）。两者互斥，必须恰好给一个。不接收文件路径、QML/SVG 内容或表"
        "达式。回读 currentStyle/renderer 语义锚；不适用时如实拒绝且零副作"
        "用。修改未保存为工程文件。");
    auto preset = text("named_preset", false);
    preset.description = QObject::tr("图层已存在的具名预设名（与 semantic_symbol 互斥）");
    auto semantic = AiToolParamSpec{QStringLiteral("semantic_symbol"), QStringLiteral("object"),
                                    QString(), false, {}};
    semantic.description = QObject::tr(
        "语义符号 {family: lithology|facies|line, id: 词表稳定 id}（与 "
        "named_preset 互斥；几何不匹配会被拒绝）");
    spec.parameters = {layerId(), stateToken(), preset, semantic};
    spec.effects = paleo::host::Read | paleo::host::Presentation;
    spec.resultSchemaJson = QStringLiteral(
        R"({"type":"object","required":["code","layer_id"],"properties":{"ok":{"type":"boolean"},"code":{"type":"string"},"layer_id":{"type":"string"},"applied_kind":{"type":"string"},"current_style":{"type":"string"},"symbol_semantics":{"type":"object"},"mutated":{"type":"boolean"},"state_token":{"type":"string"},"display_revision":{"type":"string"},"persistence":{"type":"string"},"canvas_present":{"type":"boolean"},"note":{"type":"string"}}})");
    specs.append(spec);
  }
}
