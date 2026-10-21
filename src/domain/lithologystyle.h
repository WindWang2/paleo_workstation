// 层：数据
#pragma once

#include <QColor>
#include <QString>
#include <QVector>

// 岩性道「宽度 + 纹理 + 颜色 + 高度粒度」样式规则（纯函数，可单测）。
//
// - 宽度：按岩性分级占道宽的比例（地质惯例：粒度越粗越宽，泥岩最窄、砾岩满宽）。
//   规则表可配置：setWidthRules() 覆盖全局默认；关键词最长匹配，等长取名称中
//   位置靠后者（「泥质粉砂岩」核心词是粉砂岩，「粉砂质泥岩」核心词是泥岩）。
// - 颜色：从岩性解释名中解析颜色词（须带「色」字，如「黑色砂岩」「灰绿色泥岩」），
//   避免把「灰岩」「白云岩」误当颜色；解析不到时沿用区间自带颜色属性。
// - 高度粒度：竖向以 quantum（深度单位）为一个高度粒度；层厚 35、粒度 10 即
//   3.5 个粒度。粒度像素高 = quantum × 当前比例尺 px/m，保证与深度道严格对齐。
namespace WellComposite::LithologyStyle
{
struct WidthRule
{
  QString keyword;  // 岩性关键词（如 "泥岩"、"细砂岩"）
  double fraction;  // 占道宽比例 (0,1]
};

QVector<WidthRule> defaultWidthRules();
QVector<WidthRule> widthRules();
void setWidthRules(const QVector<WidthRule> &rules); // 空表 = 恢复默认
double defaultWidthFraction();                         // 未识别岩性的宽度（3/5）

// 岩性名 → 宽度比例（rules 为空时用当前全局表）
double widthFraction(const QString &lithoName, const QVector<WidthRule> &rules = {});
// 命中的规则关键词（测试/图例用）；未命中返回空串
QString matchedWidthKeyword(const QString &lithoName, const QVector<WidthRule> &rules = {});

// 从岩性名解析颜色词；ok 为 false 时返回无效色
QColor colorFromName(const QString &lithoName, QString *colorWord = nullptr);
// 灰调化：保留色相，HSL 饱和度封顶 kMutedMaxSaturation、明度夹到 [kMutedMinLightness, kMutedMaxLightness]；
// 近中性色（饱和度 < kNeutralSaturation，即黑/白/灰）原样返回。红 → 灰里带一点红，绿 → 灰绿。
inline constexpr double kMutedMaxSaturation = 0.18;
inline constexpr double kMutedMinLightness = 0.28;
inline constexpr double kMutedMaxLightness = 0.86;
inline constexpr double kNeutralSaturation = 0.08;
QColor muteColor(const QColor &c);
// 最终填充色：名称颜色词优先，其次区间颜色属性 fallback；两者都经 muteColor 灰调化
QColor resolveFillColor(const QString &lithoName, const QColor &fallback);

// 高度粒度换算：厚度 / 粒度（quantum<=0 返回 0）
double heightInQuanta(double thickness, double quantum);
} // namespace WellComposite::LithologyStyle
