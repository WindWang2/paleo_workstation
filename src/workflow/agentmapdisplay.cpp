// 层：功能
// workflow/agentmapdisplay.cpp — I3：显示原子的严格外部档校验。
// AiToolSpec 通用档（必填/类型/枚举/数值域/未知字段拒绝）已在 runner
// 入口先行；这里只补通用档表达不了的约束：互斥分支、semantic_symbol
// 内部形状、标识字段长度界（模型输入一律按不可信数据做有界处理）。
#include "agentmapdisplay.h"
#include "ai/chat/mapdisplaytools.h"

namespace paleo::agent::mapdisplay {
namespace {
// 标识字段长度界：manifest layerId/预设名按词表量级给界，超出直接拒绝
//（不截断——静默截断会让模型以为操作了另一个对象）。
constexpr int kMaxLayerIdChars = 200;
constexpr int kMaxStateTokenChars = 64;
constexpr int kMaxPresetNameChars = 200;
constexpr int kMaxSymbolIdChars = 64;
constexpr int kMaxCursorChars = 160;

bool boundedNonEmpty(const QString &value, int maxChars) {
  return !value.isEmpty() && value.size() <= maxChars;
}
} // namespace

QString validateMapDisplayArguments(const QString &name, const QJsonObject &args,
                                    QString *field) {
  if (field)
    *field = QString();
  const auto fail = [field](const QString &code, const QString &name2) {
    if (field)
      *field = name2;
    return code;
  };
  const bool requiresLayerId = name != QLatin1String(kLayersTool);
  if (requiresLayerId) {
    const auto layerId = args.value(QStringLiteral("layer_id")).toString();
    if (!boundedNonEmpty(layerId, kMaxLayerIdChars))
      return fail(QStringLiteral("invalid_layer_id"), QStringLiteral("layer_id"));
  }
  if (isMapDisplayMutationTool(name)) {
    const auto token = args.value(QStringLiteral("state_token")).toString();
    if (!boundedNonEmpty(token, kMaxStateTokenChars))
      return fail(QStringLiteral("invalid_state_token"), QStringLiteral("state_token"));
  }
  if (name == QLatin1String(kLayersTool)) {
    const auto cursor = args.value(QStringLiteral("cursor")).toString();
    if (!cursor.isEmpty() && cursor.size() > kMaxCursorChars)
      return fail(QStringLiteral("invalid_cursor"), QStringLiteral("cursor"));
    const auto horizon = args.value(QStringLiteral("horizon")).toString();
    if (horizon.size() > kMaxLayerIdChars)
      return fail(QStringLiteral("invalid_horizon"), QStringLiteral("horizon"));
    return {};
  }
  if (name == QLatin1String(kApplyStyleTool)) {
    const bool hasPreset = args.contains(QStringLiteral("named_preset"));
    const bool hasSemantic = args.contains(QStringLiteral("semantic_symbol"));
    if (hasPreset && hasSemantic)
      return fail(QStringLiteral("style_target_conflict"), QStringLiteral("named_preset"));
    if (!hasPreset && !hasSemantic)
      return fail(QStringLiteral("style_target_required"), QStringLiteral("named_preset"));
    if (hasPreset) {
      const auto preset = args.value(QStringLiteral("named_preset")).toString();
      if (!boundedNonEmpty(preset, kMaxPresetNameChars))
        return fail(QStringLiteral("invalid_preset_name"), QStringLiteral("named_preset"));
      return {};
    }
    const auto symbol = args.value(QStringLiteral("semantic_symbol")).toObject();
    // 对象内只认 family/id 两键：其余键拒绝（与 seeds 的
    // unsupported_seed_field 同严格度——嵌套载荷同样是模型输入）。
    for (auto it = symbol.begin(); it != symbol.end(); ++it)
      if (it.key() != QLatin1String("family") && it.key() != QLatin1String("id"))
        return fail(QStringLiteral("unsupported_symbol_field"), it.key());
    const auto family = symbol.value(QStringLiteral("family")).toString();
    const auto id = symbol.value(QStringLiteral("id")).toString();
    if (family != QLatin1String("lithology") && family != QLatin1String("facies") &&
        family != QLatin1String("line"))
      return fail(QStringLiteral("invalid_semantic_family"), QStringLiteral("semantic_symbol"));
    if (!boundedNonEmpty(id, kMaxSymbolIdChars))
      return fail(QStringLiteral("invalid_semantic_id"), QStringLiteral("semantic_symbol"));
    // 词表存在性/几何适配由 adapter 按 GeoPatterns 单一权威判定（此处
    // 不复制词表，防双源漂移）。
  }
  return {};
}

} // namespace paleo::agent::mapdisplay
