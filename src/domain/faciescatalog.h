// 层：数据
#pragma once
#include <QVariantList>
#include <QVariantMap>
namespace FaciesCatalog {
QVariantList library();
QVariantList defaults();
QVariantMap find(const QVariantList &schema, const QVariant &code);
QVariantMap attributes(const QVariantList &schema, const QVariant &code);
QString resourcePath(const QString &texture);
// 填充用可平铺纹理：图签卡（带文字的 strat_*/tex_* 图例插图）映射到
// <base>_fill.svg 变体；本身就是 _fill 名则原样解析；无变体返回空
// （调用侧退化为纯色填充 + label 标注——相名不嵌在填充图案里）。
QString fillPath(const QString &texture);
} // namespace FaciesCatalog
